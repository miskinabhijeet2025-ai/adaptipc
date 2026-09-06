/* app.js — AdaptIPC website: theme, nav, stats, charts, routing
 * simulator, timeline, health animation, experiment explorer.
 * All charts consume website/data/*.json exported from real
 * measurement CSVs by scripts/export_web_data.py. */
"use strict";

/* ---------- theme (dark/light, persisted) ---------- */
const themeBtn = document.getElementById("theme");
function applyTheme(t) {
  document.documentElement.setAttribute("data-theme", t);
  themeBtn.textContent = t === "light" ? "☀" : "☾";
  try { localStorage.setItem("adaptipc-theme", t); } catch (e) {}
}
applyTheme((() => {
  try { return localStorage.getItem("adaptipc-theme") || "dark"; }
  catch (e) { return "dark"; }
})());
themeBtn.onclick = () => applyTheme(
  document.documentElement.getAttribute("data-theme") === "light"
    ? "dark" : "light");

/* ---------- reveal-on-scroll ---------- */
const io = new IntersectionObserver(es => es.forEach(e => {
  if (e.isIntersecting) { e.target.classList.add("in"); io.unobserve(e.target); }
}), { threshold: 0.12 });
document.querySelectorAll("section").forEach(s => {
  s.classList.add("reveal"); io.observe(s);
});

