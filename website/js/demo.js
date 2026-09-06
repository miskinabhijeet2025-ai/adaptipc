/* demo.js — replays the REAL decision log (data/routing.json) with
 * animated cost bars, timeline and history. Nothing is simulated:
 * the log was produced by benchmarks/decision_demo.c. */
"use strict";
let DEC = null, idx = 0, shmN = 0, udsN = 0;

fetch("data/routing.json")
  .then(r => r.json())
  .then(j => {
    DEC = j.decisions;
    const tl = document.getElementById("d-timeline");
    DEC.forEach(d => {
      const seg = document.createElement("i");
      seg.className = d.route === "SHM" ? "tl-shm" : "tl-uds";
      tl.appendChild(seg);
    });
    document.getElementById("d-prog").textContent = `0 / ${DEC.length}`;
    document.getElementById("start").disabled = false;
  })
  .catch(e => {
    document.getElementById("d-route").textContent =
      "data/routing.json not loaded";
    console.error(e);
  });

function show(i) {
  if (!DEC) return;
  idx = i;
  const d = DEC[i];
  document.getElementById("d-seq").textContent = `${i + 1} / ${DEC.length}`;
  document.getElementById("d-payload").textContent = d.payload + " B";
  document.getElementById("d-occ").textContent =
    (d.occ_bytes / 1024).toFixed(1) + " KB";
  const routeEl = document.getElementById("d-route");
  routeEl.textContent = d.route;
  routeEl.className = d.route === "SHM" ? "shm-c" : "uds-c";
  document.getElementById("d-reason").textContent = d.reason;
  document.getElementById("d-shm").textContent = d.cost_shm_us.toFixed(2);
  document.getElementById("d-uds").textContent = d.cost_uds_us.toFixed(2);
  const mx = Math.max(d.cost_shm_us, d.cost_uds_us, 1e-6);
  document.getElementById("d-shmbar").style.width =
    (100 * d.cost_shm_us / mx) + "%";
  document.getElementById("d-udsbar").style.width =
    (100 * d.cost_uds_us / mx) + "%";
  document.getElementById("d-q").textContent =
    d.queue_wait_us.toFixed(1) + " us";
  document.getElementById("d-sw").textContent =
    d.switch_cost_us.toFixed(1) + " us";
  const segs = document.querySelectorAll("#d-timeline i");
  segs.forEach((s, k) => s.style.opacity = k <= i ? 1 : 0.25);
}

document.getElementById("start").onclick = () => {
  if (!DEC || document.getElementById("start").dataset.running) return;
  document.getElementById("start").dataset.running = "1";
  shmN = 0; udsN = 0;
  const iv = setInterval(() => {
    if (idx >= DEC.length) {
      clearInterval(iv);
      delete document.getElementById("start").dataset.running;
      return;
    }
    const d = DEC[idx];
    if (d.route === "SHM") shmN++; else udsN++;
    document.getElementById("d-shmn").textContent = shmN;
    document.getElementById("d-udsn").textContent = udsN;
    show(idx);
    idx++;
    document.getElementById("d-prog").textContent =
      `${idx} / ${DEC.length}`;
  }, 60);
};
