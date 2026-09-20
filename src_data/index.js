// ─── State ────────────────────────────────────────────────────────────────────

const LED_COUNT = 12;
// Must match the order of the .tab-button elements in index.html.
const TAB_ORDER = ["home", "fx", "bambu", "timer", "settings", "info", "console"];
let currentTabIndex = 0;

let ws;
let reconnectDelay = 1000;
const MAX_RECONNECT_DELAY = 30000;

// Mirror of the firmware state, kept in sync by the status messages.
const ring = {
  base:    { r: 0, g: 0, b: 0, on: false, blink: false },
  effect:  "none",
  spinner: { r: 0, g: 120, b: 255, tail: 4, speed: 60, cw: true },
  rainbowCycleTime: 5,
  partyMadness: 5,
  progress: { pct: 0, fr: 0, fg: 170, fb: 255, br: 0, bg: 0, bb: 0 },
  clock:    { hr: 200, hg: 40, hb: 0, mr: 0, mg: 180, mb: 0, sr: 0, sg: 0, sb: 60 },
  chase:    { r: 255, g: 255, b: 255, speed: 120 },
  geometry: { origin: 0, reverse: false }
};

let timers      = [];
let timerNextId = 0;
let suppress    = false;   // true while widgets are filled from firmware

// ─── WebSocket ────────────────────────────────────────────────────────────────

// The firmware is single-threaded and the async server has a small send queue,
// so outgoing messages are rate limited; the last payload always gets through.
const WS_MAX_CALLS_PER_SEC = 10;
const wsSendTimestamps = [];
let wsPendingPayload = null;
let wsPendingTimer   = null;

function wsSend(payload) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  const now = Date.now();
  while (wsSendTimestamps.length && now - wsSendTimestamps[0] > 1000) wsSendTimestamps.shift();
  if (wsSendTimestamps.length < WS_MAX_CALLS_PER_SEC) {
    clearTimeout(wsPendingTimer);
    wsPendingTimer = null;
    wsPendingPayload = null;
    wsSendTimestamps.push(now);
    ws.send(JSON.stringify(payload));
  } else {
    wsPendingPayload = payload;
    if (!wsPendingTimer) {
      const delay = 1000 - (now - wsSendTimestamps[0]) + 1;
      wsPendingTimer = setTimeout(() => {
        wsPendingTimer = null;
        const p = wsPendingPayload;
        wsPendingPayload = null;
        if (p) wsSend(p);
      }, delay);
    }
  }
}

let reloadPoller = null;
let otaFirmwareFlashing = false;

function startReloadPoller() {
  if (reloadPoller) return;
  reloadPoller = setInterval(async () => {
    try {
      const res = await fetch("/ping", { cache: "no-store" });
      if (res.ok) location.reload();
    } catch (_) {}
  }, 2000);
}
function stopReloadPoller() { clearInterval(reloadPoller); reloadPoller = null; }

function showDisconnected() {
  document.getElementById("disconnected-overlay").classList.add("visible");
  startReloadPoller();
}
function hideDisconnected() {
  document.getElementById("disconnected-overlay").classList.remove("visible");
  stopReloadPoller();
}

function connect() {
  ws = new WebSocket(`ws://${location.hostname}/ws`);

  ws.addEventListener("open", () => {
    reconnectDelay = 1000;
    hideDisconnected();
    const tab = localStorage.getItem("activeTab");
    if (tab === "console") wsSend({ type: "consoleOpen" });
    if (tab === "info" || tab === "settings") wsSend({ type: "infoOpen" });
    if (tab === "bambu") wsSend({ type: "bambuOpen" });
  });

  ws.addEventListener("message", (ev) => {
    let d;
    try { d = JSON.parse(ev.data); } catch (_) { return; }
    switch (d.type) {
      case "ringStatus":    onRingStatus(d);   break;
      case "effectStatus":  onEffectStatus(d); break;
      case "configStatus":  onConfigStatus(d); break;
      case "wifiConfig":    onWifiConfig(d);   break;
      case "mqttConfig":    onMqttConfig(d);   break;
      case "bambuConfig":   onBambuConfig(d);  break;
      case "bambuStatus":   onBambuStatus(d);  break;
      case "timerConfig":   onTimerConfig(d);  break;
      case "sysInfo":
      case "sysInfoStatic": onSysInfo(d);      break;
      case "randomResult":  onRandomResult(d); break;
      case "otaStatus":     onOtaStatus(d.step);          break;
      case "otaProgress":   onOtaProgress(d.step, d.pct); break;
      case "console":       appendConsoleLine(d.text);    break;
      case "status":        onStatus(d);       break;
    }
  });

  ws.addEventListener("close", () => {
    if (otaFirmwareFlashing) {
      const msg = document.getElementById("ota-reconnect-msg");
      if (msg) {
        msg.textContent = "Device rebooting, page will refresh automatically…";
        msg.style.display = "block";
      }
      startReloadPoller();
      setTimeout(() => {
        if (document.visibilityState !== "hidden") location.reload();
      }, 5000);
      return;
    }
    showDisconnected();
    setTimeout(connect, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, MAX_RECONNECT_DELAY);
  });

  ws.addEventListener("error", () => ws.close());
}

function onStatus(d) {
  if (d.reboot) {
    showRestorePhase("wifi");
    document.getElementById("wifiSaveText").textContent = "Configuration saved. Rebooting...";
    startReloadPoller();
    return;
  }
  if (d.status === "saved")  toast(d.message || "Saved");
  if (d.status === "error")  toast(d.message || "Error", true);
}

// ─── Small helpers ────────────────────────────────────────────────────────────

const $ = (id) => document.getElementById(id);

const clamp255 = (v) => Math.max(0, Math.min(255, Math.round(v)));
const hex2 = (n) => clamp255(n).toString(16).padStart(2, "0");
const rgbToHex = (r, g, b) => `#${hex2(r)}${hex2(g)}${hex2(b)}`;

function hexToRgb(hex) {
  const v = parseInt(String(hex).replace("#", ""), 16);
  return { r: (v >> 16) & 255, g: (v >> 8) & 255, b: v & 255 };
}

// h in [0,360)
function hsvToRgb(h, s, v) {
  h = ((h % 360) + 360) % 360;
  const c = v * s, x = c * (1 - Math.abs(((h / 60) % 2) - 1)), m = v - c;
  let r = 0, g = 0, b = 0;
  if      (h <  60) { r = c; g = x; }
  else if (h < 120) { r = x; g = c; }
  else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; }
  else if (h < 300) { r = x; b = c; }
  else              { r = c; b = x; }
  return { r: (r + m) * 255, g: (g + m) * 255, b: (b + m) * 255 };
}

// ON/OFF pill buttons, the same control Semaphore uses everywhere.
function setToggle(btn, on) {
  if (!btn) return;
  btn.classList.toggle("on", !!on);
  btn.classList.toggle("off", !on);
  btn.textContent = on ? "ON" : "OFF";
}
const isToggleOn = (btn) => btn.classList.contains("on");

let toastTimer = null;
function toast(msg, isError) {
  const el = $("toast");
  el.textContent = msg;
  el.classList.toggle("error", !!isError);
  el.classList.add("visible");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove("visible"), 2200);
}

