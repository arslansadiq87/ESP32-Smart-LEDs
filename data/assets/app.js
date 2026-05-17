/*
  RGB Controller Dashboard
  - Pulls /api/status
  - Sends /api/cmd (JSON) with debounce
  - Saves /api/save
*/

const $ = (q) => document.querySelector(q);
const byId = (id) => document.getElementById(id);
const el = (tag, cls) => { const e = document.createElement(tag); if (cls) e.className = cls; return e; };

let STATE = null;
let PATTERNS = [];
let ZONES = [];

// Keep UI state for the "All Zones" card so refresh() doesn't reset controls
let ALL_UI = {
  pat: null,   // number | null
  col: null,   // "#RRGGBB" | null
  bri: null,   // number | null
  spd: null    // number | null
};

// ---------- utils ----------
function fmtBri(b) { return `${b}/95`; }
function nowStr() { return new Date().toLocaleString(); }

async function api(path, { method = 'GET', body = null } = {}) {
  const opt = { method, headers: {} };
  if (body != null) {
    opt.headers['Content-Type'] = 'application/json';
    opt.body = JSON.stringify(body);
  }
  const r = await fetch(path, opt);
  if (!r.ok) {
    const t = await r.text();
    throw new Error(`${r.status} ${r.statusText}: ${t}`);
  }
  const ct = r.headers.get('content-type') || '';
  if (ct.includes('application/json')) return r.json();
  return r.text();
}

// ---------- safe DOM helpers ----------
function setText(id, value) {
  const e = byId(id);
  if (!e) return;
  e.textContent = value ?? "";
}
function setStyle(id, prop, value) {
  const e = byId(id);
  if (!e) return;
  e.style[prop] = value;
}
function toggleClass(id, className, on) {
  const e = byId(id);
  if (!e) return;
  e.classList.toggle(className, !!on);
}

// ---------- UI sections ----------
function setTopBar(s) {
  setText("tbStatus", "Online");
  setText("tbIP", s?.ip || "—");
  setText("tbTime", nowStr());

  // dot color
  const dot = document.getElementById("dot-net");
  if (dot) {
    dot.classList.remove("ok", "bad");
    dot.classList.add("ok"); // green
  }
}


function setHero(s) {
  const zones = s?.zones || [];
  const active = zones.filter(z => z.on).length;
  const first = zones.find(z => z.on) || zones[0] || null;

  setText("heroTitle", "Live Scene");
  setText("heroSub", "All Zones");
  setText("heroDesc", "Tap a zone to control it. Changes apply instantly.");

  setText("statZones", String(active));
  setText("statPattern", first ? (PATTERNS[first.pattern] || `#${first.pattern}`) : "—");
  setText("statBrightness", first ? fmtBri(first.brightness) : "—");

  toggleClass("heroOrb", "glow", true);
  setStyle("heroAccent", "opacity", "1");
}

// ---------- components ----------
function badge(on) {
  const b = el('span', `badge ${on ? 'on' : 'off'}`);
  b.textContent = on ? 'ON' : 'OFF';
  return b;
}

function sliderRow(label, valueText, input, pillClass = "") {
  const row = el('div', 'row slider-row');
  const l = el('div', 'label'); l.textContent = label;

  const r = el('div', `pill slider-pill ${pillClass}`.trim());
  r.textContent = valueText;

  row.append(l, input, r);
  return row;
}

function debounce(fn, ms) {
  let t = null;
  return (...args) => { clearTimeout(t); t = setTimeout(() => fn(...args), ms); };
}

function toast(msg) {
  const t = el('div', 'toast');
  t.textContent = msg;
  document.body.appendChild(t);
  setTimeout(() => t.classList.add('show'), 10);
  setTimeout(() => { t.classList.remove('show'); setTimeout(() => t.remove(), 250); }, 2600);
}

// ---------- commands ----------
const sendCmdDebounced = debounce(async (payload) => {
  try {
    await api('/api/cmd', { method: 'POST', body: payload });
    await refresh();
  } catch (e) {
    console.error(e);
    toast(`Command failed: ${e.message}`);
  }
}, 120);

async function sendCmdNoRefresh(payload) {
  try {
    await api('/api/cmd', { method: 'POST', body: payload });
  } catch (e) {
    console.error(e);
    toast(`Command failed: ${e.message}`);
  }
}

// ---------- helpers for ALL zones card ----------
function allSame(key) {
  if (!ZONES.length) return { same: true, value: null };
  const v0 = ZONES[0][key];
  for (const z of ZONES) {
    if (z[key] !== v0) return { same: false, value: null };
  }
  return { same: true, value: v0 };
}

