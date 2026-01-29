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
const st = document.getElementById("status");
const btnLoad = document.getElementById("btnLoad");
const btnSaveAll = document.getElementById("btnSaveAll");

function setStatus(msg, ok = true) {
  st.textContent = msg || "";
  st.style.color = ok ? "#0a0" : "#c00";
}

function makeCell(i, color) {
  const d = document.createElement("div");
  d.className = "cell";

  const left = document.createElement("div");
  left.innerHTML = '<div class="idx">led ' + i + "</div>";

  const pick = document.createElement("input");
  pick.type = "color";
  pick.value = color || "#ffffff";

  const btn = document.createElement("button");
  btn.textContent = "save";
  btn.addEventListener("click", async () => {
    try {
      btn.disabled = true;
      setStatus("saving led " + i + "…");
      await apiPost("/api/rgb", { idx: i, hex: pick.value });
      setStatus("saved led " + i + " ✅");
    } catch (e) {
      setStatus("save failed: " + e.message, false);
    } finally {
      btn.disabled = false;
    }
  });

  d.appendChild(left);
  d.appendChild(pick);
  d.appendChild(btn);

  // expose picker for save-all
  d._picker = pick;
  return d;
}

async function loadAll() {
  try {
    btnLoad.disabled = true;
    setStatus("loading…");
    const j = await apiGet("/api/rgb");
    const n = (j && j.count) || 0;
    const colors = (j && j.colors) || [];

    grid.innerHTML = "";
    for (let i = 0; i < n; i++) {
      grid.appendChild(makeCell(i, colors[i] || "#ffffff"));
    }
    setStatus("loaded ✅");
  } catch (e) {
    setStatus("load failed: " + e.message, false);
  } finally {
    btnLoad.disabled = false;
  }
}

btnLoad.addEventListener("click", loadAll);

btnSaveAll.addEventListener("click", async () => {
  try {
    btnSaveAll.disabled = true;
    setStatus("saving all…");

    const picks = Array.from(grid.children).map((c) => (c._picker ? c._picker.value : "#ffffff"));
    await apiPost("/api/rgb", { colors: picks });

    setStatus("saved all ✅");
  } catch (e) {
    setStatus("save all failed: " + e.message, false);
  } finally {
    btnSaveAll.disabled = false;
  }
});

loadAll();
