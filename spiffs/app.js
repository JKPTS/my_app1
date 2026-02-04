// ===== FILE: spiffs/app.js =====
function $(id) { return document.getElementById(id); }
function must(id) {
  const el = $(id);
  if (!el) throw new Error(`missing element id="${id}"`);
  return el;
}

let META = { maxBanks: 100, buttons: 8, bankCount: 1, maxActions: 20, longMs: 400 };
let LAYOUT = { bankCount: 1, banks: [] };
let BANKDATA = { switchNames: [] };

let cur = { bank: 0, btn: 0 };
let MAP = null;

let LOADING = false;
let lastUserNavAt = 0;

// limits
const MAX_BANK_NAME = 10;
const MAX_SWITCH_NAME = 5;

let dirtyBtn = false;
let dirtyLayout = false;
let dirtyBank = false;

// exp/fs (global)
let EXPFS = [null, null];
let expfsDirty = [false, false];
let expfsVer = [0, 0];
let expfsSaving = [false, false];
let expfsSavePromise = [Promise.resolve(), Promise.resolve()];

// ✅ non-blocking save controller (prevents UI lag)
let btnVer = 0, btnSaving = false, btnSavePromise = Promise.resolve();
let layoutVer = 0, layoutSaving = false, layoutSavePromise = Promise.resolve();
let bankVer = 0, bankSaving = false, bankSavePromise = Promise.resolve();
function nowMs() { return Date.now(); }

function wrap(n, max) {
  max = Math.max(1, Number(max || 1));
  let r = n % max;
  if (r < 0) r += max;
  return r;
}

function clampInt(v, lo, hi) {
  v = Number(v);
  if (!Number.isFinite(v)) v = lo;
  v = Math.trunc(v);
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

function clipText(s, maxLen) {
  return String(s || "").slice(0, maxLen);
}

function setMsg(text, ok = true) {
  const el = must("msg");
  const t = String(text || "");

  // Suppress a noisy validation toast that can show up during intermediate UI states.
  // (User-visible behavior: the toast simply doesn't appear.)
  if (!ok && /button\s+config\s+invalid/i.test(t)) {
    el.textContent = "";
    el.className = "msg";
    return;
  }

  el.textContent = t;
  el.className = "msg " + (ok ? "ok" : "bad");
}

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

// ---------- firmware + import/export helpers ----------
async function loadFwInfo() {
  const el = $("fwInfo");
  if (!el) return;
  try {
    const info = await apiGet("/api/fwinfo");
    el.textContent = `${info.name} v${info.ver} | idf ${info.idf} | ${info.date} ${info.time}`;
  } catch (e) {
    el.textContent = "unavailable";
  }
}

function setupImportExport() {
  const bExp = $("btnExport");
  const bImp = $("btnImport");
  const fImp = $("fileImport");
  if (bExp) {
    bExp.addEventListener("click", () => {
      const a = document.createElement("a");
      a.href = "/api/export";
      a.download = "footsw_cfg_v5.json";
      document.body.appendChild(a);
      a.click();
      a.remove();
    });
  }
  if (bImp && fImp) {
    bImp.addEventListener("click", () => fImp.click());
    fImp.addEventListener("change", async () => {
      const file = fImp.files && fImp.files[0];
      if (!file) return;
      try {
        const txt = await file.text();
        setMsg("importing… ⚙️");
        const res = await fetch("/api/import", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: txt,
        });
        if (!res.ok) throw new Error(await res.text());
        setMsg("import ok — reboot not required ✅");
        // refresh UI state
        await loadMeta();
        await loadLayout();
        await gotoBank(cur.bank);
      } catch (e) {
        setMsg("import failed: " + e.message, false);
      } finally {
        fImp.value = "";
      }
    });
  }
}

function setupFirmwareUpdate() {
  const b = $("fwUpload");
  const f = $("fwFile");
  const st = $("fwStatus");
  const pr = $("fwProg");
  if (!b || !f) return;

  b.addEventListener("click", () => {
    const file = f.files && f.files[0];
    if (!file) {
      if (st) st.textContent = "select a .bin first";
      return;
    }

    if (pr) pr.value = 0;
    if (st) st.textContent = "uploading…";
    b.disabled = true;

    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/fwupdate", true);
    xhr.setRequestHeader("Content-Type", "application/octet-stream");

    xhr.upload.onprogress = (ev) => {
      if (pr && ev.lengthComputable) {
        pr.value = Math.round((ev.loaded / ev.total) * 100);
      }
    };
    xhr.onerror = () => {
      if (st) st.textContent = "upload failed";
      b.disabled = false;
    };
    xhr.onload = () => {
      b.disabled = false;
      if (xhr.status >= 200 && xhr.status < 300) {
        if (st) st.textContent = "uploaded ✅ rebooting…";
      } else {
        if (st) st.textContent = "upload failed: " + xhr.responseText;
      }
    };

    xhr.send(file);
  });
}


async function apiGetExpfs(port) {
  return apiGet(`/api/expfs?port=${port}`);
}

async function apiPostExpfs(port, obj) {
  return apiPost(`/api/expfs?port=${port}`, obj);
}

async function apiPostExpfsCal(port, which) {
  return apiPost(`/api/expfs_cal?port=${port}&which=${which}`, {});
}

function curBankObj() { return (LAYOUT.banks || [])[cur.bank]; }

// ---------- "finish typing then save" helpers ----------
function isEditableField(el) {
  if (!el) return false;
  const tag = (el.tagName || "").toLowerCase();
  return tag === "input" || tag === "textarea" || tag === "select";
}

