#pragma once
#include <Arduino.h>

// HTML stored in flash (PROGMEM) — zero heap allocation per call
static const char WEB_UI[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>DMX Remapper</title>
<link rel="icon" type="image/svg+xml" href="/favicon.svg">
<style>
  :root {
    --bg:      #0d0f12;
    --surface: #161a1f;
    --surface2:#1e242c;
    --border:  #2a2f38;
    --accent:  #ff6b1a;
    --accent2: #ffaa55;
    --cyan:    #22d3ee;
    --text:    #dde3ec;
    --muted:   #5a6070;
    --danger:  #e03333;
    --ok:      #2ecc71;
    --mono:    ui-monospace, 'SF Mono', 'Cascadia Mono', 'Consolas', monospace;
    --sans:    system-ui, -apple-system, 'Segoe UI', sans-serif;
  }

  * { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    background: var(--bg);
    color: var(--text);
    font-family: var(--sans);
    min-height: 100vh;
    display: flex;
    flex-direction: column;
  }

  /* Header */
  header {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 12px 24px;
    border-bottom: 1px solid var(--border);
    flex-shrink: 0;
  }

  .logo {
    width: 36px; height: 36px;
    background: var(--accent);
    border-radius: 6px;
    display: flex; align-items: center; justify-content: center;
    font-family: var(--mono);
    font-size: 15px;
    font-weight: bold;
    color: #000;
    flex-shrink: 0;
  }

  header h1 { font-size: 18px; font-weight: 700; letter-spacing: .04em; }
  header h1 span { color: var(--accent); }

  .dot {
    width: 7px; height: 7px;
    border-radius: 50%;
    flex-shrink: 0;
  }
  .dot-ok    { background: var(--ok);     animation: pulse 2s infinite; }
  .dot-in    { background: var(--accent); animation: pulse 1.5s infinite; }
  .dot-out   { background: var(--cyan);   animation: pulse 1.5s infinite; }

  @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:.3} }


  /* Tabs */
  .tabs {
    display: flex;
    border-bottom: 1px solid var(--border);
    padding: 0 24px;
    flex-shrink: 0;
  }

  .tab-btn {
    padding: 12px 22px;
    background: none;
    border: none;
    border-bottom: 2px solid transparent;
    color: var(--muted);
    font-family: var(--sans);
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: all .15s;
    margin-bottom: -1px;
    letter-spacing: .02em;
  }
  .tab-btn:hover  { color: var(--text); }
  .tab-btn.active { color: var(--accent); border-bottom-color: var(--accent); }
  .tab-btn.testing { color: var(--accent) !important; }

  .tab-panel { display: none; flex: 1; padding: 24px; overflow-y: auto; }
  .tab-panel.active { display: block; }

  .section-title {
    font-size: 10px;
    letter-spacing: .12em;
    text-transform: uppercase;
    color: var(--muted);
    margin-bottom: 14px;
  }

  /* Monitor */
  .monitor-cols {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 16px;
  }

  .monitor-panel {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 14px;
    transition: padding .2s;
  }
  .monitor-panel.collapsed { padding-bottom: 14px; }
  .monitor-panel.collapsed .dmx-grid { display: none; }

  .monitor-panel-title {
    font-family: var(--mono);
    font-size: 11px;
    letter-spacing: .08em;
    text-transform: uppercase;
    margin-bottom: 10px;
    display: flex;
    align-items: center;
    gap: 8px;
    color: var(--muted);
    cursor: pointer;
    user-select: none;
  }
  .monitor-panel.collapsed .monitor-panel-title { margin-bottom: 0; }

  .collapse-icon {
    margin-left: auto;
    font-size: 10px;
    transition: transform .2s;
    opacity: .5;
  }
  .monitor-panel.collapsed .collapse-icon { transform: rotate(-90deg); }

  .dmx-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(36px, 1fr));
    gap: 0;
  }

  .ch-cell {
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 3px;
    padding: 2px 1px 3px;
    text-align: center;
    font-family: var(--mono);
    font-size: 9px;
    color: var(--muted);
    position: relative;
    overflow: hidden;
    height: 38px;
  }

  .ch-cell .bar {
    position: absolute; bottom: 0; left: 0; right: 0;
    opacity: .2;
    transition: height .08s;
  }
  .bar-in  { background: var(--accent); }
  .bar-out { background: var(--cyan); }

  .ch-cell .val { position: relative; z-index: 1; display: block; line-height: 1.5; margin-top: 10px; color: var(--text); }
  .ch-cell .ch  { display: block; font-size: 7px; color: #6b7280; position: relative; z-index: 1; }

  /* Group label on the first cell */
  .grp-label {
    position: absolute;
    top: 1px; left: 0; right: 0;
    font-size: 6px;
    line-height: 1;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
    padding: 0 2px;
    z-index: 2;
    font-family: var(--sans);
    font-weight: 700;
    letter-spacing: .02em;
  }

  /* Hover group — all cells fade out, hovered group stays at full opacity */
  .dmx-grid.hovering .ch-cell          { opacity: .15; transition: opacity .12s; }
  .dmx-grid.hovering .ch-cell.hov      { opacity: 1; }
  /* Cross-highlight: the opposite grid receives .hovering-peer class from JS */
  .dmx-grid.hovering-peer .ch-cell     { opacity: .15; transition: opacity .12s; }
  .dmx-grid.hovering-peer .ch-cell.hov { opacity: 1; }

  /* Active test channel — amber outline on output cells */
  .ch-cell.tested { outline: 1px dashed #f59e0b; outline-offset: -2px; }
  .ch-cell.tested .bar { background: #f59e0b !important; opacity: .4; }

  /* Carte de test active */
  .test-card.is-tested { border-color: #f59e0b; background: rgba(245,158,11,.04); }
  .test-card.is-tested .test-card-title::after {
    content: ' · en test';
    font-size: 10px; font-weight: 400;
    color: #f59e0b; font-family: var(--mono);
  }

  /* ── Test ── */
  .test-groups { display: flex; flex-direction: column; gap: 10px; margin-top: 16px; }

  .test-card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 14px 16px;
    transition: border-color .2s;
    position: relative;
  }
  .test-card:focus-within { border-color: var(--accent); }
  .test-card.is-testing {
    border-color: var(--accent);
    box-shadow: 0 0 0 1px var(--accent);
  }

  .test-badge {
    display: none;
    position: absolute;
    top: -1px; right: 12px;
    background: var(--accent);
    color: #000;
    font-size: 9px;
    font-weight: 700;
    letter-spacing: .08em;
    text-transform: uppercase;
    padding: 2px 8px;
    border-radius: 0 0 5px 5px;
    font-family: var(--sans);
    animation: testpulse 1.4s ease-in-out infinite;
  }
  .test-card.is-testing .test-badge { display: block; }

  @keyframes testpulse {
    0%,100% { opacity: 1; }
    50%      { opacity: .35; }
  }

  .test-card-header {
    display: flex;
    align-items: center;
    gap: 10px;
    margin-bottom: 12px;
  }

  .test-card-dot {
    width: 10px; height: 10px;
    border-radius: 50%;
    flex-shrink: 0;
  }

  .test-card-title {
    font-family: var(--sans);
    font-size: 13px;
    font-weight: 600;
    flex: 1;
  }

  .test-card-channels {
    font-family: var(--mono);
    font-size: 11px;
    color: var(--muted);
  }

  .test-card-val {
    font-family: var(--mono);
    font-size: 20px;
    font-weight: 700;
    min-width: 44px;
    text-align: right;
  }

  .test-slider {
    -webkit-appearance: none;
    width: 100%;
    height: 6px;
    border-radius: 3px;
    background: var(--border);
    outline: none;
    cursor: pointer;
  }
  .test-slider::-webkit-slider-thumb {
    -webkit-appearance: none;
    width: 18px; height: 18px;
    border-radius: 50%;
    background: var(--accent);
    cursor: pointer;
    border: 2px solid var(--bg);
    box-shadow: 0 0 0 1px var(--accent);
  }

  .test-card-actions {
    display: flex;
    gap: 8px;
    margin-top: 10px;
  }

  .test-btn {
    padding: 5px 12px;
    border-radius: 5px;
    font-family: var(--mono);
    font-size: 11px;
    cursor: pointer;
    border: 1px solid var(--border);
    background: transparent;
    color: var(--muted);
    transition: all .15s;
  }
  .test-btn:hover { border-color: var(--accent); color: var(--accent); }
  .test-btn.full  { border-color: var(--ok); color: var(--ok); }
  .test-btn.full:hover { background: var(--ok); color: #000; }
  .test-btn.zero  { border-color: var(--danger); color: var(--danger); }
  .test-btn.zero:hover { background: var(--danger); color: #fff; }

  .test-manual {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 14px 16px;
    margin-bottom: 16px;
  }
  .test-manual-row {
    display: flex;
    gap: 10px;
    align-items: center;
    flex-wrap: wrap;
  }
  .test-manual input {
    background: var(--bg);
    border: 1px solid var(--border);
    color: var(--text);
    border-radius: 5px;
    padding: 6px 10px;
    font-family: var(--mono);
    font-size: 13px;
    width: 90px;
    outline: none;
  }
  .test-manual input:focus { border-color: var(--accent); }

  /* Config */
  #mapping-list { display: flex; flex-direction: column; gap: 8px; }

  .mapping-row {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 12px 14px;
    transition: border-color .15s;
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .mapping-row:focus-within { border-color: var(--accent); }

  .mapping-row-top {
    display: grid;
    grid-template-columns: 1fr 72px 72px 34px;
    gap: 8px;
    align-items: end;
  }

  .mapping-row-dests {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    align-items: center;
  }

  .dest-label {
    font-size: 10px;
    color: var(--muted);
    letter-spacing: .04em;
    width: 100%;
    margin-bottom: -2px;
  }

  .dest-tag {
    display: flex;
    align-items: center;
    gap: 4px;
    background: var(--surface2);
    border: 1px solid var(--border);
    border-radius: 5px;
    padding: 3px 4px 3px 8px;
    font-family: var(--mono);
    font-size: 12px;
    color: var(--text);
  }

  .dest-tag input {
    background: transparent;
    border: none;
    color: var(--text);
    font-family: var(--mono);
    font-size: 12px;
    width: 42px;
    outline: none;
    padding: 0;
  }

  .dest-tag-del {
    background: transparent;
    border: none;
    color: var(--muted);
    cursor: pointer;
    font-size: 11px;
    padding: 0 2px;
    line-height: 1;
    transition: color .15s;
  }
  .dest-tag-del:hover { color: var(--danger); }

  .dest-add-btn {
    background: transparent;
    border: 1px dashed var(--border);
    color: var(--muted);
    border-radius: 5px;
    padding: 3px 10px;
    font-size: 14px;
    cursor: pointer;
    transition: all .15s;
    line-height: 1.4;
  }
  .dest-add-btn:hover { border-color: var(--accent); color: var(--accent); }

  .row-label {
    display: block;
    font-size: 10px;
    color: var(--muted);
    margin-bottom: 4px;
    letter-spacing: .04em;
  }

  .mapping-row input {
    background: var(--bg);
    border: 1px solid var(--border);
    color: var(--text);
    border-radius: 5px;
    padding: 6px 8px;
    font-family: var(--mono);
    font-size: 12px;
    width: 100%;
    outline: none;
    transition: border-color .15s;
  }
  .mapping-row input:focus { border-color: var(--accent); }

  .del-btn {
    background: transparent;
    border: 1px solid var(--border);
    color: var(--muted);
    border-radius: 5px;
    width: 34px; height: 34px;
    cursor: pointer;
    font-size: 14px;
    transition: all .15s;
    display: flex; align-items: center; justify-content: center;
  }
  .del-btn:hover { border-color: var(--danger); color: var(--danger); background: rgba(224,51,51,.08); }

  .actions {
    display: flex;
    gap: 10px;
    margin-top: 20px;
    flex-wrap: wrap;
    align-items: center;
  }

  .btn {
    padding: 9px 20px;
    border-radius: 6px;
    font-family: var(--sans);
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: all .15s;
    border: none;
    white-space: nowrap;
  }
  .btn-primary { background: var(--accent); color: #000; }
  .btn-primary:hover { background: var(--accent2); }
  .btn-ghost { background: transparent; border: 1px solid var(--border); color: var(--text); }
  .btn-ghost:hover { border-color: var(--accent); color: var(--accent); }
  .btn-danger { background: transparent; border: 1px solid var(--border); color: var(--muted); margin-left: auto; }
  .btn-danger:hover { border-color: var(--danger); color: var(--danger); }

  /* Toast */
  #toast {
    position: fixed;
    bottom: 24px; left: 50%;
    transform: translateX(-50%) translateY(80px);
    background: var(--surface);
    border: 1px solid var(--border);
    color: var(--text);
    padding: 10px 22px;
    border-radius: 8px;
    font-family: var(--mono);
    font-size: 12px;
    transition: transform .3s ease;
    pointer-events: none;
    z-index: 999;
  }
  #toast.show { transform: translateX(-50%) translateY(0); }
  #toast.ok  { border-color: var(--ok);     color: var(--ok); }
  #toast.err { border-color: var(--danger); color: var(--danger); }

  footer {
    margin-top: auto;
    padding: 12px 24px;
    border-top: 1px solid var(--border);
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    font-size: 11px;
    color: var(--muted);
    flex-shrink: 0;
  }
  footer a { color: var(--accent); text-decoration: none; font-weight: 600; }
  footer a:hover { text-decoration: underline; }
  .footer-sep { opacity: .3; }

  @media (max-width: 700px) {
    .monitor-cols { grid-template-columns: 1fr; }
    .mapping-row  { grid-template-columns: 1fr 1fr; grid-template-rows: auto auto auto; }
  }
</style>
</head>
<body>

<header>
  <div class="logo">DMX</div>
  <h1>DMX <span>Remapper</span></h1>
</header>

<div class="tabs">
  <button class="tab-btn active" onclick="showTab('monitor', this)">Monitor</button>
  <button class="tab-btn"        onclick="showTab('config',  this)">Configuration</button>
  <button class="tab-btn"        onclick="showTab('test',    this)">Output test</button></div>

<!-- ONGLET MONITOR -->
<div class="tab-panel active" id="tab-monitor">
  <div class="section-title">DMX Channels — live 5 Hz</div>
  <div class="monitor-cols">
    <div class="monitor-panel" id="panel-in">
      <div class="monitor-panel-title" onclick="togglePanel('panel-in')">
        <span class="dot dot-in"></span>Input · console
        <span class="collapse-icon">▾</span>
      </div>
      <div class="dmx-grid" id="grid-in"></div>
    </div>
    <div class="monitor-panel" id="panel-out">
      <div class="monitor-panel-title" onclick="togglePanel('panel-out')">
        <span class="dot dot-out"></span>Output · DMX line
        <span class="collapse-icon">▾</span>
      </div>
      <div class="dmx-grid" id="grid-out"></div>
    </div>
  </div>
</div>

<!-- ONGLET CONFIGURATION -->
<div class="tab-panel" id="tab-config">
  <div class="section-title">Remap rules</div>
  <div id="mapping-list"></div>
  <div class="actions">
    <button class="btn btn-ghost"   onclick="addRow()">+ Add rule</button>
    <button class="btn btn-primary" onclick="saveConfig()">💾 Save</button>
    <button class="btn btn-danger"  onclick="resetConfig()">↺ Restart</button>
  </div>
</div>

<!-- ONGLET TEST SORTIE -->
<div class="tab-panel" id="tab-test">
  <div style="display:flex;align-items:center;margin-bottom:14px">
    <div class="section-title" style="margin:0">DMX output channel test</div>
    <button id="live-btn" onclick="resetTest()" disabled
      style="margin-left:auto;font-size:12px;padding:6px 14px;
             border-radius:6px;border:1px solid var(--border);
             background:transparent;color:var(--muted);cursor:default;
             font-family:var(--sans);font-weight:600;transition:all .2s">
      ↺ Back to live
    </button>
  </div>

  <!-- Channel manuel -->
  <div class="test-manual">
    <p style="font-size:12px;color:var(--muted);margin-bottom:10px">Free channel — send a value directly to any output channel</p>
    <div class="test-manual-row">
      <div><span class="row-label">Channel (1–512)</span>
        <input id="test-ch" type="number" min="1" max="512" value="1"></div>
      <div><span class="row-label">Value (0–255)</span>
        <input id="test-val" type="number" min="0" max="255" value="255"></div>
      <button class="btn btn-primary" onclick="sendTest()" style="align-self:flex-end">Send</button>
      <button class="btn btn-ghost"   onclick="sendTestCh(0)" style="align-self:flex-end">→ 0</button>
    </div>
  </div>

  <!-- Par groupe de rules -->
  <div class="section-title">By configured group</div>
  <div class="test-groups" id="test-groups"></div>

  <div id="test-result" style="font-family:var(--mono);font-size:12px;color:var(--muted);margin-top:12px;min-height:18px"></div>
</div>

<div id="toast"></div>

<script>
  let config = { mappings: [] };
  let dmxEventSource = null;

  function showTab(name, btn) {
    document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.getElementById('tab-' + name).classList.add('active');
    btn.classList.add('active');
    if (name === 'monitor') { startMonitor(); }
    else stopMonitor();
    if (name === 'test') restoreTestState();
  }

  function togglePanel(id) {
    document.getElementById(id).classList.toggle('collapsed');
  }

  function startMonitor() {
    buildGrids();
    startSse();
    document.addEventListener('visibilitychange', onVisibility);
  }

  function stopMonitor() {
    stopSse();
    document.removeEventListener('visibilitychange', onVisibility);
  }

  function startSse() {
    if (dmxEventSource) { dmxEventSource.close(); dmxEventSource = null; }
    dmxEventSource = new EventSource('/api/events');
    dmxEventSource.addEventListener('dmx', e => {
      try {
        const { in: vi, out: vo } = JSON.parse(e.data);
        for (let i = 0; i < 512; i++) {
          setCell('in',  i+1, vi[i]||0);
          setCell('out', i+1, vo[i]||0);
        }
      } catch(err) {}
    });
  }

  function stopSse() {
    if (dmxEventSource) { dmxEventSource.close(); dmxEventSource = null; }
  }

  function onVisibility() {
    if (document.hidden) stopSse();
    else                  startSse();
  }

  // Palette couleurs par rule (cyclique)
  const PALETTE = [
    '#ff6b1a','#22d3ee','#2ecc71','#a78bfa',
    '#f59e0b','#ec4899','#34d399','#60a5fa'
  ];

  function buildMappingMeta() {
    const inMap = {}, outMap = {};
    config.mappings.forEach((m, ri) => {
      const color = PALETTE[ri % PALETTE.length];
      const src   = m.src || 1;
      const nch   = m.channels || 1;
      // Source
      for (let c = 0; c < nch; c++) {
        const n = src + c;
        if (n >= 1 && n <= 512)
          inMap[n] = { color, isFirst: c === 0, isLast: c === nch-1, label: m.label||'', grp: ri };
      }
      // Destinations
      (m.dests || []).forEach(d => {
        for (let c = 0; c < nch; c++) {
          const n = d + c;
          if (n >= 1 && n <= 512)
            outMap[n] = { color, isFirst: c === 0, isLast: c === nch-1, label: m.label||'', grp: ri };
        }
      });
    });
    return { inMap, outMap };
  }

  function buildGrids() {
    const { inMap, outMap } = buildMappingMeta();
    ['in','out'].forEach(side => {
      const grid    = document.getElementById('grid-' + side);
      grid.innerHTML = '';
      const bc      = side === 'in' ? 'bar-in' : 'bar-out';
      const mapData = side === 'in' ? inMap : outMap;

      for (let i = 1; i <= 512; i++) {
        const cell = document.createElement('div');
        cell.className = 'ch-cell';
        cell.id = side + '-' + i;
        const meta = mapData[i];

        if (meta) {
          const { color, isFirst, isLast, label, grp } = meta;
          const bTop   = `2px solid ${color}`;
          const bBot   = `2px solid ${color}`;
          const bLeft  = isFirst ? `2px solid ${color}` : '1px solid transparent';
          const bRight = isLast  ? `2px solid ${color}` : '1px solid transparent';
          const radius = isFirst && isLast ? '3px'
                       : isFirst ? '3px 0 0 3px'
                       : isLast  ? '0 3px 3px 0' : '0';
          cell.style.cssText =
            `border-top:${bTop};border-bottom:${bBot};`
            + `border-left:${bLeft};border-right:${bRight};`
            + `border-radius:${radius};background:${color}18;`;
          cell.dataset.grp = grp;
          const lbl = (isFirst && label)
            ? `<span class="grp-label" style="color:${color}">${esc(label)}</span>` : '';
          cell.innerHTML = lbl
            + `<div class="bar ${bc}" id="b-${side}-${i}" style="height:0%"></div>`
            + `<span class="val" id="v-${side}-${i}">0</span>`
            + `<span class="ch">${i}</span>`;
        } else {
          cell.innerHTML =
            `<div class="bar ${bc}" id="b-${side}-${i}" style="height:0%"></div>`
            + `<span class="val" id="v-${side}-${i}">0</span>`
            + `<span class="ch">${i}</span>`;
        }
        grid.appendChild(cell);
      }

      // Hover: highlight hovered group on BOTH grids
      grid.addEventListener('mouseover', e => {
        const target = e.target.closest('.ch-cell');
        if (!target || target.dataset.grp === undefined) return;
        const grp = target.dataset.grp;
        // Grille courante
        grid.classList.add('hovering');
        grid.querySelectorAll('.ch-cell').forEach(c =>
          c.classList.toggle('hov', c.dataset.grp === grp));
        // Opposite grid
        const other = document.getElementById(side === 'in' ? 'grid-out' : 'grid-in');
        if (other) {
          other.classList.add('hovering-peer');
          other.querySelectorAll('.ch-cell').forEach(c =>
            c.classList.toggle('hov', c.dataset.grp === grp));
        }
      });
      grid.addEventListener('mouseleave', () => {
        grid.classList.remove('hovering');
        grid.querySelectorAll('.ch-cell').forEach(c => c.classList.remove('hov'));
        const other = document.getElementById(side === 'in' ? 'grid-out' : 'grid-in');
        if (other) {
          other.classList.remove('hovering-peer');
          other.querySelectorAll('.ch-cell').forEach(c => c.classList.remove('hov'));
        }
      });
    });
  }

  function setCell(side, ch, v) {
    const val = document.getElementById('v-' + side + '-' + ch);
    const bar = document.getElementById('b-' + side + '-' + ch);
    if (val) val.textContent = v;
    if (bar) bar.style.height = (v/255*100) + '%';
  }

  // ── Test state — persists across tab visits ──────────────────────────────────
  let testActive = false;
  const testVals = {};   // key: "ri-c", value: int 0-255

  function setLiveBtn(active) {
    testActive = active;
    const btn = document.getElementById('live-btn');
    if (btn) {
      btn.disabled      = !active;
      btn.style.cursor  = active ? 'pointer' : 'default';
      btn.style.borderColor = active ? 'var(--accent)' : 'var(--border)';
      btn.style.color   = active ? 'var(--accent)' : 'var(--muted)';
    }
    // Onglet Test — indication visuelle
    const tab = document.querySelector('.tab-btn[onclick*="test"]');
    if (tab) {
      tab.textContent = active ? 'Output test ●' : 'Output test';
      tab.classList.toggle('testing', active);
    }
  }

  function restoreTestState() {
    Object.entries(testVals).forEach(([key, v]) => {
      const sl = document.getElementById('sl-' + key);
      const vl = document.getElementById('sv-' + key);
      if (sl) sl.value = v;
      if (vl) vl.textContent = v;
      // Restore orange border if this group was being tested
      const ri = key.split('-')[0];
      const card = document.getElementById('tc-' + ri);
      if (card) card.classList.add('is-testing');
    });
  }

  function clearTested() {
    Object.keys(testVals).forEach(k => delete testVals[k]);
    setLiveBtn(false);
    document.querySelectorAll('.test-card').forEach(c => c.classList.remove('is-testing'));
    document.querySelectorAll('.test-slider').forEach(sl => {
      sl.value = 0;
      const vl = document.getElementById('sv-' + sl.id.slice(3));
      if (vl) vl.textContent = '0';
    });
  }

  async function resetTest() {
    if (!testActive) return;
    try {
      await fetch('/api/test/reset', { method: 'POST' });
      clearTested();
      showResult('↺ Output reset to current rules');
    } catch(e) { toast('\u2717 ESP32 injoignable', 'err'); }
  }

  // ── Construction des cartes (une seule fois par config) ───────────────────
  function buildTestGroups() {
    clearTested();
    const el = document.getElementById('test-groups');
    el.innerHTML = '';
    if (!config.mappings.length) {
      el.innerHTML = '<p style="color:var(--muted);font-size:13px">No rules configured.</p>';
      return;
    }
    config.mappings.forEach((m, ri) => {
      const color = PALETTE[ri % PALETTE.length];
      const nch   = m.channels || 1;
      const dests = m.dests || [];
      const byOffset = [];
      for (let c = 0; c < nch; c++) {
        const chans = dests.map(d => d + c).filter(n => n >= 1 && n <= 512);
        byOffset.push(chans);
      }
      if (!byOffset.length || byOffset[0].length === 0) return;

      const card = document.createElement('div');
      card.className = 'test-card';
      card.id = 'tc-' + ri;
      let html =
        '<span class="test-badge">en test\u2026</span>'
        + '<div class="test-card-header">'
        + '<span class="test-card-dot" style="background:' + color + '"></span>'
        + '<span class="test-card-title">' + esc(m.label || 'Group ' + (ri+1)) + '</span>'
        + '<div style="margin-left:auto;display:flex;gap:6px">'
        + '<button class="test-btn full" onclick="setGroup(' + ri + ',255)">\u25b2 Full</button>'
        + '<button class="test-btn zero" onclick="setGroup(' + ri + ',0)">\u25bc Zero</button>'
        + '<button class="test-btn"      onclick="setGroup(' + ri + ',128)">\u25c6 50%</button>'
        + '</div></div>';

      byOffset.forEach((chans, c) => {
        const key = ri + '-' + c;
        html +=
          '<div style="display:grid;grid-template-columns:32px 1fr 36px;gap:8px;align-items:center;margin-top:8px">'
          + '<span style="font-family:var(--mono);font-size:10px;color:' + color + ';text-align:right">ch' + (c+1) + '</span>'
          + '<input type="range" class="test-slider" id="sl-' + key + '" min="0" max="255" value="0">'
          + '<span id="sv-' + key + '" style="font-family:var(--mono);font-size:13px;font-weight:700;color:' + color + ';text-align:right">0</span>'
          + '</div>';
      });

      card.innerHTML = html;
      el.appendChild(card);

      byOffset.forEach((chans, c) => {
        const key = ri + '-' + c;
        const sl  = document.getElementById('sl-' + key);
        const vl  = document.getElementById('sv-' + key);
        if (!sl) return;
        sl.addEventListener('input', () => {
          const v = parseInt(sl.value);
          testVals[key] = v;
          if (vl) vl.textContent = v;
          chans.forEach(ch => sendRaw(ch, v));
          setLiveBtn(true);
          const card = document.getElementById('tc-' + ri);
          if (card) card.classList.add('is-testing');
        });
      });
    });
  }

  async function setGroup(ri, val) {
    const m   = config.mappings[ri];
    const nch = m.channels || 1;
    const allCh = [];
    (m.dests || []).forEach(d => { for (let c = 0; c < nch; c++) allCh.push(d + c); });
    for (const ch of allCh.filter(c => c >= 1 && c <= 512)) await sendRaw(ch, val);
    for (let c = 0; c < nch; c++) {
      const key = ri + '-' + c;
      testVals[key] = val;
      const sl = document.getElementById('sl-' + key);
      const vl = document.getElementById('sv-' + key);
      if (sl) sl.value = val;
      if (vl) vl.textContent = val;
    }
    setLiveBtn(true);
    const card = document.getElementById('tc-' + ri);
    if (card) card.classList.add('is-testing');
    showResult('\u2713 ' + (m.label||'Group') + ' \u2192 ' + val);
  }

  async function sendRaw(ch, val) {
    try {
      const fd = new FormData();
      fd.append('ch', ch); fd.append('val', val);
      await fetch('/api/test', { method: 'POST', body: fd });
    } catch(e) {}
  }

  async function sendTest() {
    const ch  = parseInt(document.getElementById('test-ch').value);
    const val = parseInt(document.getElementById('test-val').value);
    await sendTestCh(val, ch);
  }

  async function sendTestCh(val, ch) {
    if (!ch) ch = parseInt(document.getElementById('test-ch').value);
    if (isNaN(ch)||ch<1||ch>512)  { toast('✗ Invalid channel','err'); return; }
    if (isNaN(val)||val<0||val>255){ toast('✗ Invalid value','err'); return; }
    await sendRaw(ch, val);
    setLiveBtn(true);
    showResult('✓ Channel ' + ch + ' → ' + val);
  }

  function showResult(msg) {
    const el = document.getElementById('test-result');
    if (el) { el.style.color='var(--ok)'; el.textContent = msg; }
  }

  async function loadConfig() {
    const r = await fetch('/api/config');
    config = await r.json();
    // Sort destinations ascending for each rule
    config.mappings.forEach(m => {
      if (Array.isArray(m.dests)) m.dests.sort((a, b) => a - b);
    });
    renderTable();
    buildGrids();
    buildTestGroups();
  }

  function renderTable() {
    const list = document.getElementById('mapping-list');
    list.innerHTML = '';
    if (!config.mappings.length) {
      list.innerHTML = '<p style="color:var(--muted);font-size:13px;padding:16px 0">No rules — click « Add rule ».</p>';
      return;
    }
    config.mappings.forEach((m, i) => {
      const row = document.createElement('div');
      row.className = 'mapping-row';

      // Ligne du haut : nom, src, canaux, supprimer
      const top = document.createElement('div');
      top.className = 'mapping-row-top';
      top.innerHTML =
        '<div><span class="row-label">Name / Group</span>'
        + '<input type="text" value="' + esc(m.label) + '" placeholder="e.g. Rear PAR L"'
        + ' onchange="config.mappings[' + i + '].label=this.value"></div>'
        + '<div><span class="row-label">Src address</span>'
        + '<input type="number" min="1" max="512" value="' + m.src + '"'
        + ' onchange="config.mappings[' + i + '].src=+this.value"></div>'
        + '<div><span class="row-label">Channels</span>'
        + '<input type="number" min="1" max="64" value="' + m.channels + '"'
        + ' onchange="config.mappings[' + i + '].channels=+this.value"></div>'
        + '<button class="del-btn" title="Delete" onclick="deleteRow(' + i + ')">✕</button>';
      row.appendChild(top);

      // Ligne destinations — tags
      const destsRow = document.createElement('div');
      destsRow.className = 'mapping-row-dests';
      destsRow.id = 'dests-' + i;

      const lbl = document.createElement('span');
      lbl.className = 'dest-label';
      lbl.textContent = 'Destinations';
      destsRow.appendChild(lbl);

      renderDestTags(destsRow, i);
      row.appendChild(destsRow);
      list.appendChild(row);
    });
  }

  function renderDestTags(container, i) {
    // Supprime tout sauf le label
    while (container.children.length > 1) container.removeChild(container.lastChild);

    const m = config.mappings[i];
    (m.dests || []).forEach((d, di) => {
      const tag = document.createElement('div');
      tag.className = 'dest-tag';
      const inp = document.createElement('input');
      inp.type = 'number'; inp.min = 1; inp.max = 512; inp.value = d;
      inp.onchange = () => {
        config.mappings[i].dests[di] = parseInt(inp.value) || 1;
      };
      const del = document.createElement('button');
      del.className = 'dest-tag-del';
      del.textContent = '✕';
      del.title = 'Remove this destination';
      del.onclick = () => {
        config.mappings[i].dests.splice(di, 1);
        renderDestTags(container, i);
      };
      tag.appendChild(inp);
      tag.appendChild(del);
      container.appendChild(tag);
    });

    // Bouton "+" — propose la prochaine adresse logique
    const addBtn = document.createElement('button');
    addBtn.className = 'dest-add-btn';
    addBtn.textContent = '+';
    addBtn.title = 'Add destination';
    addBtn.onclick = () => {
      const dests = config.mappings[i].dests;
      const nch   = config.mappings[i].channels || 1;
      // Next address = last dest + channel count, or src if empty
      const next = dests.length
        ? (dests[dests.length - 1] + nch)
        : (config.mappings[i].src || 1);
      dests.push(Math.min(next, 512));
      renderDestTags(container, i);
    };
    container.appendChild(addBtn);
  }

  function esc(s) { return String(s).replace(/"/g,'&quot;'); }

  function addRow() {
    config.mappings.push({ label: 'New group', src: 1, channels: 8, dests: [] });
    renderTable();
    document.getElementById('mapping-list').lastElementChild?.scrollIntoView({ behavior:'smooth', block:'center' });
  }

  function deleteRow(i) {
    config.mappings.splice(i, 1);
    renderTable();
  }

  async function waitAndReload() {
    // Attend que l'ESP32 soit de nouveau disponible puis recharge la page
    await new Promise(r => setTimeout(r, 1500)); // laisse le temps de rebooter
    const poll = async () => {
      try {
        const r = await fetch('/api/ping', { cache: 'no-store' });
        if (r.ok) { location.reload(); return; }
      } catch(e) {}
      setTimeout(poll, 500);
    };
    poll();
  }

  function fireReboot() {
    // Fire reboot without awaiting response (ESP32 cuts before replying)
    fetch('/api/reboot', { method: 'POST' }).catch(() => {});
    waitAndReload();
  }

  async function saveConfig() {
    if (!confirm('Save configuration and restart the ESP32?')) return;
    try {
      const r = await fetch('/api/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(config)
      });
      const j = await r.json();
      if (j.status !== 'ok') { toast('✗ Save error', 'err'); return; }
      toast('✓ Saved — restarting…', 'ok');
      fireReboot();
    } catch(e) { toast('✗ ESP32 unreachable', 'err'); }
  }

  async function resetConfig() {
    if (!confirm('Restart the ESP32?')) return;
    toast('Restarting…', 'ok');
    fireReboot();
  }

  function toast(msg, type) {
    const el = document.getElementById('toast');
    el.textContent = msg;
    el.className = 'show ' + (type||'');
    setTimeout(() => { el.className = ''; }, 2800);
  }

  loadConfig();
  startMonitor();
</script>
<footer>
  <span>DMX Remapper v1.0.0</span>
  <span class="footer-sep">·</span>
  <span>by <a href="https://boherm.dev" target="_blank">boherm</a></span>
</footer>

</body>
</html>
)rawhtml";

// Returns a direct pointer to flash — zero heap allocation
inline const char* getWebUI() { return WEB_UI; }