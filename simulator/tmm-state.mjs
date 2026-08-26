const MAX_EVENTS = 100;

export class TmmState {
  constructor({ clock = () => new Date(), version = "0.0.1-simulator" } = {}) {
    this.clock = clock;
    this.version = version;
    this.startedAt = this.clock();
    this.pollSequence = 0;
    this.events = [];
    this.modules = [
      this.#createModule("TMB-X-001", "TMB-X", "Modbus Expansion"),
      this.#createModule("TDI-X-001", "TDI-X", "Digital Input Expansion"),
      this.#createModule("TPS-X-001", "TPS-X", "Pulse to Serial Expansion")
    ];
    this.#record("system.started", "TMM desktop simulator started");
  }

  #createModule(id, productCode, name) {
    return {
      id,
      productCode,
      name,
      online: true,
      lastSeen: null,
      pollCount: 0,
      simulatedValue: 0
    };
  }

  #record(type, message, moduleId = null) {
    this.events.unshift({
      id: `${this.clock().getTime()}-${this.events.length + 1}`,
      timestamp: this.clock().toISOString(),
      type,
      moduleId,
      message
    });
    this.events.length = Math.min(this.events.length, MAX_EVENTS);
  }

  health() {
    const onlineModules = this.modules.filter((module) => module.online).length;
    return {
      productCode: "TMM",
      productName: "Telemetric Module Master",
      version: this.version,
      mode: "desktop-simulator",
      status: onlineModules === this.modules.length ? "healthy" : "degraded",
      uptimeSeconds: Math.max(0, Math.floor((this.clock() - this.startedAt) / 1000)),
      modules: {
        total: this.modules.length,
        online: onlineModules,
        offline: this.modules.length - onlineModules
      },
      evidenceLevel: "software-simulation-only"
    };
  }

  listModules() {
    return structuredClone(this.modules);
  }

  listEvents() {
    return structuredClone(this.events);
  }

  pollAll() {
    this.pollSequence += 1;
    const timestamp = this.clock().toISOString();
    const results = this.modules.map((module, index) => {
      if (!module.online) {
        this.#record("module.poll_failed", `${module.id} is offline`, module.id);
        return { id: module.id, ok: false, reason: "offline" };
      }
      module.lastSeen = timestamp;
      module.pollCount += 1;
      module.simulatedValue = this.pollSequence * 10 + index;
      this.#record("module.polled", `${module.id} responded`, module.id);
      return { id: module.id, ok: true, value: module.simulatedValue };
    });
    return { sequence: this.pollSequence, timestamp, results };
  }

  setModuleState(id, online) {
    const module = this.modules.find((candidate) => candidate.id === id);
    if (!module) return null;
    module.online = online;
    if (online) module.lastSeen = this.clock().toISOString();
    this.#record(
      online ? "module.online" : "module.offline",
      `${module.id} marked ${online ? "online" : "offline"}`,
      module.id
    );
    return structuredClone(module);
  }
}