function forceCommitActiveField() {
  const el = document.activeElement;
  if (isEditableField(el)) el.blur();
}

function hookFinishedTypingInput(inp, onDirty, onFinish) {
  if (!inp) return;

  inp.addEventListener("input", () => onDirty?.());

  inp.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      e.preventDefault();
      inp.blur();
    }
  });

  inp.addEventListener("change", () => onFinish?.());
  inp.addEventListener("blur", () => onFinish?.());
}

async function flushPendingSaves() {
  forceCommitActiveField();

  await Promise.all([layoutSavePromise, bankSavePromise, btnSavePromise, expfsSavePromise[0], expfsSavePromise[1]]);

  if (dirtyLayout) await saveLayoutImmediate();
  if (dirtyBank) await saveBankImmediate();
  if (dirtyBtn) await saveButtonImmediate();
  if (expfsDirty[0]) await saveExpfsPortImmediate(0);
  if (expfsDirty[1]) await saveExpfsPortImmediate(1);
}

// ---------- render ----------
function renderHeader() {
  const b = curBankObj();

  must("curBank").textContent = cur.bank;
  must("curBankName").textContent = (b && b.name) ? b.name : "bank";

  must("bankName").value = (b && b.name) ? b.name : "";

  const sn = (BANKDATA.switchNames && BANKDATA.switchNames[cur.btn]) ? BANKDATA.switchNames[cur.btn] : "";
  must("switchName").value = sn;

  // dropdown
  const sel = must("bankSelect");
  sel.value = String(cur.bank);
}

function highlightGrid() {
  const pads = Array.from(must("btnGrid").querySelectorAll(".pad"));
  pads.forEach((p) => {
    const i = Number(p.dataset.idx);
    p.classList.toggle("active", i === cur.btn);
  });
}

function escapeHtml(s) {
  s = String(s ?? "");
  return s.replace(/[&<>"']/g, (c) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
  }[c]));
}

function renderGridLabels() {
  const pads = Array.from(must("btnGrid").querySelectorAll(".pad"));
  pads.forEach((p) => {
    const i = Number(p.dataset.idx);
    const name = (BANKDATA.switchNames && BANKDATA.switchNames[i]) ? BANKDATA.switchNames[i] : `SW ${i + 1}`;
    p.innerHTML = `
      <div class="padNum">${i + 1}</div>
      <div class="padName">${escapeHtml(name)}</div>
    `;
  });
}

function makeGrid() {
  const g = must("btnGrid");
  g.innerHTML = "";

  const count = Number(META.buttons || 8);
  for (let i = 0; i < count; i++) {
    const b = document.createElement("button");
    b.className = "pad";
    b.type = "button";
    b.dataset.idx = String(i);
    b.onclick = async () => {
      try {
        await flushPendingSaves();

        lastUserNavAt = nowMs();
        cur.btn = i;
        highlightGrid();
        renderHeader();
        await loadButton();
      } catch (e) {
        setMsg("load switch failed: " + e.message, false);
      }
    };
    g.appendChild(b);
  }

  renderGridLabels();
  highlightGrid();
}

// ---------- action rows ----------
function setInputVisible(inp, visible) {
  inp.style.display = visible ? "" : "none";
}

// ✅ small label wrappers (inline styles to avoid touching style.css)
function mkField(labelText, controlEl) {
  const wrap = document.createElement("div");
  wrap.className = "fieldWrap";
  wrap.style.display = "flex";
  wrap.style.flexDirection = "column";
  wrap.style.gap = "6px";

  const lbl = document.createElement("div");
  lbl.className = "fieldLbl";
  lbl.textContent = labelText || "";
  lbl.style.fontSize = "12px";
  lbl.style.opacity = "0.8";
  lbl.style.userSelect = "none";
  lbl.style.lineHeight = "1";

  wrap.appendChild(lbl);
  wrap.appendChild(controlEl);

  wrap._lbl = lbl;
  wrap._ctl = controlEl;

  return wrap;
}

function mkActionRow(action, onRemove, onDirtyBtn, onFinishBtn, onImmediateSaveBtn) {
  const row = document.createElement("div");
  row.className = "action";

  const type = document.createElement("select");
  ["cc", "pc"].forEach((t) => {
    const o = document.createElement("option");
    o.value = t;
    o.textContent = t;
    type.appendChild(o);
  });
  type.value = action.type || "cc";

  const ch = document.createElement("input");
  ch.type = "number"; ch.min = 1; ch.max = 16;
  ch.value = (action.ch ?? 1);

  const a = document.createElement("input");
  a.type = "number"; a.min = 0; a.max = 127;
  a.value = (action.a ?? 0);

  const b = document.createElement("input");
  b.type = "number"; b.min = 0; b.max = 127;
  b.value = (action.b ?? 0);

  const c = document.createElement("input");
  c.type = "number"; c.min = 0; c.max = 127;
  c.value = (action.c ?? 0);

  const rm = document.createElement("button");
  rm.className = "btn2 danger";
  rm.type = "button";
  rm.textContent = "remove";
  rm.onclick = () => {
    onRemove();
    onDirtyBtn?.();
    onFinishBtn?.();
  };

  // show/hide fields based on type
  function refreshFields() {
    const isCC = (type.value === "cc");
    // CC: ch, a=cc#, b=value, c=value2
    // PC: ch, a=program  (we'll keep b/c hidden)
    setInputVisible(a, true);
    setInputVisible(b, isCC);
    setInputVisible(c, isCC);
  }
  refreshFields();

  // ✅ save strategy:
  // - input => mark dirty (no network)
  // - change/blur => schedule save (non-blocking)
  [type, ch, a, b, c].forEach((el) => {
    el.addEventListener("input", () => onDirtyBtn?.());
    el.addEventListener("change", () => onFinishBtn?.());
    el.addEventListener("blur", () => onFinishBtn?.());
  });

  type.onchange = () => {
    refreshFields();
    onDirtyBtn?.();
    onFinishBtn?.();
  };

  // layout
  const grid = document.createElement("div");
  grid.className = "actionGrid";

  // ✅ label wrappers
  const fType = mkField("type", type);
  const fCh   = mkField("ch", ch);
  const fA    = mkField("a", a);
  const fB    = mkField("b", b);
  const fC    = mkField("c", c);

  // prettier widths (keep old look)
  [type, ch, a, b, c].forEach((el) => el.classList.add("mini"));
  rm.classList.add("miniBtn");

  grid.append(fType, fCh, fA, fB, fC, rm);
  row.appendChild(grid);

  // expose for collecting
  row._get = () => ({
    type: type.value,
    ch: clampInt(ch.value, 1, 16),
    a: clampInt(a.value, 0, 127),
    b: clampInt(b.value, 0, 127),
    c: clampInt(c.value, 0, 127),
  });

  return row;
}