// Wraps every number input in − / + steppers, as on the Semaphore.
function wrapNumberInputs() {
  document.querySelectorAll('input[type="number"]').forEach(input => {
    if (input.parentNode.classList.contains("number-wrapper")) return;
    const isTime = input.classList.contains("time-input");
    const wrapper = document.createElement("div");
    wrapper.className = "number-wrapper" + (isTime ? " is-time" : "");

    const btnMinus = document.createElement("button");
    btnMinus.type = "button";
    btnMinus.className = "number-btn";
    btnMinus.textContent = "−";

    const btnPlus = document.createElement("button");
    btnPlus.type = "button";
    btnPlus.className = "number-btn";
    btnPlus.textContent = "+";

    btnMinus.addEventListener("click", () => {
      input.stepDown();
      input.dispatchEvent(new Event("change", { bubbles: true }));
    });
    btnPlus.addEventListener("click", () => {
      input.stepUp();
      input.dispatchEvent(new Event("change", { bubbles: true }));
    });

    input.parentNode.insertBefore(wrapper, input);
    wrapper.appendChild(btnMinus);
    wrapper.appendChild(input);
    wrapper.appendChild(btnPlus);
  });
}

const EYE_OPEN   = '<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/>';
const EYE_CLOSED = '<path d="M17.94 17.94A10.07 10.07 0 0112 20c-7 0-11-8-11-8a18.45 18.45 0 015.06-5.94M9.9 4.24A9.12 9.12 0 0112 4c7 0 11 8 11 8a18.5 18.5 0 01-2.16 3.19m-6.72-1.07a3 3 0 11-4.24-4.24M1 1l22 22"/>';

function togglePassword(inputId, btn) {
  const input = $(inputId);
  const showing = input.type === "text";
  input.type = showing ? "password" : "text";
  btn.querySelector("svg").innerHTML = showing ? EYE_OPEN : EYE_CLOSED;
}

function animateSaveIcon(id) {
  const icon = $(id);
  if (!icon) return;
  icon.classList.add("spinning");
  setTimeout(() => icon.classList.remove("spinning"), 700);
}

// ─── Ring preview ─────────────────────────────────────────────────────────────
// Mirrors the firmware's effect maths so the page shows what the ring is doing
// without the device streaming twelve colours several times a second.

const svgPixels = [];

function buildRingSvg() {
  const svg = $("ringSvg");
  const cx = 120, cy = 120, radius = 88, dot = 15;
  const ns = "http://www.w3.org/2000/svg";

  const halo = document.createElementNS(ns, "circle");
  halo.setAttribute("cx", cx);
  halo.setAttribute("cy", cy);
  halo.setAttribute("r", radius);
  halo.setAttribute("fill", "none");
  halo.setAttribute("stroke", "#21262d");
  halo.setAttribute("stroke-width", "10");
  svg.appendChild(halo);

  for (let i = 0; i < LED_COUNT; i++) {
    // Pixel 0 sits at the top and they run clockwise, which is how the ring
    // reads when the origin offset is zero.
    const a = (-90 + i * (360 / LED_COUNT)) * Math.PI / 180;
    const c = document.createElementNS(ns, "circle");
    c.setAttribute("cx", cx + radius * Math.cos(a));
    c.setAttribute("cy", cy + radius * Math.sin(a));
    c.setAttribute("r", dot);
    c.setAttribute("class", "px");
    c.setAttribute("fill", "#161b22");
    c.setAttribute("stroke", "#30363d");
    c.setAttribute("stroke-width", "1.5");
    svg.appendChild(c);
    svgPixels.push(c);
  }
}

// Logical ring position -> physical pixel, same rule as RingController::_px().
function mapPx(logical) {
  let i = ((logical % LED_COUNT) + LED_COUNT) % LED_COUNT;
  if (ring.geometry.reverse) i = (LED_COUNT - i) % LED_COUNT;
  return (i + ring.geometry.origin) % LED_COUNT;
}

const partyState = Array.from({ length: LED_COUNT },
                              () => ({ on: false, next: 0, col: { r: 0, g: 0, b: 0 } }));

// The random yes/no spin is a one-shot, not an effect, so the firmware never
// reports it in effectStatus. Replaying the same maths here keeps the preview
// in step with the ring during the game.
const ynPreview = { phase: null, start: 0, pos: 0, last: 0, yes: false };

function ynPreviewStart() {
  Object.assign(ynPreview, { phase: "spin", start: performance.now(), pos: 0, last: 0 });
}
function ynPreviewResult(yes) {
  Object.assign(ynPreview, { phase: "result", start: performance.now(), yes });
}

function computePixels(now) {
  const px = Array.from({ length: LED_COUNT }, () => ({ r: 0, g: 0, b: 0 }));
  const put = (logical, c) => { px[mapPx(logical)] = c; };

  if (ynPreview.phase) {
    const el = now - ynPreview.start;
    if (ynPreview.phase === "result") {
      if (el < 5000) {
        const c = ynPreview.yes ? { r: 0, g: 255, b: 0 } : { r: 255, g: 0, b: 0 };
        for (let i = 0; i < LED_COUNT; i++) px[i] = c;
        return px;
      }
      ynPreview.phase = null;
    } else if (el < 12000) {
      // Decelerates from 30 ms to 320 ms per step over 5 s, as in RingController.
      const interval = 30 + (320 - 30) * Math.min(1, el / 5000);
      if (now - ynPreview.last >= interval) {
        ynPreview.last = now;
        ynPreview.pos = (ynPreview.pos + 1) % LED_COUNT;
      }
      put(ynPreview.pos, { r: 255, g: 200, b: 0 });
      return px;
    } else {
      ynPreview.phase = null;   // no answer came back; stop spinning
    }
  }

  switch (ring.effect) {
    case "spinner": {
      const s = ring.spinner;
      const head = Math.floor(now / Math.max(10, s.speed)) % LED_COUNT;
      for (let t = 0; t <= s.tail; t++) {
        const f = 1 - t / (s.tail + 1);
        put(s.cw ? head - t : head + t, { r: s.r * f, g: s.g * f, b: s.b * f });
      }
      break;
    }
    case "rainbow": {
      const cycleMs = Math.max(100, ring.rainbowCycleTime * 1000);
      const base = (now % cycleMs) / cycleMs * 360;
      for (let i = 0; i < LED_COUNT; i++)
        put(i, hsvToRgb(base + i * (360 / LED_COUNT), 1, 1));
      break;
    }
    case "party": {
      const maxMs = 2000 + (ring.partyMadness - 1) * (20 - 2000) / 9;
      const minMs = Math.max(5, maxMs / 4);
      for (let i = 0; i < LED_COUNT; i++) {
        const st = partyState[i];
        if (now >= st.next) {
          st.on = !st.on;
          st.col = hsvToRgb(Math.random() * 360, 1, 1);
          st.next = now + minMs + Math.random() * (maxMs - minMs);
        }
        px[i] = st.on ? st.col : { r: 0, g: 0, b: 0 };
      }
      break;
    }
    case "progress": {
      const p = ring.progress;
      const exact = p.pct * LED_COUNT / 100;
      const full = Math.floor(exact), frac = exact - full;
      for (let i = 0; i < LED_COUNT; i++) {
        if (i < full) put(i, { r: p.fr, g: p.fg, b: p.fb });
        else if (i === full) put(i, { r: p.br + (p.fr - p.br) * frac,
                                      g: p.bg + (p.fg - p.bg) * frac,
                                      b: p.bb + (p.fb - p.bb) * frac });
        else put(i, { r: p.br, g: p.bg, b: p.bb });
      }
      break;
    }
    case "clock": {
      const d = new Date();
      const add = (pos, r, g, b) => {
        const t = px[mapPx(pos)];
        t.r = Math.min(255, t.r + r);
        t.g = Math.min(255, t.g + g);
        t.b = Math.min(255, t.b + b);
      };
      const k = ring.clock;
      add(Math.floor(d.getSeconds() * LED_COUNT / 60), k.sr, k.sg, k.sb);
      add(Math.floor(d.getMinutes() * LED_COUNT / 60), k.mr, k.mg, k.mb);
      add(d.getHours() % 12,                           k.hr, k.hg, k.hb);
      break;
    }
    case "chase": {
      const c = ring.chase;
      const step = Math.floor(now / Math.max(20, c.speed)) % 3;
      for (let i = step; i < LED_COUNT; i += 3) put(i, { r: c.r, g: c.g, b: c.b });
      break;
    }
    default: {
      const b = ring.base;
      const lit = b.on && (!b.blink || Math.floor(now / 500) % 2 === 0);
      if (lit) for (let i = 0; i < LED_COUNT; i++) px[i] = { r: b.r, g: b.g, b: b.b };
    }
  }
  return px;
}

