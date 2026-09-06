// AdaptIPC dashboard -- renders REAL decisions from decisions.json.
// The JSON is produced by benchmarks/decision_demo.c running the
// actual adaptive policy; nothing in this file simulates routing.
let D = null, idx = 0, playing = false, timer = null;

fetch("decisions.json")
  .then(r => { if (!r.ok) throw new Error("decisions.json not found");
               return r.json(); })
  .then(j => { D = j; buildTimeline(); render(0); })
  .catch(e => { document.getElementById("route").textContent =
                "decisions.json not loaded";
                document.getElementById("reason").textContent = e.message; });

function fmtB(v) { return v >= 1024 ? (v/1024).toFixed(1) + " KB" : v + " B"; }

function buildTimeline() {
  const tl = document.getElementById("timeline");
  tl.innerHTML = "";
  D.decisions.forEach((d, i) => {
    const seg = document.createElement("i");
    seg.className = d.route === "SHM" ? "tl-shm" : "tl-uds";
    seg.title = `#${d.seq}: ${d.payload} B -> ${d.route} (${d.reason})`;
    tl.appendChild(seg);
  });
}

function render(i) {
  if (!D) return;
  idx = Math.max(0, Math.min(i, D.decisions.length - 1));
  const d = D.decisions[idx];
  document.getElementById("seq").textContent = `${idx + 1} / ${D.decisions.length}`;
  document.getElementById("payload").textContent = fmtB(d.payload);
  document.getElementById("ewma").textContent = fmtB(d.occ_bytes);
  document.getElementById("route").textContent =
      d.route === "SHM" ? "SHARED MEMORY" : "UNIX SOCKET (UDS)";
  document.getElementById("route").className =
      "big " + (d.route === "SHM" ? "route-shm" : "route-uds");
  document.getElementById("reason").textContent = d.reason;
  document.getElementById("shm_cost").textContent = d.cost_shm_us.toFixed(2);
  document.getElementById("uds_cost").textContent = d.cost_uds_us.toFixed(2);
  const mx = Math.max(d.cost_shm_us, d.cost_uds_us, 1e-6);
  document.getElementById("shm_bar").style.width = (100*d.cost_shm_us/mx) + "%";
  document.getElementById("uds_bar").style.width = (100*d.cost_uds_us/mx) + "%";
  document.getElementById("qwait").textContent = d.queue_wait_shm_us.toFixed(1);
  document.getElementById("swcost").textContent = d.switch_cost_us.toFixed(1);
  document.getElementById("setup").textContent = d.setup_cost_us.toFixed(1);
  document.getElementById("health").textContent = d.health_penalty_us.toFixed(1);
  document.getElementById("phase").textContent = d.phase || "";
  const tb = document.getElementById("history");
  tb.innerHTML = "";
  for (let k = idx; k >= 0 && k > idx - 12; k--) {
    const h = D.decisions[k];
    const tr = document.createElement("tr");
    tr.innerHTML = `<td class="num">${h.seq}</td><td class="num">${h.payload}</td>` +
      `<td class="num">${h.occ_bytes.toFixed(0)}</td>` +
      `<td class="num">${h.queue_wait_shm_us.toFixed(1)}</td>` +
      `<td class="num">${h.cost_shm_us.toFixed(2)}</td>` +
      `<td class="num">${h.cost_uds_us.toFixed(2)}</td>` +
      `<td class="${h.route === "SHM" ? "route-shm" : "route-uds"}">${h.route}</td>` +
      `<td>${h.reason}</td>`;
    tb.appendChild(tr);
  }
  const segs = document.querySelectorAll("#timeline i");
  segs.forEach((s, k) => s.style.opacity = k <= idx ? 1 : 0.25);
}

document.getElementById("play").onclick = () => {
  playing = !playing;
  document.getElementById("play").textContent = playing ? "❚❚ Pause" : "▶ Play";
  if (playing) timer = setInterval(() => {
    if (idx >= D.decisions.length - 1) idx = -1;
    render(idx + 1);
  }, 400);
  else clearInterval(timer);
};
document.getElementById("step").onclick = () => render(idx + 1);
document.getElementById("reset").onclick = () => render(0);