function collectActions(listEl) {
  const rows = Array.from(listEl.querySelectorAll(".action"));
  return rows.map((r) => r._get()).filter(Boolean);
}

function renderActions(listEl, arr, onDirtyBtn, onFinishBtn, onImmediateSaveBtn) {
  listEl.innerHTML = "";
  (arr || []).forEach((a) => {
    const row = mkActionRow(
      a,
      () => row.remove(),
      onDirtyBtn,
      onFinishBtn,
      onImmediateSaveBtn
    );
    listEl.appendChild(row);
  });
}

function tryAddRow(listEl) {
  if (!MAP) return;

  const maxA = Number(META.maxActions || 20);
  const curCount = listEl.querySelectorAll(".action").length;
  if (curCount >= maxA) {
    setMsg("max actions reached", false);
    return;
  }

  const a = { type: "cc", ch: 1, a: 0, b: 0, c: 0 };
  const row = mkActionRow(
    a,
    () => row.remove(),
    () => markBtnDirty(),
    () => requestSaveButtonAfterFinish(),
    () => saveButtonImmediate()
  );
  listEl.appendChild(row);
  markBtnDirty();
  requestSaveButtonAfterFinish();
}

// ---------- mode UI (short/long/a+b/group led) ----------
function updateModeUI() {
  const pm = Number(must("pressMode").value || "0");

  // right pane visibility
  const pr = must("paneRight");
  pr.style.display = (pm === 0) ? "none" : "";

  // titles and add buttons
  const rightTitle = must("rightTitle");
  const leftTitle = must("leftTitle");
  const addRight = must("addRight");

  if (pm === 0) {
    leftTitle.textContent = "commands";
    addRight.style.display = "none";
  } else if (pm === 1) {
    leftTitle.textContent = "short";
    rightTitle.textContent = "long";
    addRight.style.display = "";
  } else if (pm === 2) {
    leftTitle.textContent = "a";
    rightTitle.textContent = "b";
    addRight.style.display = "";
  } else if (pm === 3) {
    leftTitle.textContent = "commands";
    rightTitle.textContent = "group led";
    addRight.style.display = "";
  }

  // a+b led selector
  const ab = must("abLedWrap");
  ab.style.display = (pm === 2) ? "" : "none";

  // ensure radio matches current config if loaded
  if (pm === 2 && MAP && MAP.abLedSel != null) {
    const v = Number(MAP.abLedSel) ? 1 : 0;
    must("abLedA").checked = (v === 0);
    must("abLedB").checked = (v === 1);
  }
}

function fsEditorUpdateMode(paneRight, addLeft, addRight, leftTitle, rightTitle, pressMode) {
  paneRight.style.display = (pressMode === 0) ? "none" : "";
  addRight.style.display = (pressMode === 0) ? "none" : "";

  if (pressMode === 0) {
    leftTitle.textContent = "commands";
  } else if (pressMode === 1) {
    leftTitle.textContent = "short";
    rightTitle.textContent = "long";
  } else {
    leftTitle.textContent = "a";
    rightTitle.textContent = "b";
  }
}

function tryAddRowExp(listEl, port) {
  const maxA = Number(META.maxActions || 20);
  const curCount = listEl.querySelectorAll(".action").length;
  if (curCount >= maxA) {
    setMsg("max actions reached", false);
    return;
  }

  const a = { type: "cc", ch: 1, a: 0, b: 0, c: 0 };
  const row = mkActionRow(
    a,
    () => row.remove(),
    () => markExpfsDirty(port),
    () => requestSaveExpfsAfterFinish(port),
    () => saveExpfsPortImmediate(port)
  );
  listEl.appendChild(row);
  markExpfsDirty(port);
  requestSaveExpfsAfterFinish(port);
}

// ---------- store -> UI ----------
async function loadMeta() {
  META = await apiGet("/api/meta");
}

async function loadLayout() {
  LAYOUT = await apiGet("/api/layout");

  // bank dropdown
  const sel = must("bankSelect");
  sel.innerHTML = "";
  const bc = Number(LAYOUT.bankCount || 1);
  for (let i = 0; i < bc; i++) {
    const b = (LAYOUT.banks || [])[i] || { name: `Bank ${i + 1}` };
    const opt = document.createElement("option");
    opt.value = String(i);
    opt.textContent = `#${i} · ${b.name || "bank"}`;
    sel.appendChild(opt);
  }
}