function renderRing() {
  const px = computePixels(performance.now());
  for (let i = 0; i < LED_COUNT; i++) {
    const c = px[i];
    const dark = c.r + c.g + c.b < 12;
    svgPixels[i].setAttribute("fill", dark ? "#161b22" : rgbToHex(c.r, c.g, c.b));
  }
  requestAnimationFrame(renderRing);
}

function updateRingCaption() {
  const label = ring.effect !== "none"
    ? ring.effect
    : (ring.base.on ? (ring.base.blink ? "blink" : "on") : "off");
  $("ringCaption").innerHTML = `${LED_COUNT} px &middot; <b>${label.toUpperCase()}</b>`;
}

// ─── Tabs ─────────────────────────────────────────────────────────────────────

function moveTabIndicator(btn) {
  const indicator = document.querySelector(".tab-indicator");
  const tabs = document.querySelector(".tabs");
  if (!indicator || !tabs || !btn) return;
  const tabsRect = tabs.getBoundingClientRect();
  const btnRect  = btn.getBoundingClientRect();
  indicator.style.left   = `${btnRect.left - tabsRect.left}px`;
  indicator.style.top    = `${btnRect.top  - tabsRect.top}px`;
  indicator.style.width  = `${btnRect.width}px`;
  indicator.style.height = `${btnRect.height}px`;
}

function openTab(evt, tabName) {
  const newIndex = TAB_ORDER.indexOf(tabName);
  const toRight  = newIndex > currentTabIndex;
  const inClass  = toRight ? "slide-in-right" : "slide-in-left";
  const outClass = toRight ? "slide-out-left" : "slide-out-right";

  const oldTab = document.querySelector(".tab-content.active");
  const newTab = $(tabName);

  document.querySelectorAll(".tab-button").forEach(b => b.classList.remove("active"));
  const btn = evt ? evt.currentTarget : document.querySelectorAll(".tab-button")[newIndex];
  if (btn) { btn.classList.add("active"); moveTabIndicator(btn); }

  if (oldTab && oldTab !== newTab) {
    const viewport = document.querySelector(".tab-viewport");
    viewport.classList.add("is-transitioning");
    let done = 0;
    const onDone = () => { if (++done === 2) viewport.classList.remove("is-transitioning"); };

    oldTab.classList.add(outClass);
    oldTab.addEventListener("animationend", () => {
      oldTab.classList.remove("active", outClass);
      onDone();
    }, { once: true });

    newTab.classList.add("active", inClass);
    newTab.addEventListener("animationend", () => {
      newTab.classList.remove(inClass);
      onDone();
    }, { once: true });
  } else if (newTab) {
    newTab.classList.add("active");
  }

  const prev = localStorage.getItem("activeTab");
  currentTabIndex = newIndex;
  localStorage.setItem("activeTab", tabName);

  if (prev === "console" && tabName !== "console") wsSend({ type: "consoleClose" });
  if (tabName === "console") wsSend({ type: "consoleOpen" });

  const wasInfo = prev === "info" || prev === "settings";
  const isInfo  = tabName === "info" || tabName === "settings";
  if (wasInfo && !isInfo) wsSend({ type: "infoClose" });
  if (!wasInfo && isInfo) wsSend({ type: "infoOpen" });

  if (prev === "bambu" && tabName !== "bambu") wsSend({ type: "bambuClose" });
  if (tabName === "bambu" && prev !== "bambu") wsSend({ type: "bambuOpen" });

  if (tabName === "timer") renderTimers();
}

// ─── HOME ─────────────────────────────────────────────────────────────────────

function onRingStatus(d) {
  ring.base = { r: d.r, g: d.g, b: d.b, on: d.on, blink: d.blink };
  suppress = true;
  $("ringColor").value = rgbToHex(d.r, d.g, d.b);
  setToggle($("ringOnBtn"), d.on);
  setToggle($("ringBlinkBtn"), d.blink);
  suppress = false;
  updateRingCaption();
}

function sendRing() {
  if (suppress) return;
  const c = hexToRgb($("ringColor").value);
  const on    = isToggleOn($("ringOnBtn"));
  const blink = isToggleOn($("ringBlinkBtn"));
  ring.base = { ...c, on, blink };
  ring.effect = "none";
  markEffect();
  updateRingCaption();
  wsSend({ type: "setRing", r: c.r, g: c.g, b: c.b, on, blink });
}

// ─── FX ───────────────────────────────────────────────────────────────────────

function onEffectStatus(d) {
  ring.effect           = d.effect;
  ring.spinner          = d.spinner;
  ring.rainbowCycleTime = d.rainbowCycleTime;
  ring.partyMadness     = d.partyMadness;
  ring.progress         = d.progress;
  ring.clock            = d.clock || ring.clock;
  ring.chase            = d.chase;
  ring.geometry         = d.geometry;

  suppress = true;
  $("spinColor").value = rgbToHex(d.spinner.r, d.spinner.g, d.spinner.b);
  $("spinTail").value  = d.spinner.tail;
  $("spinSpeed").value = d.spinner.speed;
  setToggle($("spinCWBtn"), d.spinner.cw);

  $("rainbowCycleTime").value = d.rainbowCycleTime;
  $("partyMadnessVal").value  = d.partyMadness;

  $("progPct").value = d.progress.pct;
  $("progFg").value  = rgbToHex(d.progress.fr, d.progress.fg, d.progress.fb);
  $("progBg").value  = rgbToHex(d.progress.br, d.progress.bg, d.progress.bb);

  $("clockHours").value   = rgbToHex(ring.clock.hr, ring.clock.hg, ring.clock.hb);
  $("clockMinutes").value = rgbToHex(ring.clock.mr, ring.clock.mg, ring.clock.mb);
  $("clockSeconds").value = rgbToHex(ring.clock.sr, ring.clock.sg, ring.clock.sb);

  $("chaseColor").value = rgbToHex(d.chase.r, d.chase.g, d.chase.b);
  $("chaseSpeed").value = d.chase.speed;

  $("geoOrigin").value = d.geometry.origin;
  setToggle($("geoReverseBtn"), d.geometry.reverse);
  suppress = false;

  markEffect();
  updateRingCaption();
}

function markEffect() {
  document.querySelectorAll(".effect-btn").forEach(b =>
    b.classList.toggle("on", b.dataset.effect === ring.effect));
  document.querySelectorAll(".effect-params").forEach(p =>
    p.classList.toggle("visible", p.id === "p-" + ring.effect));
}

function pickEffect(name) {
  ring.effect = name;
  markEffect();
  updateRingCaption();
  switch (name) {
    case "spinner":  sendSpinner();  break;
    case "rainbow":  sendRainbow();  break;
    case "party":    sendParty();    break;
    case "progress": sendProgress(); break;
    case "chase":    sendChase();    break;
    case "clock":    sendClock();    break;
    default:
      // Releasing every effect leaves the base colour on the ring.
      ["setSpinner", "setRainbow", "setParty", "setProgress", "setClock", "setChase"]
        .forEach(t => wsSend({ type: t, on: false }));
  }
}