function computeAllUIFromState() {
  // If user already changed ALL_UI manually, keep it.
  // Only initialize from device state if ALL_UI.* is null.
  const samePat = allSame("pattern");
  const sameCol = allSame("color");
  const sameBri = allSame("brightness");
  const sameSpd = allSame("frame_ms");

  if (ALL_UI.pat == null && samePat.same) ALL_UI.pat = samePat.value;
  if (ALL_UI.col == null && sameCol.same) ALL_UI.col = `#${sameCol.value}`;
  if (ALL_UI.bri == null && sameBri.same) ALL_UI.bri = sameBri.value;
  if (ALL_UI.spd == null && sameSpd.same) ALL_UI.spd = sameSpd.value;

  // If not same, leave null -> show Mixed label in UI
}

function patternSupportsColor(patIdx) {
  const name = PATTERNS[patIdx];
  return name && name.toUpperCase() === "SOLID";
}

// ---------- cards ----------
function zoneCard(z, i) {
  const card = el('section', 'card span-6');

  const head = el('div', 'card-head');

  const left = el('div', 'card-title');
  const title = el('div', 'card-name'); title.textContent = z.name;
  const meta = el('div', 'card-meta');
  meta.textContent = `${z.leds} LEDs • Max ${z.max_leds}`;
  left.append(title, meta);

  const right = el('div', 'card-actions'); // CSS will keep them on same row
  right.appendChild(badge(z.on));

  const btn = el('button', 'btn small');
  btn.textContent = z.on ? 'Turn Off' : 'Turn On';
  btn.onclick = () => sendCmdDebounced({ zone: i, cmd: z.on ? 'OFF' : 'ON' });
  right.appendChild(btn);

  head.append(left, right);

  const body = el('div', 'card-body');

  // LED count
  const cnt = el('input');
  cnt.type = 'range';
  cnt.min = '1';
  cnt.max = String(z.max_leds);
  cnt.value = String(z.leds);

  const cntInput = el('input');
  cntInput.type = 'number';
  cntInput.min = '1';
  cntInput.max = String(z.max_leds);
  cntInput.value = String(z.leds);

  const syncCount = (value) => {
    const v = Math.max(1, Math.min(Number(z.max_leds), Number(value)));
    cnt.value = String(v);
    cntInput.value = String(v);
    sendCmdDebounced({ zone: i, count: v });
  };
  cnt.oninput = () => syncCount(cnt.value);
  cntInput.oninput = () => syncCount(cntInput.value);

  const row = el('div', 'row slider-row');
  const label = el('div', 'label'); label.textContent = 'LED Count';
  const pill = el('div', 'pill slider-pill'); pill.textContent = `${z.leds}`;
  row.append(label, cnt, cntInput);

  body.append(row);

  card.append(head, body);
  return card;
}