async function loadBankData(bank) {
  BANKDATA = await apiGet(`/api/bank?bank=${bank}`);
}

async function loadButton() {
  MAP = await apiGet(`/api/btn?bank=${cur.bank}&btn=${cur.btn}`);
  renderButtonConfig();
}

function renderButtonConfig() {
  if (!MAP) return;

  LOADING = true;
  try {
    // press mode
    must("pressMode").value = String(MAP.pressMode ?? 0);

    // ab led
    const ab = Number(MAP.abLedSel ?? 0);
    must("abLedA").checked = (ab === 0);
    must("abLedB").checked = (ab === 1);

    // actions
    renderActions(
      must("shortList"),
      MAP.short || [],
      () => markBtnDirty(),
      () => requestSaveButtonAfterFinish(),
      () => saveButtonImmediate()
    );
    renderActions(
      must("longList"),
      MAP.long || [],
      () => markBtnDirty(),
      () => requestSaveButtonAfterFinish(),
      () => saveButtonImmediate()
    );

    updateModeUI();
  } finally {
    LOADING = false;
  }
}

// ---------- UI -> store payload ----------
function readButtonFromUI() {
  const pm = Number(must("pressMode").value || "0");
  const shortArr = collectActions(must("shortList"));
  const longArr = (pm === 0) ? [] : collectActions(must("longList"));

  let abLedSel = 0;
  if (pm === 2) {
    abLedSel = must("abLedB").checked ? 1 : 0;
  }

  return { pressMode: pm, abLedSel, short: shortArr, long: longArr };
}

// ---------- dirty + autosave controllers ----------
function markBtnDirty() {
  if (LOADING) return;
  dirtyBtn = true;
  btnVer += 1;
  setMsg("editing… ✍️");
}

function markLayoutDirty() {
  if (LOADING) return;
  dirtyLayout = true;
  layoutVer += 1;
  setMsg("editing… ✍️");
}

function markBankDirty() {
  if (LOADING) return;
  dirtyBank = true;
  bankVer += 1;
  setMsg("editing… ✍️");
}

function requestSaveButtonAfterFinish() {
  if (LOADING) return;
  if (!dirtyBtn) return;
  if (btnSaving) return;

  btnSaving = true;
  btnSavePromise = (async () => {
    while (true) {
      const v = btnVer;
      try {
        await saveButton();
        if (btnVer === v) {
          dirtyBtn = false;
          setMsg("saved ✅");
          break;
        }
      } catch (e) {
        setMsg("save failed: " + e.message, false);
        break;
      }
      await new Promise((r) => setTimeout(r, 0));
    }
    btnSaving = false;
  })();
}

function requestSaveLayoutAfterFinish() {
  if (LOADING) return;
  if (!dirtyLayout) return;
  if (layoutSaving) return;

  layoutSaving = true;
  layoutSavePromise = (async () => {
    while (true) {
      const v = layoutVer;
      try {
        await saveLayout();
        if (layoutVer === v) {
          dirtyLayout = false;
          setMsg("saved ✅");
          break;
        }
      } catch (e) {
        setMsg("save failed: " + e.message, false);
        break;
      }
      await new Promise((r) => setTimeout(r, 0));
    }
    layoutSaving = false;
  })();
}

function requestSaveBankAfterFinish() {
  if (LOADING) return;
  if (!dirtyBank) return;
  if (bankSaving) return;

  bankSaving = true;
  bankSavePromise = (async () => {
    while (true) {
      const v = bankVer;
      try {
        await saveBank();
        if (bankVer === v) {
          dirtyBank = false;
          setMsg("saved ✅");
          break;
        }
      } catch (e) {
        setMsg("save failed: " + e.message, false);
        break;
      }
      await new Promise((r) => setTimeout(r, 0));
    }
    bankSaving = false;
  })();
}

// ---------- immediate save wrappers ----------
async function saveButtonImmediate() {
  if (LOADING) return;
  dirtyBtn = true;
  btnVer++;
  requestSaveButtonAfterFinish();
  return btnSavePromise;
}

async function saveLayoutImmediate() {
  if (LOADING) return;
  dirtyLayout = true;
  layoutVer++;
  requestSaveLayoutAfterFinish();
  await layoutSavePromise;
  renderHeader();
  refreshLayoutButtons();
  refreshBankDropdown();
}

async function saveBankImmediate() {
  if (LOADING) return;
  dirtyBank = true;
  bankVer++;
  requestSaveBankAfterFinish();
  await bankSavePromise;
  renderGridLabels();
  renderHeader();
}

// ---------- exp/fs autosave ----------
function markExpfsDirty(port) {
  if (LOADING) return;
  port = clampInt(port, 0, 1);
  expfsDirty[port] = true;
  expfsVer[port] += 1;
  setMsg("editing… ✍️");
}

function requestSaveExpfsAfterFinish(port) {
  if (LOADING) return;
  port = clampInt(port, 0, 1);
  if (!expfsDirty[port]) return;
  if (expfsSaving[port]) return;

  expfsSaving[port] = true;
  expfsSavePromise[port] = (async () => {
    while (true) {
      const v = expfsVer[port];
      try {
        await saveExpfsPort(port);
        if (expfsVer[port] === v) {
          expfsDirty[port] = false;
          setMsg("saved ✅");
          break;
        }
      } catch (e) {
        setMsg("save failed: " + e.message, false);
        break;
      }
      await new Promise((r) => setTimeout(r, 0));
    }
    expfsSaving[port] = false;
  })();
}