function sendSpinner() {
  if (suppress) return;
  const c = hexToRgb($("spinColor").value);
  const tail  = +$("spinTail").value;
  const speed = +$("spinSpeed").value;
  const cw    = isToggleOn($("spinCWBtn"));
  ring.spinner = { ...c, tail, speed, cw };
  ring.effect = "spinner";
  markEffect();
  updateRingCaption();
  wsSend({ type: "setSpinner", on: true, r: c.r, g: c.g, b: c.b, tail, speed, cw });
}

function sendRainbow() {
  if (suppress) return;
  const t = +$("rainbowCycleTime").value;
  ring.rainbowCycleTime = t;
  ring.effect = "rainbow";
  markEffect();
  updateRingCaption();
  wsSend({ type: "setRainbow", on: true, rainbowCycleTime: t });
}

function sendParty() {
  if (suppress) return;
  const m = +$("partyMadnessVal").value;
  ring.partyMadness = m;
  ring.effect = "party";
  markEffect();
  updateRingCaption();
  wsSend({ type: "setParty", on: true, partyMadness: m });
}

function sendProgress() {
  if (suppress) return;
  const pct = +$("progPct").value;
  const f = hexToRgb($("progFg").value);
  const b = hexToRgb($("progBg").value);
  ring.progress = { pct, fr: f.r, fg: f.g, fb: f.b, br: b.r, bg: b.g, bb: b.b };
  ring.effect = "progress";
  markEffect();
  updateRingCaption();
  wsSend({ type: "setProgress", on: true, pct,
           fr: f.r, fg: f.g, fb: f.b, br: b.r, bg: b.g, bb: b.b });
}

function sendClock() {
  if (suppress) return;
  const h = hexToRgb($("clockHours").value);
  const m = hexToRgb($("clockMinutes").value);
  const s = hexToRgb($("clockSeconds").value);
  ring.clock = { hr: h.r, hg: h.g, hb: h.b,
                 mr: m.r, mg: m.g, mb: m.b,
                 sr: s.r, sg: s.g, sb: s.b };
  ring.effect = "clock";
  markEffect();
  updateRingCaption();
  wsSend({ type: "setClock", on: true, ...ring.clock });
}

function sendChase() {
  if (suppress) return;
  const c = hexToRgb($("chaseColor").value);
  const speed = +$("chaseSpeed").value;
  ring.chase = { ...c, speed };
  ring.effect = "chase";
  markEffect();
  updateRingCaption();
  wsSend({ type: "setChase", on: true, r: c.r, g: c.g, b: c.b, speed });
}

function sendGeometry() {
  if (suppress) return;
  const origin  = +$("geoOrigin").value;
  const reverse = isToggleOn($("geoReverseBtn"));
  ring.geometry = { origin, reverse };
  wsSend({ type: "setGeometry", origin, reverse });
}

// ─── One-shot animations ──────────────────────────────────────────────────────

function sendWipe() {
  const c = hexToRgb($("wipeColor").value);
  wsSend({ type: "wipe", r: c.r, g: c.g, b: c.b, speed: 60 });
}

function openMorseOverlay()  { $("morse-overlay").classList.add("visible"); }
function closeMorseOverlay() { $("morse-overlay").classList.remove("visible"); }

function sendMorse() {
  const text = ($("morseText").value || "SOS").trim().toUpperCase();
  wsSend({ type: "morse", text });
  closeMorseOverlay();
}

// True between placing a bet and the firmware revealing the answer. Only the
// client that bet gets the verdict card; a spin started by a timer or over MQTT
// just animates the preview.
let ynAwaiting = false;

// Opens the card on its pick phase.
function openRandomYN() {
  resetGuessCard();
  $("guess-phase-pick").style.display = "";
  $("guess-overlay").classList.add("visible");
}

// Places the bet: the overlay gets out of the way so the ring can be watched.
function pickRandomYN(pick) {
  ynAwaiting = true;
  $("guess-phase-pick").style.display = "none";
  $("guess-overlay").classList.remove("visible");
  const icon = $("diceIcon");
  icon.classList.remove("spinning");
  void icon.offsetWidth;              // force reflow so a second press restarts it
  icon.classList.add("spinning");
  ynPreviewStart();
  wsSend({ type: "randomYesNo", pick });
}

function resetGuessCard() {
  const card = document.querySelector(".guess-card");
  card.style.background  = "";
  card.style.borderColor = "";
  card.classList.remove("winner", "loser");
  $("guess-phase-result").classList.remove("active");
  $("guessResultText").style.color = "";
  const hint = card.querySelector(".guess-tap-hint");
  if (hint) hint.style.color = "";
}

// The firmware decides the verdict, so every client agrees on it. Same card as
// the Semaphore's guess game: #guess-phase-result and both icons are
// display:none until the classes below reveal them.
function onRandomResult(d) {
  $("diceIcon").classList.remove("spinning");
  ynPreviewResult(d.yes);
  if (!ynAwaiting || d.win === undefined) return;
  ynAwaiting = false;

  const win  = d.win;
  const el   = $("guessResultText");
  const card = document.querySelector(".guess-card");
  el.textContent = win ? "WINNER" : "LOOSER";
  el.style.color = win ? "#1a1a1a" : "#fff";
  card.style.background  = win ? "var(--primary)" : "#c0392b";
  card.style.borderColor = win ? "var(--primary)" : "#c0392b";
  card.classList.toggle("winner", win);
  card.classList.toggle("loser", !win);
  const hint = card.querySelector(".guess-tap-hint");
  if (hint) hint.style.color = win ? "#1a1a1a" : "#fff";
  $("guess-phase-pick").style.display = "none";
  $("guess-phase-result").classList.add("active");
  $("guess-overlay").classList.add("visible");
}

function closeGuessOverlay() {
  $("guess-overlay").classList.remove("visible");
  resetGuessCard();
}

// ─── BAMBULAB ─────────────────────────────────────────────────────────────────

let bambuStateColors = {};
let bambuConnected   = false;

function onBambuConfig(d) {
  suppress = true;
  if (d.ip         !== undefined) $("bambuIp").value           = d.ip;
  if (d.serial     !== undefined) $("bambuSerial").value       = d.serial;
  if (d.accessCode !== undefined) $("bambuCode").value         = d.accessCode;
  if (d.enabled    !== undefined) $("bambuEnabled").checked    = d.enabled;
  if (d.idleTimeoutMin !== undefined) $("bambuIdleTimeout").value = d.idleTimeoutMin;
  if (d.bambuMode  !== undefined) setToggle($("bambuModeBtn"), d.bambuMode);
  if (d.stateColors) { bambuStateColors = d.stateColors; onBambuStateSelect(); }
  if (d.percent !== undefined) $("bambuPercentText").textContent = d.percent + " %";
  if (d.idleSec !== undefined && d.idleSec >= 0) {
    const m = Math.floor(d.idleSec / 60), s = d.idleSec % 60;
    $("bambuIdleRow").style.display = "flex";
    $("infoBambuIdle").textContent = `${m}m ${s}s`;
  }
  suppress = false;
  if (d.connected !== undefined || d.state !== undefined)
    onBambuStatus({ state: d.state, connected: d.connected, percent: d.percent });
}