function allZonesCard() {
  const card = el('section', 'card span-12 allzones'); // full row always

  const head = el('div', 'card-head');

  const left = el('div', 'card-title');

// icon badge (not blank anymore)
const icon = el('div', 'badge');
icon.innerHTML = `
  <svg width="18" height="18" viewBox="0 0 24 24" fill="none">
    <path d="M4 7h16M4 12h16M4 17h16" stroke="rgba(255,255,255,0.85)" stroke-width="2" stroke-linecap="round"/>
  </svg>
`;

const textWrap = el('div','');
const title = el('div', 'card-name'); title.textContent = 'All Zones';
const meta  = el('div', 'card-meta'); meta.textContent = 'Apply settings to every zone';
textWrap.append(title, meta);

left.append(icon, textWrap);

  const right = el('div', 'card-actions');
  const on = el('button', 'btn small'); on.textContent = 'All ON';
  on.onclick = () => sendCmdDebounced({ all: true, cmd: 'ON' });

  const off = el('button', 'btn small ghost'); off.textContent = 'All OFF';
  off.onclick = () => sendCmdDebounced({ all: true, cmd: 'OFF' });

  right.append(on, off);

  head.append(left, right);

  const body = el('div', 'card-body');

  const kv = el('div', 'kv');

  // Pattern
  const patWrap = el('div', 'kv-item');
  const pLab = el('div', 'kv-label'); pLab.textContent = 'Pattern';
  const sel = el('select');

  // If patterns are mixed and ALL_UI.pat is null => show "Mixed"
  const samePat = allSame("pattern");
  const showMixed = (!samePat.same && ALL_UI.pat == null);

  if (showMixed) {
    const mixed = document.createElement('option');
    mixed.value = "-1";
    mixed.textContent = "— Mixed —";
    sel.appendChild(mixed);
  }

  PATTERNS.forEach((p, idx) => {
    const o = document.createElement('option');
    o.value = String(idx);
    o.textContent = p;
    sel.appendChild(o);
  });

  // set current value
  if (ALL_UI.pat != null) sel.value = String(ALL_UI.pat);
  else if (samePat.same) sel.value = String(samePat.value);
  else sel.value = "-1";

  sel.onchange = () => {
    const v = Number(sel.value);
    if (v < 0) return;
    ALL_UI.pat = v;
    sendCmdDebounced({ all: true, pat: v });
    colorWrap.style.display = patternSupportsColor(v) ? '' : 'none';
  };
  patWrap.append(pLab, sel);

  // Color (only for patterns that support color)
  const colorWrap = el('div', 'kv-item');
  const cLab = el('div', 'kv-label'); cLab.textContent = 'Color';
  const cRow = el('div', 'kv-row');
  const cDot = el('div', 'dot');

  const sameCol = allSame("color");
  const showMixedCol = (!sameCol.same && ALL_UI.col == null);

  const cPick = el('input');
  cPick.type = 'color';

  if (showMixedCol) {
    cDot.style.background = '#29f2a6';
    cPick.value = '#29f2a6';
  } else {
    const col = (ALL_UI.col != null) ? ALL_UI.col : `#${sameCol.value}`;
    cDot.style.background = col;
    cPick.value = col;
  }

  cPick.oninput = () => {
    cDot.style.background = cPick.value;
    ALL_UI.col = cPick.value;
    sendCmdNoRefresh({ all: true, col: cPick.value });
  };
  cPick.onchange = () => {
    cDot.style.background = cPick.value;
    ALL_UI.col = cPick.value;
    sendCmdDebounced({ all: true, col: cPick.value });
  };

  cRow.append(cDot, cPick);
  colorWrap.append(cLab, cRow);

  const initialPat = (ALL_UI.pat != null) ? ALL_UI.pat : (samePat.same ? samePat.value : -1);
  colorWrap.style.display = patternSupportsColor(initialPat) ? '' : 'none';

  kv.append(patWrap, colorWrap);

  // Brightness
  const sameBri = allSame("brightness");
  const bri = el('input');
  bri.type = 'range';
  bri.min = '0';
  bri.max = '95';
  bri.value = String(
    ALL_UI.bri != null ? ALL_UI.bri : (sameBri.same ? sameBri.value : 60)
  );
  bri.oninput = () => {
    ALL_UI.bri = Number(bri.value);
    sendCmdDebounced({ all: true, bri: Number(bri.value) });
  };

  // Speed
  const sameSpd = allSame("frame_ms");
  const spd = el('input');
  spd.type = 'range';
  spd.min = '1';
  spd.max = '200';
  spd.value = String(
    ALL_UI.spd != null ? ALL_UI.spd : (sameSpd.same ? sameSpd.value : 20)
  );
  spd.oninput = () => {
    ALL_UI.spd = Number(spd.value);
    sendCmdDebounced({ all: true, spd: Number(spd.value) });
  };

  body.append(
    kv,
    sliderRow('Brightness', (sameBri.same && ALL_UI.bri == null) ? fmtBri(sameBri.value) : '0–95', bri),
    sliderRow('Speed', (sameSpd.same && ALL_UI.spd == null) ? `${sameSpd.value} ms` : '1–200 ms', spd)
  );

  card.append(head, body);
  return card;
}

// ---------- refresh ----------
async function refresh() {
  try {
    const s = await api('/api/status');

    STATE = s;
    PATTERNS = s.patterns || [];
    ZONES = s.zones || [];

    computeAllUIFromState();

    setTopBar(s);
    setHero(s);

    const grid = byId('grid');
    if (!grid) return;

    grid.innerHTML = '';
    grid.appendChild(allZonesCard());
    ZONES.forEach((z, i) => grid.appendChild(zoneCard(z, i)));

  } catch (e) {
    console.error(e);
    const dot = document.getElementById("dot-net");
    if (dot) {
      dot.classList.remove("ok");
      dot.classList.add("bad"); // red
    }

    setText("tbStatus", "Offline");
    toast('Device not reachable.');
  }
}

// ---------- init ----------
function init() {
  const btnRefresh = byId('btn-refresh');
  if (btnRefresh) btnRefresh.addEventListener('click', refresh);

  const btnSave = byId('btn-save');
  if (btnSave) {
    btnSave.addEventListener('click', async () => {
      try {
        await api('/api/save', { method: 'POST', body: {} });
        toast('Saved!');
      } catch (e) {
        console.error(e);
        toast(`Save failed: ${e.message}`);
      }
    });
  }

  const btnHKReset = byId('btn-hk-reset');
  if (btnHKReset) {
    btnHKReset.addEventListener('click', async () => {
      const ok = confirm('Reset HomeKit pairing? You will need to re-add the accessory in Home.');
      if (!ok) return;
      try {
        await api('/api/hk/reset', { method: 'POST', body: {} });
        toast('HomeKit pairing reset.');
      } catch (e) {
        console.error(e);
        toast(`HomeKit reset failed: ${e.message}`);
      }
    });
  }

  refresh();
}

window.addEventListener('load', init);