async function saveExpfsPortImmediate(port) {
  port = clampInt(port, 0, 1);
  await saveExpfsPort(port);
  expfsDirty[port] = false;
  setMsg("saved ✅");
}


// ---------- load/save ----------
async function loadMeta() {
  META = await apiGet("/api/meta");
}

function refreshLayoutButtons() {
  const bc = Number(LAYOUT.bankCount || 1);

  must("btnAddBank").disabled = bc >= Number(META.maxBanks || 100);
  must("btnDelBank").disabled = bc <= 1;
}

function refreshBankDropdown() {
  const sel = must("bankSelect");
  sel.innerHTML = "";

  const bc = Number(LAYOUT.bankCount || 1);
  for (let i = 0; i < bc; i++) {
    const b = (LAYOUT.banks || [])[i] || { name: `Bank ${i + 1}` };
    const opt = document.createElement("option");
    opt.value = String(i);
    opt.textContent = `#${i} · ${b.name || "bank"}`;
    sel.appendChild(opt);
  }

  sel.value = String(cur.bank);
}

async function saveButton() {
  const payload = readButtonFromUI();
  await apiPost(`/api/btn?bank=${cur.bank}&btn=${cur.btn}`, payload);
  MAP = payload;
}

async function saveLayout() {
  await apiPost("/api/layout", LAYOUT);
}

async function saveBank() {
  await apiPost(`/api/bank?bank=${cur.bank}`, BANKDATA);
}

async function gotoBank(b) {
  await flushPendingSaves();

  cur.bank = wrap(b, LAYOUT.bankCount || 1);
  cur.btn = clampInt(cur.btn, 0, Number(META.buttons || 8) - 1);

  lastUserNavAt = nowMs();

  await loadBankData(cur.bank);
  renderGridLabels();
  highlightGrid();
  renderHeader();
  refreshLayoutButtons();
  refreshBankDropdown();

  await loadButton();
}

function insertBankAfterCurrent() {
  const bc = Number(LAYOUT.bankCount || 1);
  const idx = clampInt(cur.bank + 1, 1, bc);

  const newBank = { index: idx, name: "new bank" };
  const banks = Array.isArray(LAYOUT.banks) ? LAYOUT.banks : [];
  banks.splice(idx, 0, newBank);

  // reindex
  banks.forEach((b, i) => b.index = i);
  LAYOUT.banks = banks;
  LAYOUT.bankCount = bc + 1;

  return idx;
}

function deleteCurrentBank() {
  const bc = Number(LAYOUT.bankCount || 1);
  if (bc <= 1) throw new Error("cannot delete last bank");

  const banks = Array.isArray(LAYOUT.banks) ? LAYOUT.banks : [];
  banks.splice(cur.bank, 1);

  banks.forEach((b, i) => b.index = i);
  LAYOUT.banks = banks;
  LAYOUT.bankCount = bc - 1;

  // adjust cur bank
  if (cur.bank >= LAYOUT.bankCount) cur.bank = LAYOUT.bankCount - 1;
}

// ---------- live state polling ----------
async function pollLive() {
  try {
    const st = await apiGet("/api/state");
    must("liveBank").textContent = String(st.bank ?? 0);
  } catch (_) {}
  setTimeout(pollLive, 800);
}

// ---------- exp/fs UI ----------
function mkSelect(opts, value) {
  const s = document.createElement("select");
  for (const [v, t] of opts) {
    const o = document.createElement("option");
    o.value = String(v);
    o.textContent = String(t);
    s.appendChild(o);
  }
  s.value = String(value ?? opts?.[0]?.[0] ?? "");
  return s;
}

function mkNumberInput(val, min, max, step) {
  const i = document.createElement("input");
  i.type = "number";
  i.value = String(val ?? 0);
  if (min != null) i.min = String(min);
  if (max != null) i.max = String(max);
  if (step != null) i.step = String(step);
  i.classList.add("mini");
  return i;
}