function onBambuStatus(d) {
  if (d.connected !== undefined) {
    bambuConnected = d.connected;
    $("bambuModeBtn").disabled = !d.connected;
    $("infoBambuConnected").textContent = d.connected ? "Connected" : "Disconnected";
    $("infoBambuConnected").style.color = d.connected ? "var(--primary)" : "#f44336";
    // The idle counter is meaningless once the printer is gone.
    if (!d.connected) $("bambuIdleRow").style.display = "none";
  }
  if (d.state !== undefined && d.state !== null) {
    $("bambuStatusText").textContent = d.state;
    $("bambuStatusText").style.color = "var(--primary)";
  }
  if (d.percent !== undefined) {
    $("bambuPercentText").textContent = d.percent + " %";
    $("bambuPercentText").style.color = "var(--primary)";
  }
}

function onBambuStateSelect() {
  const st = $("bambuStateSelect").value;
  const c  = bambuStateColors[st] || [0, 0, 0];
  const prev = suppress;
  suppress = true;
  $("bambuStateColor").value = rgbToHex(c[0], c[1], c[2]);
  suppress = prev;
}

function sendBambuStateColor() {
  if (suppress) return;
  const st = $("bambuStateSelect").value;
  const c  = hexToRgb($("bambuStateColor").value);
  bambuStateColors[st] = [c.r, c.g, c.b];
  wsSend({ type: "setBambuStateColor", state: st, r: c.r, g: c.g, b: c.b });
}

function sendBambuMode() {
  if (suppress) return;
  wsSend({
    type: "setBambuMode",
    bambuMode: isToggleOn($("bambuModeBtn")),
    idleTimeoutMin: +$("bambuIdleTimeout").value || 0
  });
}

function saveBambu() {
  animateSaveIcon("saveBambuIcon");
  wsSend({
    type: "setBambu",
    ip:         $("bambuIp").value.trim(),
    serial:     $("bambuSerial").value.trim(),
    accessCode: $("bambuCode").value,
    enabled:    $("bambuEnabled").checked
  });
  toast("BambuLab saved");
}

// ─── TIMERS ───────────────────────────────────────────────────────────────────

const DAY_NAMES = ["Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"];

const TIMER_ACTIONS = [
  { value: "all_off",           label: "All OFF"         },
  { value: "ring",              label: "Ring Color"      },
  { value: "wipe",              label: "Color Wipe"      },
  { value: "spinner",           label: "Spinner ON"      },
  { value: "rainbow",           label: "Rainbow ON"      },
  { value: "party",             label: "Party ON"        },
  { value: "chase",             label: "Chase ON"        },
  { value: "clock",             label: "Clock ON"        },
  { value: "progress",          label: "Progress ON"     },
  { value: "random_yes_no",     label: "Random Yes/No"   },
  { value: "morse",             label: "Morse"           },
  { value: "weather_color",     label: "Weather Color"   },
  { value: "air_quality_color", label: "Air Quality"     },
  { value: "bambu_mode_on",     label: "Printer Mode ON" }
];

const NO_DURATION = ["morse", "random_yes_no", "wipe", "weather_color", "air_quality_color"];

function actionLabel(a) {
  const f = TIMER_ACTIONS.find(x => x.value === a);
  return f ? f.label : a;
}

function daysSummary(days) {
  if (!days || days.length === 0) return "Never";
  if (days.length === 7) return "Every day";
  const s = days.slice().sort((a, b) => a - b);
  if (s.join() === "0,1,2,3,4") return "Mon–Fri";
  if (s.join() === "5,6") return "Weekend";
  return s.map(d => DAY_NAMES[d]).join(" ");
}

function timerSummary(t) {
  return `${daysSummary(t.days)} ${t.time} — ${actionLabel(t.action)}`;
}

const timeParts = (time, idx) => (time || "00:00:00").split(":")[idx] || "0";

function onTimerConfig(d) {
  timers = (d.timers || []).map(t => ({ ...t, expanded: false }));
  timerNextId = timers.reduce((m, t) => Math.max(m, t.id), 0) + 1;
  renderTimers();
}

function addTimer() {
  timers.push({
    id: timerNextId++, enabled: true, days: [0, 1, 2, 3, 4], time: "08:00:00",
    action: "ring", ledColor: "#00aaff", morseText: "SOS", percent: 50,
    duration: 0, expanded: true
  });
  renderTimers();
}

function removeTimer(id) { timers = timers.filter(t => t.id !== id); renderTimers(); }
const timerById = (id) => timers.find(t => t.id === id);

function toggleTimerCard(id) {
  const t = timerById(id);
  t.expanded = !t.expanded;
  renderTimers();
}
function toggleTimerEnabled(id) {
  const t = timerById(id);
  t.enabled = !t.enabled;
  renderTimers();
}
function setTimerDay(id, day) {
  const t = timerById(id);
  const i = t.days.indexOf(day);
  if (i >= 0) t.days.splice(i, 1); else t.days.push(day);
  renderTimers();
}
function setTimerField(id, field, value) {
  const t = timerById(id);
  t[field] = value;
  renderTimers();
}
function setTimerTime(id, part, input) {
  const t = timerById(id);
  const p = (t.time || "00:00:00").split(":");
  const idx = part === "h" ? 0 : part === "m" ? 1 : 2;
  p[idx] = String(Math.max(0, parseInt(input.value) || 0)).padStart(2, "0");
  t.time = p.join(":");
  const el = document.querySelector(`.timer-card[data-id="${id}"] .timer-summary`);
  if (el) el.textContent = timerSummary(t);
}

function renderTimerCard(t) {
  const actOptions = TIMER_ACTIONS.map(a =>
    `<option value="${a.value}"${t.action === a.value ? " selected" : ""}>${a.label}</option>`).join("");

  const dayChips = DAY_NAMES.map((name, i) =>
    `<button class="day-chip${t.days.includes(i) ? " active" : ""}" onclick="setTimerDay(${t.id},${i})">${name}</button>`).join("");

  const colorRow = (t.action === "ring" || t.action === "wipe")
    ? `<div class="timer-row">
        <label>Color</label>
        <input type="color" value="${t.ledColor}" onchange="setTimerField(${t.id},'ledColor',this.value)">
       </div>` : "";

  const morseRow = t.action === "morse"
    ? `<div class="timer-row">
        <label>Text</label>
        <input type="text" value="${t.morseText || "SOS"}" maxlength="32"
               onchange="setTimerField(${t.id},'morseText',this.value.toUpperCase())">
       </div>` : "";

  const percentRow = t.action === "progress"
    ? `<div class="timer-row">
        <label>Percent</label>
        <input type="number" class="time-input" min="0" max="100" step="5" value="${t.percent || 0}"
               onchange="setTimerField(${t.id},'percent',parseInt(this.value)||0)">
       </div>` : "";

  const durationRow = !NO_DURATION.includes(t.action)
    ? `<div class="timer-row">
        <label>Duration (s)</label>
        <input type="number" class="time-input" min="0" step="1" value="${t.duration || 0}"
               placeholder="0 = no limit" onchange="setTimerField(${t.id},'duration',parseInt(this.value)||0)">
       </div>` : "";

  return `
    <div class="timer-card${t.expanded ? " expanded" : ""}" data-id="${t.id}">
      <div class="timer-header" onclick="toggleTimerCard(${t.id})">
        <svg class="timer-chevron" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"><polyline points="9 18 15 12 9 6"/></svg>
        <span class="timer-summary">${timerSummary(t)}</span>
        <button class="toggle-btn${t.enabled ? " on" : ""}" onclick="event.stopPropagation();toggleTimerEnabled(${t.id})">${t.enabled ? "ON" : "OFF"}</button>
      </div>
      <div class="timer-body"><div class="timer-body-inner">
        <div class="day-chips">${dayChips}</div>
        <div class="timer-row">
          <label>Time</label>
          <div class="time-fields">
            <input type="number" class="time-part" min="0" max="23" value="${timeParts(t.time,0)}" onchange="setTimerTime(${t.id},'h',this)">
            <span>:</span>
            <input type="number" class="time-part" min="0" max="59" value="${timeParts(t.time,1)}" onchange="setTimerTime(${t.id},'m',this)">
            <span>:</span>
            <input type="number" class="time-part" min="0" max="59" value="${timeParts(t.time,2)}" onchange="setTimerTime(${t.id},'s',this)">
          </div>
        </div>
        <div class="timer-row">
          <label>Action</label>
          <select onchange="setTimerField(${t.id},'action',this.value)">${actOptions}</select>
        </div>
        ${colorRow}${morseRow}${percentRow}${durationRow}
        <button class="timer-delete-btn" onclick="removeTimer(${t.id})">Delete</button>
      </div></div>
    </div>`;
}

