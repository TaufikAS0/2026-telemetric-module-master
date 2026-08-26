import http from "node:http";
import { pathToFileURL } from "node:url";
import { TmmState } from "./tmm-state.mjs";

const dashboard = `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>TMM Desktop Simulator</title>
  <style>
    :root { color-scheme: dark; font-family: Inter, Segoe UI, sans-serif; background:#031019; color:#dffaff; }
    * { box-sizing:border-box; }
    body { margin:0; min-height:100vh; background:radial-gradient(circle at 80% 0,#07324a 0,transparent 34%),#031019; }
    main { width:min(1080px,calc(100% - 32px)); margin:auto; padding:32px 0 48px; }
    header,.panel,.module { border:1px solid #12637b; background:rgba(3,22,33,.88); box-shadow:0 12px 38px #0008; }
    header { padding:24px; border-radius:18px; display:flex; align-items:center; justify-content:space-between; gap:20px; }
    h1,h2,p { margin-top:0; } h1 { letter-spacing:.08em; margin-bottom:6px; } h2 { font-size:15px; color:#53e4ff; letter-spacing:.08em; }
    .badge { padding:8px 12px; border-radius:999px; color:#39f58a; border:1px solid #1b9d5d; background:#06301f; font-weight:700; }
    .warning { margin:18px 0; color:#ffd36a; border-left:3px solid #ffb020; padding:12px 14px; background:#2e210c99; }
    .grid { display:grid; grid-template-columns:repeat(3,1fr); gap:14px; }
    .panel { margin-top:16px; border-radius:14px; padding:18px; }
    .module { border-radius:12px; padding:16px; }
    .module.offline { border-color:#b54b4b; }
    .row { display:flex; justify-content:space-between; gap:14px; margin:7px 0; color:#9ecbd5; }
    strong { color:#fff; } button { cursor:pointer; border:1px solid #23bddb; color:#031019; background:#42def8; font-weight:800; border-radius:8px; padding:11px 15px; }
    button.secondary { color:#dffaff; background:transparent; }
    .actions { display:flex; gap:10px; flex-wrap:wrap; }
    #events { max-height:250px; overflow:auto; font:12px Consolas,monospace; color:#9fd8e5; }
    @media(max-width:760px){ .grid{grid-template-columns:1fr;} header{align-items:flex-start;flex-direction:column;} }
  </style>
</head>
<body><main>
  <header><div><h1>TELEMETRIC MODULE MASTER</h1><p>Phase 0 · Hardware-neutral desktop simulator</p></div><span class="badge" id="health">LOADING</span></header>
  <div class="warning">Simulation evidence only. This dashboard does not prove PCB, field-bus, or electrical compatibility.</div>
  <section class="panel"><h2>VIRTUAL MODULES</h2><div class="grid" id="modules"></div></section>
  <section class="panel"><h2>CONTROL</h2><div class="actions"><button id="poll">POLL ALL MODULES</button><button class="secondary" id="refresh">REFRESH STATUS</button></div></section>
  <section class="panel"><h2>EVENTS</h2><div id="events"></div></section>
</main>
<script>
async function json(url, options){ const response=await fetch(url,options); const body=await response.json(); if(!response.ok) throw new Error(body.error||response.statusText); return body; }
async function setState(id,online){ await json('/api/modules/'+encodeURIComponent(id)+'/state',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({online})}); await refresh(); }
async function refresh(){
  const [health,modules,events]=await Promise.all([json('/api/health'),json('/api/modules'),json('/api/events')]);
  const badge=document.querySelector('#health'); badge.textContent=health.status.toUpperCase(); badge.style.color=health.status==='healthy'?'#39f58a':'#ffd36a';
  document.querySelector('#modules').innerHTML=modules.map(module=>'<article class="module '+(module.online?'':'offline')+'"><h2>'+module.productCode+' · '+module.id+'</h2><div class="row"><span>Status</span><strong>'+(module.online?'ONLINE':'OFFLINE')+'</strong></div><div class="row"><span>Poll count</span><strong>'+module.pollCount+'</strong></div><div class="row"><span>Value</span><strong>'+module.simulatedValue+'</strong></div><div class="row"><span>Last seen</span><strong>'+(module.lastSeen||'Never')+'</strong></div><button class="secondary" data-id="'+encodeURIComponent(module.id)+'" data-online="'+(!module.online)+'">MARK '+(module.online?'OFFLINE':'ONLINE')+'</button></article>').join('');
  document.querySelectorAll('#modules button[data-id]').forEach(button=>{ button.onclick=()=>setState(decodeURIComponent(button.dataset.id),button.dataset.online==='true'); });
  document.querySelector('#events').innerHTML=events.map(event=>'<div>['+event.timestamp+'] '+event.type+' · '+event.message+'</div>').join('');
}
document.querySelector('#poll').onclick=async()=>{ await json('/api/poll',{method:'POST'}); await refresh(); };
document.querySelector('#refresh').onclick=refresh;
refresh();
</script></body></html>`;

function sendJson(response, statusCode, body) {
  response.writeHead(statusCode, {
    "content-type": "application/json; charset=utf-8",
    "cache-control": "no-store"
  });
  response.end(JSON.stringify(body));
}

async function readJson(request) {
  let body = "";
  for await (const chunk of request) {
    body += chunk;
    if (body.length > 16_384) throw new Error("body_too_large");
  }
  if (!body) return {};
  try {
    return JSON.parse(body);
  } catch {
    throw new Error("invalid_json");
  }
}

export function createTmmServer({ state = new TmmState() } = {}) {
  return http.createServer(async (request, response) => {
    const url = new URL(request.url, "http://localhost");
    try {
      if (request.method === "GET" && url.pathname === "/") {
        response.writeHead(200, { "content-type": "text/html; charset=utf-8" });
        response.end(dashboard);
        return;
      }
      if (request.method === "GET" && url.pathname === "/api/health") return sendJson(response, 200, state.health());
      if (request.method === "GET" && url.pathname === "/api/modules") return sendJson(response, 200, state.listModules());
      if (request.method === "GET" && url.pathname === "/api/events") return sendJson(response, 200, state.listEvents());
      if (request.method === "POST" && url.pathname === "/api/poll") return sendJson(response, 200, state.pollAll());

      const stateMatch = url.pathname.match(/^\/api\/modules\/([^/]+)\/state$/);
      if (request.method === "POST" && stateMatch) {
        const input = await readJson(request);
        if (typeof input.online !== "boolean") return sendJson(response, 400, { error: "online_must_be_boolean" });
        const module = state.setModuleState(decodeURIComponent(stateMatch[1]), input.online);
        return module ? sendJson(response, 200, module) : sendJson(response, 404, { error: "module_not_found" });
      }
      return sendJson(response, 404, { error: "route_not_found" });
    } catch (error) {
      const status = error.message === "body_too_large" ? 413 : 400;
      return sendJson(response, status, { error: error.message });
    }
  });
}

const isMain = process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href;
if (isMain) {
  const host = process.env.TMM_SIM_HOST || "127.0.0.1";
  const port = Number(process.env.TMM_SIM_PORT || 8090);
  const server = createTmmServer();
  server.listen(port, host, () => {
    console.log(`TMM desktop simulator listening at http://${host}:${port}`);
  });
}