function buildFsEditor(port, side /*"tip"|"ring"*/, cfg) {
  const wrap = document.createElement("div");
  wrap.className = "pane";
  wrap.style.border = "1px solid var(--line)";
  wrap.style.borderRadius = "14px";
  wrap.style.padding = "12px";
  wrap.style.background = "rgba(255,255,255,.02)";

  const head = document.createElement("div");
  head.className = "paneHead";
  head.style.marginBottom = "10px";
  head.style.display = "flex";
  head.style.alignItems = "center";
  head.style.justifyContent = "space-between";
  head.style.gap = "12px";

  const title = document.createElement("div");
  title.className = "paneTitle";
  title.textContent = side;

  const pmSel = mkSelect(
    [["0","short"],["1","short + long"],["2","a + b"]],
    cfg?.pressMode ?? 0
  );

  pmSel.onchange = async () => {
    try {
      fsEditorUpdateMode(paneRight, addLeft, addRight, leftTitle, rightTitle, Number(pmSel.value||"0"));
      markExpfsDirty(port);
      await saveExpfsPortImmediate(port);
    } catch (e) { setMsg("save failed: " + e.message, false); }
  };

  head.appendChild(title);
  head.appendChild(mkField("press mode", pmSel));
  wrap.appendChild(head);

  const cmdGrid = document.createElement("div");
  cmdGrid.className = "cmdGrid";

  const paneLeft = document.createElement("div");
  paneLeft.className = "pane";
  paneLeft.style.border = "none";
  paneLeft.style.padding = "0";
  paneLeft.style.background = "transparent";

  const paneRight = document.createElement("div");
  paneRight.className = "pane";
  paneRight.style.border = "none";
  paneRight.style.padding = "0";
  paneRight.style.background = "transparent";

  const headL = document.createElement("div");
  headL.className = "paneHead";
  const leftTitle = document.createElement("div");
  leftTitle.className = "paneTitle";
  const addLeft = document.createElement("button");
  addLeft.className = "btn2";
  addLeft.type = "button";
  addLeft.onclick = () => tryAddRowExp(shortList, port);

  headL.append(leftTitle, addLeft);

  const headR = document.createElement("div");
  headR.className = "paneHead";
  const rightTitle = document.createElement("div");
  rightTitle.className = "paneTitle";
  const addRight = document.createElement("button");
  addRight.className = "btn2";
  addRight.type = "button";
  addRight.onclick = () => tryAddRowExp(longList, port);

  headR.append(rightTitle, addRight);

  const shortList = document.createElement("div");
  shortList.className = "list";
  const longList = document.createElement("div");
  longList.className = "list";

  renderActions(
    shortList,
    (cfg?.short || []),
    () => markExpfsDirty(port),
    () => requestSaveExpfsAfterFinish(port),
    () => saveExpfsPortImmediate(port)
  );
  renderActions(
    longList,
    (cfg?.long || []),
    () => markExpfsDirty(port),
    () => requestSaveExpfsAfterFinish(port),
    () => saveExpfsPortImmediate(port)
  );

  paneLeft.append(headL, shortList);
  paneRight.append(headR, longList);

  cmdGrid.append(paneLeft, paneRight);
  wrap.appendChild(cmdGrid);

  // init titles/visibility
  fsEditorUpdateMode(paneRight, addLeft, addRight, leftTitle, rightTitle, Number(pmSel.value||"0"));

  // expose getters
  wrap._get = () => {
    const pm = Number(pmSel.value||"0");
    const shortArr = collectActions(shortList);
    const longArr = (pm === 0) ? [] : collectActions(longList);
    return { pressMode: pm, ccBehavior: 0, short: shortArr, long: longArr };
  };

  return wrap;
}

function buildExpEditor(port, cfg) {
  const box = document.createElement("div");

  const row = document.createElement("div");
  row.className = "expRow";

  const typeSel = mkSelect([["cc","cc"],["pc","pc"]], (cfg?.exp?.cmd?.[0]?.type || "cc"));

  const chInp = mkNumberInput(cfg?.exp?.cmd?.[0]?.ch ?? 1, 1, 16, 1);

  const ccInp = mkNumberInput(cfg?.exp?.cmd?.[0]?.a ?? 0, 0, 127, 1);
  const v1Inp = mkNumberInput((cfg?.exp?.cmd?.[0]?.type==="cc") ? (cfg?.exp?.cmd?.[0]?.b ?? 0) : (cfg?.exp?.cmd?.[0]?.a ?? 0), 0, 127, 1);
  const v2Inp = mkNumberInput((cfg?.exp?.cmd?.[0]?.type==="cc") ? (cfg?.exp?.cmd?.[0]?.c ?? 127) : (cfg?.exp?.cmd?.[0]?.b ?? 127), 0, 127, 1);

  function refresh() {
    const t = typeSel.value;
    ccInp.parentElement.style.display = (t === "cc") ? "" : "none";
  }

  [typeSel, chInp, ccInp, v1Inp, v2Inp].forEach((el) => {
    el.addEventListener("change", () => { markExpfsDirty(port); requestSaveExpfsAfterFinish(port); });
    el.addEventListener("input", () => markExpfsDirty(port));
  });

  row.append(
    mkField("type", typeSel),
    mkField("ch", chInp),
    mkField("cc#", ccInp),
    mkField("val1", v1Inp),
    mkField("val2", v2Inp),
  );

  const calRow = document.createElement("div");
  calRow.className = "calRow";

  const calBtn = document.createElement("button");
  calBtn.className = "btn2";
  calBtn.type = "button";
  calBtn.textContent = "EXP Calibrate";
  calBtn.dataset.step = "0";

  const calLabel = document.createElement("span");
  calLabel.className = "calHint";
  calLabel.textContent = "";
  calLabel.style.display = "none";

  const vals = document.createElement("div");
  vals.className = "calVals";
  vals.textContent = `calMin=${cfg?.calMin ?? 0} · calMax=${cfg?.calMax ?? 4095}`;

  function setCalStep(step) {
    calBtn.dataset.step = String(step);
    calBtn.classList.remove("calNext", "calSave");

    if (step === 0) {
      calBtn.textContent = "EXP Calibrate";
      calLabel.style.display = "none";
      calLabel.textContent = "";
      return;
    }

    calLabel.style.display = "";
    if (step === 1) {
      calBtn.textContent = "Next ↑";
      calBtn.classList.add("calNext");
      calLabel.textContent = "↑ ยก exp ขึ้นสุด แล้วกด next";
    } else {
      calBtn.textContent = "Save ↓";
      calBtn.classList.add("calSave");
      calLabel.textContent = "↓ เหยียบ exp ลงสุด แล้วกด save";
    }
  }

  async function refreshCalValsFromServer() {
    try {
      const fresh = await apiGetExpfs(port);
      EXPFS[port] = fresh;
      vals.textContent = `calMin=${fresh?.calMin ?? 0} · calMax=${fresh?.calMax ?? 4095}`;
    } catch (_) {}
  }

  calBtn.onclick = async () => {
    const step = Number(calBtn.dataset.step || "0");

    if (step === 0) {
      // start wizard (no network call)
      setCalStep(1);
      return;
    }

    if (step === 1) {
      // save UP/MAX
      try {
        await apiPostExpfsCal(port, "max");
        await refreshCalValsFromServer();
        setMsg("cal up saved ✅");
        setCalStep(2);
      } catch (e) {
        setMsg("cal failed: " + e.message, false);
      }
      return;
    }

    // step 2: save DOWN/MIN
    try {
      await apiPostExpfsCal(port, "min");
      await refreshCalValsFromServer();
      setMsg("cal down saved ✅");
      setCalStep(0);
    } catch (e) {
      setMsg("cal failed: " + e.message, false);
    }
  };

  calRow.append(calBtn, calLabel, vals);
  box.append(row, calRow);

  refresh();

  box._get = () => {
    const t = typeSel.value;
    const ch = clampInt(chInp.value, 1, 16);
    const val1 = clampInt(v1Inp.value, 0, 127);
    const val2 = clampInt(v2Inp.value, 0, 127);

    let cmd;
    if (t === "cc") {
      const cc = clampInt(ccInp.value, 0, 127);
      cmd = { type: "cc", ch, a: cc, b: val1, c: val2 };
    } else {
      cmd = { type: "pc", ch, a: val1, b: val2, c: 0 };
    }
    return {
      kind: "exp",
      calMin: clampInt(EXPFS?.[port]?.calMin ?? (cfg?.calMin ?? 0), 0, 4095),
      calMax: clampInt(EXPFS?.[port]?.calMax ?? (cfg?.calMax ?? 4095), 0, 4095),
      exp: { cmd: [cmd] },
      tip: { pressMode: 0, ccBehavior: 0, short: [], long: [] },
      ring: { pressMode: 0, ccBehavior: 0, short: [], long: [] },
    };
  };

  box._setCalVals = (cmin, cmax) => {
    vals.textContent = `calMin=${cmin} · calMax=${cmax}`;
  };

  return box;
}

