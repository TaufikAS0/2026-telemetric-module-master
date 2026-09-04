// QC summary contract mirror (desktop simulator, decision D-027).
//
// Mirrors the firmware's `qcSummary` roll-up rules in
// firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0.ino for the six mandatory QC
// items. Pure logic over the SAME /api/status shapes the firmware emits —
// no hardware access, never an automated hardware PASS.
//
// States (exactly the firmware's vocabulary):
//   "untested" belum ada bukti live;
//   "testing"  tes sedang berjalan / sampel sedang diambil;
//   "pass"     bukti live otomatis lengkap dan segar;
//   "fail"     tes berakhir gagal (error tercatat, tidak sedang jalan);
//   "manual"   butuh keputusan operator (observasi manusia) — PASS tetap
//              hanya berasal dari keputusan operator di portal QC.
// Items marked manualRequired=true can never leave "manual" by automation.

export const QC_ITEM_IDS = Object.freeze(["led", "buttons", "aht10", "ethernet", "sd", "rs485"]);
export const QC_STATES = Object.freeze(["untested", "testing", "pass", "fail", "manual"]);

export const QC_MANUAL_REQUIRED = Object.freeze({ led: true, buttons: true, aht10: false, ethernet: false, sd: false, rs485: false });

// AHT10 evidence is considered fresh within the firmware's staleness window.
export const AHT10_STALE_AFTER_MS = 5000;

export function qcItemState(item, status) {
  const s = status || {};
  switch (item) {
    case "led": {
      const t = s.ledTest || {};
      if (t.running) return "testing";
      if (t.lastError) return "fail";
      if ((t.cyclesCompleted || 0) >= 1) return "manual";
      return "untested";
    }
    case "buttons": {
      const b = s.buttons || {};
      if (b.boot?.fullCycleConfirmed && b.changeDisplay?.fullCycleConfirmed) return "manual";
      return "untested";
    }
    case "aht10": {
      const a = s.aht10 || {};
      // The firmware reports "testing" while a measurement cycle is in flight
      // (any stage other than idle) and "pass" only for a fresh valid sample
      // (its own staleness window: the emitted `stale` flag).
      if (a.sampling || (a.stage && a.stage !== "idle")) return "testing";
      if (a.sampleValid && a.stale !== true) return "pass";
      if ((a.errorCount || 0) > 0 && !a.sampleValid) return "fail";
      return "untested";
    }
    case "ethernet": {
      const e = s.ethernet || {};
      if (e.running) return "testing";
      if (e.stage === "initializing" || e.stage === "waiting_link" || e.stage === "acquiring_dhcp") return "testing";
      if (e.stage === "passed" || e.testPassed === true) return "pass";
      if (e.stage === "failed") return "fail";
      return "untested";
    }
    case "sd": {
      const d = s.sdTest || {};
      if (d.running) return "testing";
      if (["probing", "mounting", "writing", "reading", "cleaning"].includes(d.stage)) return "testing";
      if (d.stage === "passed" || d.testPassed === true) return "pass";
      if (d.stage === "failed") return "fail";
      return "untested";
    }
    case "rs485": {
      const r = s.rs485 || {};
      if (r.running) return "testing";
      if (r.pass) return "pass";
      if (r.lastError && r.lastError !== "stopped") return "fail";
      return "untested";
    }
    default:
      return "untested";
  }
}

// Builds the same document block the firmware appends to /api/status.
// Derived purely from the emitted /api/status fields (no clock needed).
export function buildQcSummary(status) {
  const items = {};
  for (const item of QC_ITEM_IDS) {
    items[item] = {
      state: qcItemState(item, status),
      manualRequired: QC_MANUAL_REQUIRED[item]
    };
  }
  return { items };
}