/* ---------- animated counters (real repository numbers) ---------- */
const STATS = [
  { n: 10, label: "test suites passing" },
  { n: 7, label: "experiment campaigns" },
  { n: 512, label: "logged policy decisions" },
  { n: 1000000, label: "stress messages, zero loss" },
  { n: 2, label: "transports, one API" },
  { n: 0, label: "false switches (200k adversarial msgs)" },
];
const statsEl = document.getElementById("stats");
if (statsEl) {
  STATS.forEach(s => {
    const d = document.createElement("div");
    d.className = "stat";
    d.innerHTML = `<b data-n="${s.n}">0</b><span>${s.label}</span>`;
    statsEl.appendChild(d);
  });
  const io2 = new IntersectionObserver(es => es.forEach(e => {
    if (!e.isIntersecting) return;
    io2.unobserve(e.target);
    const b = e.target.querySelector("b");
    const target = +b.dataset.n;
    const t0 = performance.now();
    const tick = t => {
      const p = Math.min(1, (t - t0) / 1200);
      b.textContent = Math.round(target * (1 - Math.pow(1 - p, 3)))
        .toLocaleString();
      if (p < 1) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  }), { threshold: 0.4 });
  statsEl.querySelectorAll(".stat").forEach(s => io2.observe(s));
}

/* ---------- architecture hover tips ---------- */
const tipbar = document.getElementById("tipbar");
document.querySelectorAll("#arch .anode").forEach(n => {
  n.addEventListener("mouseenter", () => {
    tipbar.textContent = n.dataset.tip || "";
  });
});

/* ---------- routing simulator (simplified browser model) ---------- */
const $ = id => document.getElementById(id);
function simUpdate() {
  const payload = +$("s-payload").value;
  const occPct = +$("s-queue").value;
  const drain = +$("s-drain").value;
  const active = drain > 5;
  RL.capacity = 1 << 20;
  RL.healthPenalty = { HEALTHY: 0, DEGRADED: 100, BLOCKED: 1000 }[
    $("s-health").value] || 0;
  RL.qos = $("s-qos").value;
  // EWMA snapshot for display: model payload smoothing toward the slider
  if (!simUpdate.ewma) simUpdate.ewma = 1024;
  simUpdate.ewma += 0.2 * (payload - simUpdate.ewma);
  const occBytes = RL.capacity * occPct / 100;
  const r = rlPredict(payload, occBytes, drain, active);
  RL.current = r.route;
  $("s-payload").output; // noop for older engines
  $("o-payload").value = payload + " B";
  $("o-queue").value = occPct + "%";
  $("o-drain").value = drain + "%";
  $("sim-shm").textContent = r.scoreShm.toFixed(2) + " us";
  $("sim-uds").textContent = r.scoreUds.toFixed(2) + " us";
  $("sim-q").textContent = r.qw.toFixed(1) + " us";
  const el = $("sim-route");
  el.textContent = r.route;
  el.className = r.route === "SHM" ? "shm-c" : "uds-c";
  $("sim-reason").textContent = r.reason;
  // hero glow follows the decision
  const bhS = $("bh-shm"), bhU = $("bh-uds");
  if (bhS && bhU) {
    bhS.querySelector(".node").classList.toggle("glowshm",
      r.route === "SHM");
    bhU.querySelector(".node").classList.toggle("glowuds",
      r.route === "UDS");
  }
}
["s-payload", "s-queue", "s-drain", "s-health", "s-qos"].forEach(
  id => { const e = $(id); if (e) e.addEventListener("input", simUpdate); });
simUpdate();

/* ---------- charts from real data ---------- */
const PALETTE = { UDS: "#4d8dfd", SHM: "#2ea043", adapt: "#d29922",
  size_only: "#d62728", size_hysteresis: "#1f77b4", queue_aware:
  "#2ca02c", cost_aware: "#ff7f0e", full_adaptive: "#9467bd" };
function jload(name) {
  return fetch("data/" + name).then(r => r.json())
    .catch(() => null);
}
function chartTheme() {
  const light = document.documentElement.getAttribute("data-theme") === "light";
  return { grid: light ? "#d8dee4" : "#21262d", fg: light ? "#1f2328" : "#e6edf3" };
}
Promise.all([jload("benchmark.json"), jload("policies.json"),
             jload("queue.json")]).then(([bench, pol, queue]) => {
  if (!bench || typeof Chart === "undefined") return;
  const th = chartTheme();
  Chart.defaults.color = th.fg;
  Chart.defaults.borderColor = th.grid;

  // throughput by policy
  const seen = {};
  const tpLabels = [], tpData = [];
  bench.rows.forEach(r => {
    if (!seen[r.policy]) { seen[r.policy] = true;
      tpLabels.push(r.policy); tpData.push(r.throughput_mbps); }
  });
  new Chart($("c-tp"), { type: "bar",
    data: { labels: tpLabels,
      datasets: [{ label: "aggregate throughput (MB/s)",
        data: tpData,
        backgroundColor: tpLabels.map(p => PALETTE[p] || "#888") }] },
    options: { plugins: { title: { display: true,
      text: "Payload sweep (measured)" } }, scales: {
      y: { type: "logarithmic", title: { display: true,
        text: "MB/s" } } } } });

  // latency p50/p99
  const lLabels = [], p50 = [], p99 = [];
  bench.rows.forEach(r => {
    if (!seen2(lLabels, r.policy)) {
      lLabels.push(r.policy); p50.push(r.p50_us); p99.push(r.p99_us); }
  });
  function seen2(arr, v) { return arr.includes(v); }
  new Chart($("c-lat"), { type: "bar",
    data: { labels: lLabels, datasets: [
      { label: "p50 (us)", data: p50, backgroundColor: "#1f77b4" },
      { label: "p99 (us)", data: p99, backgroundColor: "#d62728" }] },
    options: { scales: { y: { type: "logarithmic",
      title: { display: true, text: "us (log)" } } } } });

  if (pol) {
    const pl = Object.keys(pol.policies);
    new Chart($("c-stab"), { type: "bar",
      data: { labels: pl, datasets: [
        { label: "genuine escapes", data: pl.map(p =>
            pol.policies[p].genuine), backgroundColor: "#2ca02c" },
        { label: "noise flaps", data: pl.map(p =>
            pol.policies[p].flaps), backgroundColor: "#d62728" },
        { label: "total switches", data: pl.map(p =>
            pol.policies[p].switches), backgroundColor: "#8b949e" }] },
      options: { plugins: { title: { display: true,
        text: "switch classification (200k adversarial messages)" } },
        scales: { y: { type: "logarithmic" } } } });
  }
  if (queue) {
    new Chart($("c-queue"), { type: "line",
      data: { labels: queue.rows.map(r => r.occ_pct),
        datasets: [
          { label: "model prediction (us)", data: queue.rows.map(
              r => r.predicted_wait_us), borderColor: "#d29922",
            tension: .25 },
          { label: "actual delay (ms)", data: queue.rows.map(
              r => r.actual_delay_us / 1000), borderColor: "#d62728",
            tension: .25 }] },
      options: { plugins: { title: { display: true,
        text: "queue-wait prediction vs actual (honest gap when "
            + "consumer stalls)" } },
        scales: { x: { title: { display: true,
          text: "ring occupancy (%)" } } } } });
  }
});

/* ---------- routing timeline (real decision log) ---------- */
jload("routing.json").then(rt => {
  if (!rt) return;
  const tl = document.getElementById("rt-timeline");
  if (!tl) return;
  rt.decisions.forEach(d => {
    const seg = document.createElement("i");
    seg.className = d.route === "SHM" ? "tl-shm" : "tl-uds";
    seg.title = `#${d.seq} · ${d.payload} B · occ ${d.occ_bytes} B · `
      + `${d.route} · ${d.reason}`;
    seg.addEventListener("mouseenter", () => {
      $("rt-detail").textContent =
        `#${d.seq} — payload ${d.payload} B, occupancy ${d.occ_bytes} B, `
        + `queue wait ${d.queue_wait_us.toFixed(1)} us → ${d.route} `
        + `(${d.reason})`;
    });
    tl.appendChild(seg);
  });
});

/* ---------- health state machine animation ---------- */
const hRun = document.getElementById("health-run");
if (hRun) hRun.onclick = () => {
  const seq = ["HEALTHY", "DEGRADED", "BLOCKED", "RECOVERING",
               "HEALTHY2"];
  const tips = {
    HEALTHY: "Occupancy below 20% for the debounce streak.",
    DEGRADED: "Occupancy ≥ 80% or drain deficit (D < 0.5·A).",
    BLOCKED: "Occupancy ≥ 95% — producer parks at the watermark.",
    RECOVERING: "Drained below 20% — probation before HEALTHY.",
    HEALTHY2: "Back to normal routing. Escape measured <1 ms, "
            + "recovery 2.7 ms.",
  };
  let i = 0;
  const states = document.querySelectorAll(".hstate");
  const iv = setInterval(() => {
    states.forEach(s => s.classList.remove("active"));
    const cur = document.querySelector(
      `.hstate[data-s="${seq[i]}"]`);
    if (cur) { cur.classList.add("active");
      $("health-tip").textContent = tips[seq[i]]; }
    if (++i >= seq.length) clearInterval(iv);
  }, 900);
};

/* ---------- cost model breakdown (real decision sample) ---------- */
jload("routing.json").then(rt => {
  if (!rt) return;
  const d = rt.decisions[Math.min(430, rt.decisions.length - 1)];
  const box = document.getElementById("costbars");
  if (!box) return;
  const items = [
    ["Transport cost (SHM)", d.cost_shm_us],
    ["Queue wait (SHM)", d.queue_wait_us],
    ["Switching cost", d.switch_cost_us],
    ["Setup cost", d.setup_cost_us],
    ["Health penalty", d.health_penalty_us],
  ];
  const mx = Math.max(...items.map(i => i[1]), 1e-6);
  items.forEach(([lbl, v]) => {
    const div = document.createElement("div");
    div.className = "cb";
    div.innerHTML = `<div class="lbl"><span>${lbl}</span>`
      + `<span>${v.toFixed(2)} us</span></div>`
      + `<div class="bar"><i style="width:${100 * v / mx}%;`
      + `background:var(--adapt)"></i></div>`;
    box.appendChild(div);
  });
  box.insertAdjacentHTML("beforeend",
    `<p class="note">Representative decision #${d.seq}: selected `
    + `<b>${d.route}</b> — ${d.reason}. Values from the real decision `
    + `log.</p>`);
});

/* ---------- experiment explorer ---------- */
jload("experiments.json").then(ex => {
  if (!ex) return;
  const wrap = document.getElementById("exp-cards");
  if (!wrap) return;
  ex.experiments.forEach(e => {
    const c = document.createElement("div");
    c.className = "card expcard reveal in";
    c.innerHTML = `<h3>${e.name}</h3><p>${e.blurb}</p>`
      + `<p class="cmd"><code>${e.cmd}</code></p>`
      + `<p class="cmd">source: ${e.source}</p>`;
    c.onclick = () => navigator.clipboard &&
      navigator.clipboard.writeText(e.cmd);
    wrap.appendChild(c);
  });
});