function buildExpfsPortUI(port, cfg) {
  const box = document.createElement("div");
  box.className = "expPort";

  const head = document.createElement("div");
  head.className = "expPortHead";

  const title = document.createElement("div");
  title.className = "expPortTitle";
  title.textContent = `port ${port + 1}`;

  const kindSel = mkSelect([["single","single fs"],["dual","dual fs"],["exp","exp"]], cfg?.kind || "single");

  head.append(title, mkField("mode", kindSel));
  box.appendChild(head);

  const body = document.createElement("div");
  box.appendChild(body);

  let expEd = null;
  let fsTip = null;
  let fsRing = null;

  function renderBody() {
    body.innerHTML = "";
    const k = kindSel.value;

    if (k === "exp") {
      expEd = buildExpEditor(port, cfg);
      body.appendChild(expEd);
    } else {
      expEd = null;
      const tipCfg = cfg?.tip || { pressMode: 0, short: [], long: [] };
      const ringCfg = cfg?.ring || { pressMode: 0, short: [], long: [] };

      const tipWrap = document.createElement("div");
      tipWrap.style.display = "grid";
      tipWrap.style.gap = "10px";
      const tipTitle = document.createElement("div");
      tipTitle.className = "hint";
      tipTitle.textContent = "tip";
      tipWrap.appendChild(tipTitle);

      fsTip = buildFsEditor(port, "tip", tipCfg);
      tipWrap.appendChild(fsTip);

      body.appendChild(tipWrap);

      if (k === "dual") {
        const ringWrap = document.createElement("div");
        ringWrap.style.display = "grid";
        ringWrap.style.gap = "10px";
        ringWrap.style.marginTop = "12px";

        const ringTitle = document.createElement("div");
        ringTitle.className = "hint";
        ringTitle.textContent = "ring";
        ringWrap.appendChild(ringTitle);

        fsRing = buildFsEditor(port, "ring", ringCfg);
        ringWrap.appendChild(fsRing);

        body.appendChild(ringWrap);
      } else {
        fsRing = null;
      }
    }
  }

  kindSel.onchange = async () => {
    try {
      cfg.kind = kindSel.value;
      renderBody();
      markExpfsDirty(port);
      await saveExpfsPortImmediate(port);
    } catch (e) { setMsg("save failed: " + e.message, false); }
  };

  renderBody();

  box._get = () => {
    const k = kindSel.value;

    if (k === "exp") {
      const v = expEd?._get();
      // keep current cal values from last load (server truth)
      v.calMin = EXPFS?.[port]?.calMin ?? v.calMin;
      v.calMax = EXPFS?.[port]?.calMax ?? v.calMax;
      return v;
    }

    const tip = fsTip?._get() || { pressMode: 0, ccBehavior: 0, short: [], long: [] };
    const ring = fsRing?._get() || { pressMode: 0, ccBehavior: 0, short: [], long: [] };

    return {
      kind: k,
      calMin: EXPFS?.[port]?.calMin ?? (cfg?.calMin ?? 0),
      calMax: EXPFS?.[port]?.calMax ?? (cfg?.calMax ?? 4095),
      exp: { cmd: [] },
      tip,
      ring,
    };
  };

  box._setCalVals = (cmin, cmax) => {
    if (expEd && expEd._setCalVals) expEd._setCalVals(cmin, cmax);
  };

  return box;
}