function renderTimers() {
  const list = $("timerList");
  if (!list) return;
  list.innerHTML = timers.map(renderTimerCard).join("");
}

function saveTimers() {
  animateSaveIcon("saveTimersIcon");
  wsSend({ type: "setTimers", timers: timers.map(({ expanded, ...t }) => t) });
}

// ─── SETTINGS ─────────────────────────────────────────────────────────────────

function onWifiConfig(d) {
  suppress = true;
  $("deviceName").value   = d.deviceName || "";
  $("ntpServer").value    = d.ntpServer  || "";
  $("ssid").value         = d.ssid       || "";
  $("wifiPassword").value = d.password   || "";
  $("dhcp").checked       = !!d.dhcp;
  $("staticIp").value = d.ip      || "";
  $("subnet").value   = d.subnet  || "";
  $("gateway").value  = d.gateway || "";
  $("dns").value      = d.dns     || "";
  const tz = $("timezone");
  if (d.timezone && ![...tz.options].some(o => o.value === d.timezone)) {
    // Keep a zone the firmware holds but this list does not know about.
    tz.add(new Option(d.timezone, d.timezone));
  }
  if (d.timezone) tz.value = d.timezone;
  suppress = false;
  toggleStaticFields();
}

function toggleStaticFields() {
  $("staticFields").style.display = $("dhcp").checked ? "none" : "block";
}

function saveWifi() {
  animateSaveIcon("saveWifiIcon");
  showRestorePhase("wifi");
  $("wifiSaveText").textContent = "Applying configuration...";
  wsSend({
    type: "setWifi",
    deviceName: $("deviceName").value.trim(),
    ntpServer:  $("ntpServer").value.trim(),
    timezone:   $("timezone").value,
    ssid:       $("ssid").value.trim(),
    password:   $("wifiPassword").value,
    dhcp:       $("dhcp").checked,
    ip:         $("staticIp").value.trim(),
    subnet:     $("subnet").value.trim(),
    gateway:    $("gateway").value.trim(),
    dns:        $("dns").value.trim()
  });
}

function onMqttConfig(d) {
  suppress = true;
  $("mqttBroker").value    = d.broker   || "";
  $("mqttPort").value      = d.port     || 1883;
  $("mqttUser").value      = d.username || "";
  $("mqttPass").value      = d.password || "";
  $("mqttClientId").value  = d.clientId || "";
  $("mqttTopic").value     = d.topic    || "";
  $("mqttEnabled").checked = !!d.enabled;
  suppress = false;
  const el = $("mqttStatusLabel");
  if (!d.broker) {
    el.textContent = "Not configured";
    el.style.color = "#aaa";
  } else {
    el.textContent = d.connected ? "Connected" : "Disconnected";
    el.style.color = d.connected ? "#b1ff42" : "#f44336";
  }
}

function saveMqtt() {
  animateSaveIcon("saveMqttIcon");
  wsSend({
    type: "setMqtt",
    broker:   $("mqttBroker").value.trim(),
    port:     +$("mqttPort").value || 1883,
    username: $("mqttUser").value.trim(),
    password: $("mqttPass").value,
    clientId: $("mqttClientId").value.trim() || "ringlight",
    topic:    $("mqttTopic").value.trim()    || "ringlight",
    enabled:  $("mqttEnabled").checked
  });
}

// ─── INFO ─────────────────────────────────────────────────────────────────────

const sysInfo = {};

function formatUptime(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600);
  const m = Math.floor(s % 3600 / 60), sec = s % 60;
  if (d) return `${d}d ${h}h ${m}m`;
  if (h) return `${h}h ${m}m ${sec}s`;
  return m ? `${m}m ${sec}s` : `${sec}s`;
}

function setDot(id, r, g, b) {
  const el = $(id);
  if (r === undefined) { el.style.background = "transparent"; return; }
  el.style.background = rgbToHex(r, g, b);
}

function onSysInfo(d) {
  Object.assign(sysInfo, d);
  const S = sysInfo;
  // Skips missing elements: one absent row must not abort the whole render.
  const put = (id, v) => {
    const el = $(id);
    if (el && v !== undefined && v !== null && v !== "") el.textContent = v;
  };

  put("infoVersion",  S.version ? "v" + S.version : undefined);
  put("infoIp",       S.ip);
  put("infoSsid",     S.ssid);
  put("infoRssi",     S.rssi !== undefined ? S.rssi + " dBm" : undefined);
  put("infoDatetime", S.datetime);
  put("infoUptime",   S.uptime !== undefined ? formatUptime(S.uptime) : undefined);
  put("infoHeap",     S.freeHeap !== undefined ? (S.freeHeap / 1024).toFixed(1) + " KB" : undefined);
  put("infoLoopMax",  S.loopMax !== undefined ? S.loopMax + " ms" : undefined);
  put("infoLeds",     S.ledCount !== undefined ? `${S.ledCount} on GPIO ${S.ledPin}` : undefined);
  put("infoMac",      S.mac);
  put("infoCpu",      S.cpuFreq ? S.cpuFreq + " MHz" : undefined);
  put("infoChip",     S.chipModel ? `${S.chipModel} rev${S.chipRevision}` : undefined);
  put("infoChannel",  S.wifiChannel);
  // Only meaningful after a failed join; hidden while the station is happy.
  if (S.wifiFailText !== undefined) {
    const row  = $("wifiErrorRow");
    const show = S.wifiFailReason > 0;
    row.style.display = show ? "flex" : "none";
    if (show) {
      $("infoWifiError").textContent = `${S.wifiFailReason}: ${S.wifiFailText}`;
      $("infoWifiError").style.color = "#f44336";
    }
  }

  const mqttEl = $("infoMqtt");
  if (S.mqttBroker) {
    mqttEl.textContent = S.mqttConnected ? `Connected (${S.mqttBroker})`
                                         : `Disconnected (${S.mqttBroker})`;
    mqttEl.style.color = S.mqttConnected ? "#b1ff42" : "#f44336";
  } else if (S.mqttBroker !== undefined) {
    mqttEl.textContent = "Not configured";
    mqttEl.style.color = "#aaa";
  }
  if (S.bambuConnected !== undefined)
    onBambuStatus({ connected: S.bambuConnected });

  if (S.weatherTemp !== undefined) {
    const CONDITIONS = ["—", "Clear", "Partly cloudy", "Foggy",
                        "Drizzle", "Rainy", "Snowy", "Stormy"];
    const cond = CONDITIONS[S.weatherCondition] || "—";
    $("infoWeather").textContent        = `${cond} (code ${S.weatherCode})`;
    $("infoWeatherTemp").textContent    = S.weatherTemp.toFixed(1) + " °C";
    $("infoWeatherHumidity").textContent = Math.round(S.weatherHumidity) + " %";
    setDot("conditionDot", S.conditionR, S.conditionG, S.conditionB);
    setDot("weatherDot",   S.temperatureR, S.temperatureG, S.temperatureB);
    setDot("humidityDot",  S.humidityR, S.humidityG, S.humidityB);
  }
  if (S.aqPm25 !== undefined) {
    $("infoPm25").textContent = S.aqPm25.toFixed(1);
    $("infoPm10").textContent = S.aqPm10.toFixed(1);
    $("infoNo2").textContent  = S.aqNo2.toFixed(1);
    setDot("pm25Dot", S.aqPm25R, S.aqPm25G, S.aqPm25B);
    setDot("pm10Dot", S.aqPm10R, S.aqPm10G, S.aqPm10B);
    setDot("no2Dot",  S.aqNo2R,  S.aqNo2G,  S.aqNo2B);
  }

  $("weatherBtn").disabled    = S.weatherTemp === undefined;
  $("airQualityBtn").disabled = S.aqPm25 === undefined;

  if (S.latitude !== undefined) updateLocationLabel(S.latitude, S.longitude);
}

