// ===== FILE: spiffs/rgb.js =====
async function apiGet(url) {
  const r = await fetch(url, { cache: "no-store" });
  if (!r.ok) throw new Error(await r.text());
  return r.json();
}
async function apiPost(url, obj) {
  const r = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(obj),
  });
  if (!r.ok) throw new Error(await r.text());
  return r.json();
}

const grid = document.getElementById("grid");
const paletteEl = document.getElementById("palette");
const statusEl = document.getElementById("status");

const ledBright = document.getElementById("ledBrightness");
const ledBrightVal = document.getElementById("ledBrightnessVal");

// Human-friendly, easy-to-distinguish palette (sRGB hex)
// Firmware applies gamma correction so what you see here should match better on the LEDs.
const PALETTE = [
  { name: "red",      hex: "#ff0000" },
  { name: "orange",   hex: "#ff7a00" },
  { name: "amber",    hex: "#ffb300" },
  { name: "yellow",   hex: "#ffd400" },
  { name: "lime",     hex: "#b6ff00" },
  { name: "green",    hex: "#00ff00" },
  { name: "mint",     hex: "#00ff8a" },
  { name: "cyan",     hex: "#00fff0" },
  { name: "sky",      hex: "#00a8ff" },
  { name: "blue",     hex: "#0055ff" },
  { name: "indigo",   hex: "#3b2cff" },
  { name: "violet",   hex: "#7a00ff" },
  { name: "magenta",  hex: "#ff00ff" },
  { name: "pink",     hex: "#ff3aa6" },
  { name: "white",    hex: "#ffffff" },
  { name: "warm",     hex: "#ffd7a8" },
];

function clampInt(v, lo, hi) {
  v = Number(v);
  if (!Number.isFinite(v)) v = lo;
  v = Math.trunc(v);
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

function normHex(h) {
  if (!h) return "#000000";
  h = String(h).trim().toLowerCase();
  if (!h.startsWith("#")) h = "#" + h;
  if (h.length === 4) {
    // #rgb -> #rrggbb
    const r = h[1], g = h[2], b = h[3];
    h = "#" + r + r + g + g + b + b;
  }
  if (!/^#[0-9a-f]{6}$/.test(h)) return "#000000";
  return h;
}

function setStatus(msg, ok = true) {
  statusEl.textContent = msg || "";
  statusEl.classList.remove("ok", "bad");
  statusEl.classList.add(ok ? "ok" : "bad");
}

let selectedIdx = 0;

function selectCell(idx) {
  selectedIdx = idx;
  Array.from(grid.children).forEach((c) => {
    c.classList.toggle("active", c._idx === idx);
    if (c._pill) c._pill.textContent = c._idx === idx ? "selected" : "";
  });
}

async function saveOne(idx, hex) {
  try {
    setStatus(`saving led ${idx}…`);
    await apiPost("/api/rgb", { idx, hex });
    setStatus("saved ✅");
  } catch (e) {
    setStatus("save failed: " + e.message, false);
  }
}

function makeCell(i, color) {
  const d = document.createElement("div");
  d.className = "rgbCell";
  d._idx = i;

  const left = document.createElement("div");
  left.className = "rgbLeft";

  const idx = document.createElement("div");
  idx.className = "rgbIdx";
  idx.textContent = "led " + i;

  const hex = document.createElement("div");
  hex.className = "rgbHex";

  left.appendChild(idx);
  left.appendChild(hex);

  const right = document.createElement("div");
  right.className = "rowActions";

  const dot = document.createElement("div");
  dot.className = "colorDot";

  const pill = document.createElement("div");
  pill.className = "pill";
  pill.textContent = "";

  right.appendChild(dot);
  right.appendChild(pill);

  function setHex(h) {
    h = normHex(h);
    d._hex = h;
    hex.textContent = h;
    dot.style.background = h;
  }

  setHex(color || "#000000");

  d.addEventListener("click", () => selectCell(i));

  d._setHex = setHex;
  d._pill = pill;

  d.appendChild(left);
  d.appendChild(right);

  return d;
}

function renderPalette() {
  if (!paletteEl) return;
  paletteEl.innerHTML = "";
  PALETTE.forEach((p) => {
    const b = document.createElement("div");
    b.className = "swatch";
    b.title = p.name;
    b.style.background = p.hex;
    b.addEventListener("click", async (ev) => {
      ev.preventDefault();
      ev.stopPropagation();

      const cell = Array.from(grid.children).find((c) => c._idx === selectedIdx);
      if (!cell) return;

      const hex = normHex(p.hex);
      cell._setHex(hex);
      await saveOne(selectedIdx, hex);
    });
    paletteEl.appendChild(b);
  });
}

async function loadAllColors() {
  try {
    setStatus("loading… ⚙️");
    const j = await apiGet("/api/rgb");
    const n = (j && j.count) || 0;
    const colors = (j && j.colors) || [];

    grid.innerHTML = "";
    for (let i = 0; i < n; i++) {
      grid.appendChild(makeCell(i, colors[i] || "#000000"));
    }
    // default select first LED
    selectCell(0);

    setStatus("loaded ✅");
  } catch (e) {
    setStatus("load failed: " + e.message, false);
  }
}

let tSaveBright = null;
async function loadBrightness() {
  try {
    const r = await apiGet("/api/led");
    const v = clampInt(r?.brightness ?? 100, 0, 100);
    ledBright.value = String(v);
    ledBrightVal.textContent = String(v);
  } catch (e) {
    setStatus("brightness load failed: " + e.message, false);
  }
}

async function saveBrightness() {
  const v = clampInt(ledBright.value ?? 100, 0, 100);
  ledBright.value = String(v);
  ledBrightVal.textContent = String(v);
  await apiPost("/api/led", { brightness: v });
}

function scheduleSaveBrightness() {
  if (tSaveBright) { clearTimeout(tSaveBright); tSaveBright = null; }
  tSaveBright = setTimeout(async () => {
    try {
      setStatus("saving brightness…");
      await saveBrightness();
      setStatus("saved ✅");
    } catch (e) {
      setStatus("brightness save failed: " + e.message, false);
    }
  }, 200);
}

ledBright.addEventListener("input", () => {
  const v = clampInt(ledBright.value, 0, 100);
  ledBright.value = String(v);
  ledBrightVal.textContent = String(v);
  scheduleSaveBrightness();
});
ledBright.addEventListener("change", scheduleSaveBrightness);

(async () => {
  renderPalette();
  await loadBrightness();
  await loadAllColors();
})();