function readExpfsPortFromUI(port) {
  // IMPORTANT:
  // index.html has a "splitter" script that MOVES rendered .expPort nodes
  // out of #expfsWrap into #expfsPane1 / #expfsPane2.
  // So querying only inside #expfsWrap will fail after that move, making the
  // save path accidentally send the old cached EXPFS[port] instead of reading
  // the current UI state.
  const portEl = document.querySelector(`.expPort[data-port="${port}"]`);
  if (!portEl || !portEl._get) return EXPFS[port] || null;
  return portEl._get();
}

async function loadExpfs() {
  const ports = clampInt(META.expfsPorts ?? 2, 1, 2);
  for (let p = 0; p < ports; p++) {
    EXPFS[p] = await apiGetExpfs(p);
  }
  renderExpfsUI();
}

function renderExpfsUI() {
  const wrap = must("expfsWrap");
  wrap.innerHTML = "";

  const ports = clampInt(META.expfsPorts ?? 2, 1, 2);

  for (let p = 0; p < ports; p++) {
    const cfg = EXPFS[p] || { kind: "single", calMin: 0, calMax: 4095, exp: { cmd: [] }, tip: {}, ring: {} };
    const portUI = buildExpfsPortUI(p, cfg);
    portUI.dataset.port = String(p);

    // keep cal values visible
    portUI._setCalVals(cfg.calMin ?? 0, cfg.calMax ?? 4095);

    wrap.appendChild(portUI);
  }
}

async function saveExpfsPort(port) {
  const payload = readExpfsPortFromUI(port);
  if (!payload) return;

  // ensure required structure
  if (!payload.exp) payload.exp = { cmd: [] };
  if (!Array.isArray(payload.exp.cmd)) payload.exp.cmd = [];

  // persist
  await apiPostExpfs(port, payload);

  // update local snapshot
  EXPFS[port] = payload;
}

// ---------- UI wiring ----------
function setupUI() {
  must("bankMinus").onclick = async () => { await gotoBank(cur.bank - 1); };
  must("bankPlus").onclick  = async () => { await gotoBank(cur.bank + 1); };

  // dropdown
  must("bankSelect").onchange = async (e) => {
    const v = clampInt(e.target.value, 0, Math.max(0, (LAYOUT.bankCount || 1) - 1));
    await gotoBank(v);
  };

  must("btnAddBank").onclick = async () => {
    try {
      await flushPendingSaves();
      if (LAYOUT.bankCount >= (META.maxBanks || 100)) throw new Error("max banks reached");

      const newIdx = insertBankAfterCurrent();
      await saveLayoutImmediate();
      await gotoBank(newIdx);
      setMsg("added bank ✅");
    } catch (e) {
      setMsg("add bank failed: " + e.message, false);
    }
  };

  must("btnDelBank").onclick = async () => {
    try {
      await flushPendingSaves();
      const ok = confirm(`delete current bank? (#${cur.bank})`);
      if (!ok) return;

      deleteCurrentBank();
      await saveLayoutImmediate();
      await gotoBank(cur.bank);
      setMsg("deleted bank ✅");
    } catch (e) {
      setMsg("delete bank failed: " + e.message, false);
    }
  };

  const bankName = must("bankName");
  const switchName = must("switchName");

  bankName.oninput = () => {
    const b = curBankObj();
    if (!b) return;
    b.name = clipText(bankName.value, MAX_BANK_NAME);
    bankName.value = b.name;
    must("curBankName").textContent = b.name || "bank";
    markLayoutDirty();
    refreshBankDropdown();
  };
  hookFinishedTypingInput(bankName, () => {}, requestSaveLayoutAfterFinish);

  switchName.oninput = () => {
    const v = clipText(switchName.value, MAX_SWITCH_NAME);
    switchName.value = v;
    if (!Array.isArray(BANKDATA.switchNames)) BANKDATA.switchNames = [];
    BANKDATA.switchNames[cur.btn] = v || `SW${cur.btn + 1}`;
    renderGridLabels();
    markBankDirty();
  };
  hookFinishedTypingInput(switchName, () => {}, requestSaveBankAfterFinish);

  // a+b led radio (exclusive)
  const abA = must("abLedA");
  const abB = must("abLedB");
  function onAbChange() {
    if (LOADING) return;
    if (Number(must("pressMode").value || "0") !== 2) return;
    saveButtonImmediate().catch((e) => setMsg("save failed: " + e.message, false));
  }
  abA.addEventListener("change", onAbChange);
  abB.addEventListener("change", onAbChange);

  must("addLeft").onclick = () => tryAddRow(must("shortList"));
  must("addRight").onclick = () => tryAddRow(must("longList"));

  // (removed) manual reload buttons: UI now stays simpler; users can just refresh the page.

  must("pressMode").onchange = async () => {
    try {
      updateModeUI();
      await saveButtonImmediate();
    } catch (e) {
      setMsg("save failed: " + e.message, false);
    }
  };

  // import/export + firmware UI (if present)
  setupImportExport();
  setupFirmwareUpdate();
  loadFwInfo();
}

window.addEventListener("load", async () => {
  try {
    setMsg("init… ⚙️");
    await loadMeta();
    await loadLayout();
    await loadExpfs();

    setupUI();
    makeGrid();

    try {
      const st = await apiGet("/api/state");
      cur.bank = wrap(st.bank, LAYOUT.bankCount);
    } catch (_) {}

    await gotoBank(cur.bank);

    pollLive();
    setMsg("ready ✅");
  } catch (e) {
    try { setMsg("init failed: " + e.message, false); }
    catch (_) { alert("init failed: " + e.message); }
  }
});