function onConfigStatus(d) {
  $("makeChangesPersistent").checked = d.makeChangesPersistent;
  if (d.latitude !== undefined) updateLocationLabel(d.latitude, d.longitude);
}

// Leaflet is created lazily the first time the overlay opens, so the map costs
// nothing until somebody actually sets a location.
let _map = null, _mapMarker = null;
let _pendingLat = null, _pendingLon = null;

function updateLocationLabel(lat, lon) {
  if (lat !== undefined) $("latitude").value  = lat || "";
  if (lon !== undefined) $("longitude").value = lon || "";
  const la = parseFloat($("latitude").value);
  const lo = parseFloat($("longitude").value);
  $("locationLabel").textContent = (la && lo) ? `lat: ${la}  lon: ${lo}` : "lat: —  lon: —";
}

function openMapOverlay() {
  $("map-overlay").classList.add("visible");
  _pendingLat = null;
  _pendingLon = null;
  $("mapConfirmBtn").disabled = true;
  $("mapCoordsDisplay").textContent = "";

  const existingLat = parseFloat($("latitude").value);
  const existingLon = parseFloat($("longitude").value);
  const center = (existingLat && existingLon) ? [existingLat, existingLon] : [45, 9];
  const zoom   = (existingLat && existingLon) ? 12 : 5;

  if (!_map) {
    _map = L.map("leaflet-map", { zoomControl: true }).setView(center, zoom);
    L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
      attribution: "© OpenStreetMap contributors",
      maxZoom: 19
    }).addTo(_map);
    _map.on("click", (e) => {
      _pendingLat = +e.latlng.lat.toFixed(6);
      _pendingLon = +e.latlng.lng.toFixed(6);
      if (_mapMarker) _mapMarker.setLatLng(e.latlng);
      else _mapMarker = L.marker(e.latlng).addTo(_map);
      $("mapCoordsDisplay").textContent = `${_pendingLat}, ${_pendingLon}`;
      $("mapConfirmBtn").disabled = false;
    });
  } else {
    _map.setView(center, zoom);
  }

  if (existingLat && existingLon) {
    _pendingLat = existingLat;
    _pendingLon = existingLon;
    if (_mapMarker) _mapMarker.setLatLng([existingLat, existingLon]);
    else _mapMarker = L.marker([existingLat, existingLon]).addTo(_map);
    $("mapCoordsDisplay").textContent = `${existingLat}, ${existingLon}`;
    $("mapConfirmBtn").disabled = false;
  }

  // The container has no size until the overlay is displayed.
  setTimeout(() => _map.invalidateSize(), 120);
}

function closeMapOverlay() {
  $("map-overlay").classList.remove("visible");
}

function confirmMapLocation() {
  if (_pendingLat === null) return;
  updateLocationLabel(_pendingLat, _pendingLon);
  wsSend({ type: "setLocation", latitude: _pendingLat, longitude: _pendingLon });
  closeMapOverlay();
  toast("Location saved");
}

function backupConfig() {
  const a = document.createElement("a");
  a.href = "/backup";
  a.download = "ringlight-backup.json";
  a.click();
}

let pendingRestore = null;

function showRestorePhase(phase) {
  ["confirm", "progress", "wifi"].forEach(p => {
    const el = $("restore-phase-" + p);
    if (el) el.style.display = (p === phase) ? "block" : "none";
  });
  $("restore-overlay").classList.add("visible");
}
function closeRestoreOverlay() {
  $("restore-overlay").classList.remove("visible");
  pendingRestore = null;
}

async function restoreConfig(input) {
  const file = input.files[0];
  input.value = "";
  if (!file) return;
  pendingRestore = await file.text();
  showRestorePhase("confirm");
}

async function confirmRestore() {
  if (!pendingRestore) return;
  showRestorePhase("progress");
  try {
    await fetch("/restore", { method: "POST", body: pendingRestore });
    fetch("/restart", { method: "POST" }).catch(() => {});
    startReloadPoller();
  } catch (_) {
    toast("Restore failed", true);
    closeRestoreOverlay();
  }
  pendingRestore = null;
}

// ─── OTA ──────────────────────────────────────────────────────────────────────

function normalizeVersion(v) {
  if (v === undefined || v === null) return "";
  return String(v).trim().replace(/^v/i, "").replace(/[^0-9.]/g, "").trim();
}

function compareVersions(localV, remoteV) {
  const a = normalizeVersion(localV).split(".").map(n => parseInt(n || "0", 10));
  const b = normalizeVersion(remoteV).split(".").map(n => parseInt(n || "0", 10));
  const maxLen = Math.max(a.length, b.length);
  for (let i = 0; i < maxLen; i++) {
    const av = a[i] || 0;
    const bv = b[i] || 0;
    if (av < bv) return -1;
    if (av > bv) return 1;
  }
  return 0;
}

async function checkLatestFirmwareVersion() {
  const label = $("ota-latest-label");
  const actions = $("ota-confirm-actions");
  const confirmBtn = $("ota-confirm-btn");
  const cancelBtn = $("ota-cancel-btn");
  const btn = $("updateBtn");

  if (!label || !actions) return false;

  const localVersion = normalizeVersion(sysInfo.version || $("infoVersion")?.textContent || "");
  btn && btn.classList.add("checking");
  btn && btn.classList.remove("update-available");
  btn && (btn.title = "Checking for firmware update...");
  label.textContent = "Checking for updates…";
  actions.style.display = "flex";
  if (confirmBtn) confirmBtn.style.display = "block";
  if (cancelBtn) cancelBtn.style.display = "block";

  try {
    const res = await fetch("https://api.github.com/repos/GabeMx5/ESP32-C3-Ringlight/releases/latest", { cache: "no-store" });
    if (!res.ok) throw new Error("GitHub request failed");
    const data = await res.json();
    const remoteVersion = normalizeVersion(data.tag_name || data.name || "");
    if (!remoteVersion) throw new Error("No release tag");

    const cmp = compareVersions(localVersion, remoteVersion);
    const versionText = `v${remoteVersion}`;

    if (cmp >= 0) {
      label.textContent = `${versionText} is the latest version`;
      actions.style.display = "none";
      btn && btn.classList.remove("checking");
      btn && btn.classList.remove("update-available");
      btn && (btn.title = `${versionText} is the latest version`);
      setTimeout(() => closeOtaOverlay(), 2200);
      return false;
    }

    label.textContent = `${versionText} is available`;
    actions.style.display = "flex";
    confirmBtn && (confirmBtn.textContent = `Update to ${versionText}`);
    btn && btn.classList.remove("checking");
    btn && btn.classList.add("update-available");
    btn && (btn.title = `Update available: ${versionText}`);
    return true;
  } catch (_) {
    label.textContent = "Flashes the latest release from GitHub.";
    actions.style.display = "flex";
    btn && btn.classList.remove("checking");
    btn && btn.classList.remove("update-available");
    btn && (btn.title = "Firmware update");
    return false;
  }
}

