/* routing.js — browser model of the implemented AdaptIPC decision rule.
 * SIMPLIFIED INTERACTIVE MODEL: mirrors src/cost_model.c concepts
 * (linear transport cost, queue wait = occ / max(D, floor), notification
 * floor, switching margin, health penalty) but does not execute the C
 * middleware. Real measurements: see the Results section. */
"use strict";

const RL = {
  udsFixed: 3.0, udsSlope: 0.0015,     // us + us/B (priors; see cost_model.c)
  shmFixed: 0.3, shmSlope: 0.0004,
  switchCost: 20.0, margin: 5.0, setupCost: 52.0,
  notifyFloor: 2000.0, notifyActive: 5.0,
  drainFloorBps: 50e6, capacity: 1 << 20, healthPenalty: 0.0,
  qos: "BALANCED", latencyBudget: 500.0, latencyWeight: 1.0,
  current: "UDS",
};

function rlPredict(payload, occBytes, drainPct, consumerActive) {
  // queue wait = occupancy bytes / drain rate, with conservative floor
  let bps = Math.max(1e6, drainPct / 100 * 8e9);      // drain slider -> B/s
  const stale = !consumerActive;                       // slider = activity
  if (!stale && bps < RL.drainFloorBps) bps = RL.drainFloorBps;
  const qw = occBytes <= 0 ? 0 : occBytes / bps * 1e6;
  const costShm = RL.shmFixed + RL.shmSlope * payload + qw + RL.healthPenalty;
  const costUds = RL.udsFixed + RL.udsSlope * payload;
  const notify = consumerActive ? RL.notifyActive : RL.notifyFloor;
  const latShm = qw + notify + RL.shmFixed + RL.shmSlope * payload;
  const latUds = costUds;
  let latencyPenalty = 0;
  if (RL.qos !== "THROUGHPUT" && RL.latencyBudget > 0) {
    const oU = Math.max(0, latUds - RL.latencyBudget);
    const oS = Math.max(0, latShm - RL.latencyBudget);
    latencyPenalty = RL.latencyWeight * Math.max(oU, oS);
  }
  const scoreShm = costShm +
    (RL.current === "SHM" ? 0 : RL.switchCost) +
    (latShm > latUds ? latencyPenalty : 0);
  const scoreUds = costUds +
    (RL.current === "UDS" ? 0 : RL.switchCost) +
    (latUds >= latShm ? latencyPenalty : 0);
  let route, reason;
  if (RL.current === "NONE") {
    route = scoreShm <= scoreUds ? "SHM" : "UDS";
    reason = "INITIAL_ROUTE";
  } else {
    const cand = scoreShm <= scoreUds ? "SHM" : "UDS";
    if (cand === RL.current) { route = cand; reason = "COST_STABLE"; }
    else {
      const candScore = cand === "SHM" ? scoreShm : scoreUds;
      const curScore = RL.current === "SHM" ? scoreShm : scoreUds;
      if (candScore + RL.margin < curScore) {
        route = cand;
        reason = !consumerActive && cand === "UDS" ? "NOTIFY_FLOOR"
                   : (qw > RL.switchCost ? "QUEUE_PRESSURE" : "COST_WIN");
      } else { route = RL.current; reason = "STICKY_MARGIN"; }
    }
  }
  return { route, reason, scoreShm, scoreUds, qw, costShm, costUds,
           latencyPenalty };
}
if (typeof window !== "undefined") window.RL = RL;
