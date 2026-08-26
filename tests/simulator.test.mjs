import assert from "node:assert/strict";
import { after, before, test } from "node:test";
import { Script } from "node:vm";
import { createTmmServer } from "../simulator/server.mjs";
import { TmmState } from "../simulator/tmm-state.mjs";

let server;
let baseUrl;

before(async () => {
  server = createTmmServer();
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  baseUrl = `http://127.0.0.1:${server.address().port}`;
});

after(async () => {
  await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
});

async function request(path, options) {
  const response = await fetch(`${baseUrl}${path}`, options);
  return { response, body: await response.json() };
}

test("health identifies TMM and simulation evidence level", async () => {
  const { response, body } = await request("/api/health");
  assert.equal(response.status, 200);
  assert.equal(body.productCode, "TMM");
  assert.equal(body.mode, "desktop-simulator");
  assert.equal(body.evidenceLevel, "software-simulation-only");
  assert.deepEqual(body.modules, { total: 3, online: 3, offline: 0 });
});

test("dashboard loads and its inline script has valid JavaScript syntax", async () => {
  const response = await fetch(`${baseUrl}/`);
  const html = await response.text();
  assert.equal(response.status, 200);
  assert.match(html, /TELEMETRIC MODULE MASTER/);
  const inlineScript = html.match(/<script>([\s\S]*?)<\/script>/)?.[1];
  assert.ok(inlineScript, "dashboard inline script should exist");
  assert.doesNotThrow(() => new Script(inlineScript));
});

test("module IDs are unique", async () => {
  const { body } = await request("/api/modules");
  assert.equal(body.length, 3);
  assert.equal(new Set(body.map((module) => module.id)).size, body.length);
});

test("polling updates online modules and records events", async () => {
  const poll = await request("/api/poll", { method: "POST" });
  assert.equal(poll.response.status, 200);
  assert.equal(poll.body.results.filter((result) => result.ok).length, 3);

  const modules = await request("/api/modules");
  assert.ok(modules.body.every((module) => module.pollCount === 1));
  assert.ok(modules.body.every((module) => module.lastSeen));

  const events = await request("/api/events");
  assert.ok(events.body.filter((event) => event.type === "module.polled").length >= 3);
});

test("module can transition offline and health degrades", async () => {
  const changed = await request("/api/modules/TDI-X-001/state", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ online: false })
  });
  assert.equal(changed.response.status, 200);
  assert.equal(changed.body.online, false);

  const health = await request("/api/health");
  assert.equal(health.body.status, "degraded");
  assert.equal(health.body.modules.offline, 1);

  const poll = await request("/api/poll", { method: "POST" });
  assert.deepEqual(poll.body.results.find((result) => result.id === "TDI-X-001"), {
    id: "TDI-X-001",
    ok: false,
    reason: "offline"
  });
});

test("invalid input and missing resources return clear errors", async () => {
  const invalidType = await request("/api/modules/TDI-X-001/state", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ online: "yes" })
  });
  assert.equal(invalidType.response.status, 400);
  assert.equal(invalidType.body.error, "online_must_be_boolean");

  const invalidJson = await request("/api/modules/TDI-X-001/state", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: "{"
  });
  assert.equal(invalidJson.response.status, 400);
  assert.equal(invalidJson.body.error, "invalid_json");

  const missingModule = await request("/api/modules/UNKNOWN/state", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ online: true })
  });
  assert.equal(missingModule.response.status, 404);

  const missingRoute = await request("/api/missing");
  assert.equal(missingRoute.response.status, 404);
});

test("event history is bounded", () => {
  let milliseconds = 0;
  const state = new TmmState({ clock: () => new Date(milliseconds++) });
  for (let index = 0; index < 60; index += 1) state.pollAll();
  assert.equal(state.listEvents().length, 100);
});