async function openOtaOverlay() {
  $("ota-phase-confirm").style.display  = "flex";
  $("ota-phase-progress").style.display = "none";
  $("ota-overlay").classList.add("visible");
  $("ota-overlay").onclick = (ev) => {
    if (ev.target === $("ota-overlay")) closeOtaOverlay();
  };
  await checkLatestFirmwareVersion();
}
function closeOtaOverlay() {
  $("ota-overlay").classList.remove("visible");
  $("ota-overlay").onclick = null;
}

function confirmOTA() {
  $("ota-phase-confirm").style.display  = "none";
  $("ota-phase-progress").style.display = "flex";
  wsSend({ type: "startOTA" });
}

const OTA_STEPS = ["backup", "filesystem", "restore", "firmware"];

function onOtaStatus(step) {
  $("ota-overlay").classList.add("visible");
  $("ota-phase-confirm").style.display  = "none";
  $("ota-phase-progress").style.display = "flex";
  if (step === "error") {
    $("ota-step-error").style.display = "block";
    return;
  }
  if (step === "firmware") {
    otaFirmwareFlashing = true;
    $("ota-step-error").style.display = "none";
  }
  const idx = OTA_STEPS.indexOf(step);
  OTA_STEPS.forEach((s, i) => {
    const el = $("ota-step-" + s);
    el.classList.toggle("active", i === idx);
    el.classList.toggle("done",   i < idx);
  });
}

function onOtaProgress(step, pct) {
  const el = $("ota-step-" + step);
  if (!el) return;
  const base = step === "filesystem" ? "Updating filesystem" : "Updating firmware";
  el.textContent = `${base} ${pct}%`;
}

// ─── CONSOLE ──────────────────────────────────────────────────────────────────

const consoleHistory = [];
let historyIdx = -1;

function appendConsoleLine(text) {
  const out = $("consoleOutput");
  const atBottom = out.scrollHeight - out.scrollTop - out.clientHeight < 40;
  const line = document.createElement("div");
  line.className = "console-line";
  line.textContent = text;
  out.appendChild(line);
  // Keep the buffer bounded: an ESP can talk for days.
  while (out.childElementCount > 500) out.removeChild(out.firstChild);
  if (atBottom) out.scrollTop = out.scrollHeight;
}

function consoleSend() {
  const input = $("consoleInput");
  const cmd = input.value.trim();
  if (!cmd) return;
  consoleHistory.push(cmd);
  historyIdx = consoleHistory.length;
  wsSend({ type: "consoleCmd", cmd });
  input.value = "";
}

function consoleKeyDown(e) {
  if (e.key === "Enter") { consoleSend(); return; }
  if (e.key === "ArrowUp" && historyIdx > 0) {
    historyIdx--; e.target.value = consoleHistory[historyIdx]; e.preventDefault();
  }
  if (e.key === "ArrowDown") {
    if (historyIdx < consoleHistory.length - 1) { historyIdx++; e.target.value = consoleHistory[historyIdx]; }
    else { historyIdx = consoleHistory.length; e.target.value = ""; }
    e.preventDefault();
  }
}

// ─── Boot ─────────────────────────────────────────────────────────────────────

buildRingSvg();
requestAnimationFrame(renderRing);
wrapNumberInputs();
connect();

// Home
$("ringColor").addEventListener("input", sendRing);
$("ringOnBtn").addEventListener("click", (e) => { setToggle(e.currentTarget, !isToggleOn(e.currentTarget)); sendRing(); });
$("ringBlinkBtn").addEventListener("click", (e) => { setToggle(e.currentTarget, !isToggleOn(e.currentTarget)); sendRing(); });
$("allOffBtn").addEventListener("click", () => wsSend({ type: "allOff" }));

// FX
document.querySelectorAll(".effect-btn").forEach(b =>
  b.addEventListener("click", () => pickEffect(b.dataset.effect)));
$("spinColor").addEventListener("input", sendSpinner);
$("spinTail").addEventListener("change", sendSpinner);
$("spinSpeed").addEventListener("change", sendSpinner);
$("spinCWBtn").addEventListener("click", (e) => { setToggle(e.currentTarget, !isToggleOn(e.currentTarget)); sendSpinner(); });
$("rainbowCycleTime").addEventListener("change", sendRainbow);
$("partyMadnessVal").addEventListener("change", sendParty);
$("progPct").addEventListener("change", sendProgress);
$("progFg").addEventListener("input", sendProgress);
$("progBg").addEventListener("input", sendProgress);
["clockHours", "clockMinutes", "clockSeconds"].forEach(id =>
  $(id).addEventListener("input", sendClock));
$("chaseColor").addEventListener("input", sendChase);
$("chaseSpeed").addEventListener("change", sendChase);
$("geoOrigin").addEventListener("change", sendGeometry);
$("geoReverseBtn").addEventListener("click", (e) => { setToggle(e.currentTarget, !isToggleOn(e.currentTarget)); sendGeometry(); });
$("wipeBtn").addEventListener("click", sendWipe);
$("morseBtn").addEventListener("click", openMorseOverlay);
$("morseCancelBtn").addEventListener("click", closeMorseOverlay);
$("morseConfirmBtn").addEventListener("click", sendMorse);
$("randomYNBtn").addEventListener("click", openRandomYN);
document.querySelectorAll(".guess-led-opt").forEach(b =>
  b.addEventListener("click", () => pickRandomYN(+b.dataset.pick)));
$("weatherBtn").addEventListener("click", () => wsSend({ type: "weatherColor" }));
$("airQualityBtn").addEventListener("click", () => wsSend({ type: "airQualityColor" }));

// BambuLab
$("bambuModeBtn").addEventListener("click", (e) => {
  if (e.currentTarget.disabled) return;
  setToggle(e.currentTarget, !isToggleOn(e.currentTarget));
  sendBambuMode();
});
$("bambuIdleTimeout").addEventListener("change", sendBambuMode);
$("bambuStateColor").addEventListener("input", sendBambuStateColor);

// Overlays close on tap
$("guess-overlay").addEventListener("click", () => {
  if ($("guess-phase-result").classList.contains("active")) closeGuessOverlay();
});

// Restore the last tab; the server is told which one when the socket opens.
(function restoreTab() {
  const saved = localStorage.getItem("activeTab") || "home";
  const idx = TAB_ORDER.indexOf(saved);
  if (idx < 0) return;
  currentTabIndex = idx;
  document.querySelectorAll(".tab-content").forEach(s => s.classList.remove("active"));
  document.querySelectorAll(".tab-button").forEach(b => b.classList.remove("active"));
  const el = $(saved);
  const btn = document.querySelectorAll(".tab-button")[idx];
  if (el) el.classList.add("active");
  if (btn) { btn.classList.add("active"); requestAnimationFrame(() => moveTabIndicator(btn)); }
})();

window.addEventListener("resize", () => {
  const btn = document.querySelector(".tab-button.active");
  if (btn) moveTabIndicator(btn);
});
