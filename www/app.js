'use strict';

/*
 * R2T2 WebSDR page, laid out like the afu.tools remote station (same classes,
 * style sheets copied from afu.tools). Two faces:
 *  - opened directly: header, band buttons and scale, meter, waterfall with
 *    panel, bookmarks and chat, administration for logged-in users
 *  - embedded (inside an iframe, or ?embed): scale, spectrum and waterfall only,
 *    no audio. Compatible with how afu.tools/remote drives an external OpenWebRX:
 *    it loads <base>/#freq=<Hz>,mod=<usb|lsb|cw|am|nfm> and calls
 *    window.UI.setFrequency() / setModulation(). A click in the waterfall is
 *    reported to the parent page with postMessage.
 * All URLs are relative, so the page also works behind the afu-remote tunnel.
 */
/* el() as in afu.tools app.js; afu-tabelle.js needs it as a global */
function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (value === null || value === undefined || value === false) continue;
    if (key === 'class') node.className = value;
    else if (key === 'html') node.innerHTML = value;
    else if (key.startsWith('on') && typeof value === 'function') node.addEventListener(key.slice(2), value);
    else node.setAttribute(key, value);
  }
  for (const child of children.flat()) {
    if (child === null || child === undefined || child === false) continue;
    node.append(child.nodeType ? child : document.createTextNode(String(child)));
  }
  return node;
}

(() => {
  const $ = (id) => document.getElementById(id);
  const params = new URLSearchParams(location.search);
  const EMBED = params.has('embed') ? params.get('embed') !== '0' : window.self !== window.top;
  if (EMBED) document.documentElement.classList.add('embed');

  // Passband presets per mode: [lo, hi, label] relative to the dial frequency.
  const MODES = {
    lsb: { label: 'LSB', bws: [[-2700, -300, '2,4 kHz'], [-3000, -200, '2,8 kHz'], [-2200, -400, '1,8 kHz']] },
    usb: { label: 'USB', bws: [[300, 2700, '2,4 kHz'], [200, 3000, '2,8 kHz'], [400, 2200, '1,8 kHz']] },
    cw:  { label: 'CW',  bws: [[-250, 250, '500 Hz'], [-125, 125, '250 Hz'], [-500, 500, '1 kHz']] },
    am:  { label: 'AM',  bws: [[-4500, 4500, '9 kHz'], [-3000, 3000, '6 kHz'], [-6000, 6000, '12 kHz']] },
    fm:  { label: 'FM',  bws: [[-6000, 6000, '12 kHz'], [-7500, 7500, '15 kHz'], [-4000, 4000, '8 kHz']] },
  };
  const MOD_ALIAS = { usb: 'usb', lsb: 'lsb', cw: 'cw', am: 'am', sam: 'am', fm: 'fm', nfm: 'fm', wfm: 'fm' };
  const DEFAULT_STEP = { lsb: 100, usb: 100, cw: 10, am: 5000, fm: 5000 };
  const VIEW_GRID = 25000;     // free views snap their centre to this grid so viewers share them
  const VIEW_INNER = 0.85;     // re-centre once the frequency leaves this part of the span
  const PBKDF2_ITER = 10000;

  // Band edges for the band scale (IARU region 1, plus the broadcast bands).
  const BAND_EDGES = [
    ['160 m', 1810000, 2000000], ['80 m', 3500000, 3800000], ['60 m', 5351500, 5366500],
    ['49 m Rundfunk', 5900000, 6200000], ['40 m', 7000000, 7200000], ['41 m Rundfunk', 7200000, 7450000],
    ['31 m Rundfunk', 9400000, 9900000], ['30 m', 10100000, 10150000], ['25 m Rundfunk', 11600000, 12100000],
    ['20 m', 14000000, 14350000], ['17 m', 18068000, 18168000], ['15 m', 21000000, 21450000],
    ['12 m', 24890000, 24990000], ['11 m CB', 26565000, 27405000], ['10 m', 28000000, 29700000],
    ['6 m', 50000000, 52000000],
  ];

  const st = {
    cfg: null, ws: null, gotConfig: false,
    view: -1, viewBand: -1, center: 0, span: 192000, pending: null, userBand: null,
    freq: 0, mode: 'usb', bw: 0, step: 100,
    listening: false, wantAudio: false, muted: false,
    wfMin: 40, wfMax: 120, autoFrames: 8, smooth: null, lastBins: null,
    editing: false, bookmarks: [], bmBoxes: [],
    user: null, role: null, viewAnt: '', audioAnt: '', status: null,
    chat: [], online: false,
  };

  const PREFIX = EMBED ? 'r2t2e.' : 'r2t2.';
  const store = {
    get(k, d) { try { const v = localStorage.getItem(PREFIX + k); return v === null ? d : JSON.parse(v); } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem(PREFIX + k, JSON.stringify(v)); } catch (e) { /* storage unavailable */ } },
  };
  const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  // ------------------------------------------------------------ toasts (as on /remote)

  function toast(msg, kind = 'info') {
    if (EMBED) { console.warn('r2t2sdr:', msg); return; }
    const t = document.createElement('div');
    t.className = `rm-toast ${kind}`;
    t.textContent = msg;
    $('toasts').appendChild(t);
    setTimeout(() => t.remove(), 6000);
  }

  // ------------------------------------------------------------ colour map (same as /remote)

  const palette = (() => {
    const stops = [
      [0.00, 0, 0, 0], [0.15, 0, 0, 90], [0.35, 0, 70, 210], [0.55, 0, 200, 210],
      [0.70, 235, 230, 0], [0.85, 255, 120, 0], [1.00, 255, 255, 255],
    ];
    const p = new Uint8Array(256 * 3);
    for (let i = 0; i < 256; i++) {
      const x = i / 255;
      let k = 0;
      while (k < stops.length - 2 && x > stops[k + 1][0]) k++;
      const a = stops[k], b = stops[k + 1];
      const t = (x - a[0]) / (b[0] - a[0]);
      for (let c = 0; c < 3; c++) p[i * 3 + c] = Math.round(a[c + 1] + (b[c + 1] - a[c + 1]) * t);
    }
    return p;
  })();

  // ------------------------------------------------------------ canvases

  const spec = $('spectrum'), wf = $('waterfall');
  const specCtx = spec.getContext('2d'), wfCtx = wf.getContext('2d');
  // One waterfall row covers the whole view; with several receivers side by side
  // (widened band) each segment's bins are mapped into its slice of the row.
  const ROW = 2048;
  const hist = document.createElement('canvas');
  hist.width = ROW;
  hist.height = 400;
  const histCtx = hist.getContext('2d');
  const line = histCtx.createImageData(ROW, 1);
  function clearHistory() {
    histCtx.fillStyle = '#000';
    histCtx.fillRect(0, 0, hist.width, hist.height);
    st.smooth = null;
    st.autoFrames = 8;
  }
  clearHistory();

  const dpr = () => Math.min(window.devicePixelRatio || 1, 2);
  function fitCanvas(c) {
    const w = Math.max(1, Math.round(c.clientWidth * dpr()));
    const h = Math.max(1, Math.round(c.clientHeight * dpr()));
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  }

  function resize() {
    [spec, wf].forEach(fitCanvas);
    wfCtx.imageSmoothingEnabled = false;
    drawWaterfall();
    drawWfScale();
    drawSpectrum();
    updatePassband();
    if (!EMBED) drawRibbon();
  }

  function freqToX(f, w) { return ((f - (st.center - st.span / 2)) / st.span) * w; }
  function xToFreq(x, w) { return st.center - st.span / 2 + (x / w) * st.span; }

  function drawWaterfall() {
    wfCtx.drawImage(hist, 0, 0, hist.width, hist.height, 0, 0, wf.width, wf.height);
  }

  function pushWaterfall(bins) {
    histCtx.drawImage(hist, 0, 0, hist.width, hist.height - 1, 0, 1, hist.width, hist.height - 1);
    const lo = st.wfMin, range = Math.max(1, st.wfMax - st.wfMin);
    const d = line.data;
    for (let i = 0; i < bins.length; i++) {
      let v = Math.round(((bins[i] - lo) / range) * 255);
      v = v < 0 ? 0 : v > 255 ? 255 : v;
      d[i * 4] = palette[v * 3];
      d[i * 4 + 1] = palette[v * 3 + 1];
      d[i * 4 + 2] = palette[v * 3 + 2];
      d[i * 4 + 3] = 255;
    }
    histCtx.putImageData(line, 0, 0);
    drawWaterfall();
  }

  function passbandRange() {
    const bw = MODES[st.mode].bws[st.bw];
    if (!bw || !st.center) return null;
    return [st.freq + bw[0], st.freq + bw[1]];
  }

  // spectrum as on /remote: dark ground, faint grid, one line
  function drawSpectrum() {
    const w = spec.width, h = spec.height, r = dpr();
    specCtx.fillStyle = '#05070f';
    specCtx.fillRect(0, 0, w, h);
    const lo = st.wfMin - 10, hi = st.wfMax + 20, range = hi - lo;
    specCtx.strokeStyle = 'rgba(120,140,200,0.25)';
    specCtx.lineWidth = 1;
    specCtx.fillStyle = 'rgba(138,147,179,0.85)';
    specCtx.font = `${Math.round(10 * r)}px system-ui, sans-serif`;
    specCtx.textBaseline = 'alphabetic';
    for (let db = Math.ceil(lo / 10) * 10; db <= hi; db += 10) {
      const y = Math.round(h - ((db - lo) / range) * h) + 0.5;
      specCtx.beginPath(); specCtx.moveTo(0, y); specCtx.lineTo(w, y); specCtx.stroke();
      if (h > 60 * r) specCtx.fillText(`${db - 170}`, 4, y - 2);
    }
    const pb = passbandRange();
    if (pb) {
      const x0 = freqToX(pb[0], w), x1 = freqToX(pb[1], w);
      specCtx.fillStyle = 'rgba(245,217,10,0.12)';
      specCtx.fillRect(x0, 0, Math.max(1, x1 - x0), h);
      specCtx.fillStyle = '#ef4444';
      specCtx.fillRect(Math.round(freqToX(st.freq, w)), 0, Math.max(1, Math.round(r)), h);
    }
    drawSpectrumBookmarks(w, h);
    const s = st.smooth;
    if (!s) return;
    specCtx.beginPath();
    for (let i = 0; i < s.length; i++) {
      const x = (i / (s.length - 1)) * w;
      const y = h - Math.max(0, Math.min(1, (s[i] - lo) / range)) * h;
      if (i === 0) specCtx.moveTo(x, y); else specCtx.lineTo(x, y);
    }
    specCtx.strokeStyle = '#7c9cff';
    specCtx.lineWidth = 1.5 * r;
    specCtx.stroke();
  }

  // bookmarks in the spectrum: small yellow flags like on the band scale
  function drawSpectrumBookmarks(w, h) {
    st.bmBoxes = [];
    if (!st.center || !st.bookmarks.length) return;
    const r = dpr();
    specCtx.font = `600 ${Math.round(10 * r)}px system-ui, sans-serif`;
    specCtx.textBaseline = 'middle';
    let row = 0, lastEnd = -Infinity;
    for (const b of st.bookmarks) {
      const x = freqToX(b.freq, w);
      if (x < 0 || x > w) continue;
      specCtx.strokeStyle = 'rgba(181,181,22,0.55)';
      specCtx.setLineDash([3 * r, 3 * r]);
      specCtx.beginPath();
      specCtx.moveTo(Math.round(x) + 0.5, 0);
      specCtx.lineTo(Math.round(x) + 0.5, h);
      specCtx.stroke();
      specCtx.setLineDash([]);
      const tw = specCtx.measureText(b.name).width + 8 * r, th = 14 * r;
      const x0 = Math.min(Math.max(0, x - tw / 2), w - tw);
      row = x0 < lastEnd ? (row + 1) % 3 : 0;
      if (row === 0) lastEnd = -Infinity;
      const y0 = 2 * r + row * (th + 2 * r);
      specCtx.fillStyle = '#b5b516';
      specCtx.beginPath();
      if (specCtx.roundRect) specCtx.roundRect(x0, y0, tw, th, 4 * r); else specCtx.rect(x0, y0, tw, th);
      specCtx.fill();
      specCtx.fillStyle = '#0a0e1f';
      specCtx.fillText(b.name, x0 + 4 * r, y0 + th / 2);
      lastEnd = Math.max(lastEnd, x0 + tw);
      st.bmBoxes.push({ x0, x1: x0 + tw, y0, y1: y0 + th, b });
    }
  }

  function bookmarkAt(ev) {
    const rect = spec.getBoundingClientRect();
    const sx = spec.width / rect.width, sy = spec.height / rect.height;
    const x = (ev.clientX - rect.left) * sx, y = (ev.clientY - rect.top) * sy;
    const hit = st.bmBoxes.find((k) => x >= k.x0 && x <= k.x1 && y >= k.y0 && y <= k.y1);
    return hit ? hit.b : null;
  }

  // frequency labels above the spectrum: HTML spans as on /remote
  function drawWfScale() {
    const box = $('wfskala');
    if (!st.center) { box.innerHTML = ''; return; }
    const w = box.clientWidth || 800;
    const start = st.center - st.span / 2, end = st.center + st.span / 2;
    const pxPerKHz = (w / st.span) * 1000;
    const major = [1, 2, 5, 10, 20, 25, 50].find((k) => k * pxPerKHz > 70) || 100;
    const parts = [];
    for (let f = Math.ceil(start / (major * 1000)) * major * 1000; f <= end; f += major * 1000) {
      const pct = ((f - start) / st.span) * 100;
      if (pct < 2 || pct > 98) continue;
      parts.push(`<span style="left:${pct}%">${(f / 1000).toFixed(0)}</span>`);
    }
    box.innerHTML = parts.join('');
  }

  function updatePassband() {
    const el = $('passband');
    const pb = passbandRange();
    if (!pb) { el.hidden = true; return; }
    const w = wf.clientWidth;
    const x0 = freqToX(pb[0], w), x1 = freqToX(pb[1], w);
    if (x1 < 0 || x0 > w) { el.hidden = true; return; }
    el.hidden = false;
    el.style.left = `${x0}px`;
    el.style.width = `${Math.max(2, x1 - x0)}px`;
  }

  function autoLevels(bins) {
    const s = Array.from(bins).filter((v) => v > 0).sort((a, b) => a - b);
    if (!s.length) return;
    const p = (q) => s[Math.min(s.length - 1, Math.floor(q * s.length))];
    st.wfMin = Math.max(0, p(0.2) - 6);
    st.wfMax = Math.min(255, Math.max(st.wfMin + 20, p(0.995) + 6));
    $('wfmin').value = st.wfMin;
    $('wfmax').value = st.wfMax;
    showLevels();
  }

  function showLevels() {
    $('wfmin-wert').textContent = `${st.wfMin - 170} dB`;
    $('wfmax-wert').textContent = `${st.wfMax - 170} dB`;
  }

  // per segment: which row pixels it fills and from which of its 1024 bins
  function buildSegMaps() {
    const lo = st.center - st.span / 2;
    st.segMaps = (st.segs || []).map((g) => {
      const x0 = Math.max(0, Math.floor(((g.lo - lo) / st.span) * ROW));
      const x1 = Math.min(ROW, Math.ceil(((g.hi - lo) / st.span) * ROW));
      const idx = new Int16Array(Math.max(0, x1 - x0));
      for (let x = x0; x < x1; x++) {
        const f = lo + ((x + 0.5) / ROW) * st.span;
        idx[x - x0] = Math.max(0, Math.min(1023, Math.floor(((f - g.center) / st.cfg.span + 0.5) * 1024)));
      }
      return { x0, idx };
    });
    st.row = new Uint8Array(ROW);
  }

  function onSegment(seg, bins) {
    const m = st.segMaps && st.segMaps[seg];
    if (!m) return;
    for (let i = 0; i < m.idx.length; i++) st.row[m.x0 + i] = bins[m.idx[i]];
    // segment 0 sets the pace: one row per frame of the first receiver
    if (seg === 0) onWaterfall(st.row.slice());
  }

  function onWaterfall(bins) {
    if (st.autoFrames > 0 && --st.autoFrames === 0) autoLevels(bins);
    if (!st.smooth || st.smooth.length !== bins.length) st.smooth = Float32Array.from(bins);
    else for (let i = 0; i < bins.length; i++) st.smooth[i] += (bins[i] - st.smooth[i]) * 0.35;
    st.lastBins = bins;
    pushWaterfall(bins);
    drawSpectrum();
  }

  // ------------------------------------------------------------ band buttons and band scale

  function bandEdges(f) {
    const e = BAND_EDGES.find(([, lo, hi]) => f >= lo && f <= hi);
    return e ? { name: e[0], lo: e[1], hi: e[2] } : null;
  }

  function renderBandButtons() {
    const box = $('baender');
    box.innerHTML = '';
    for (const b of st.cfg.bands) {
      const k = document.createElement('button');
      k.type = 'button';
      k.className = 'btn-sm';
      k.textContent = b.name.replace(/(\d)m\b/, '$1 m');
      k.dataset.title = `${b.name}, Wasserfall um ${(b.center / 1e6).toFixed(3)} MHz`;
      k.title = k.dataset.title;
      k.dataset.band = b.id;
      k.addEventListener('click', () => { st.userBand = b.id; requestBand(b.id); });
      box.appendChild(k);
    }
    markBand();
  }

  function markBand() {
    document.querySelectorAll('#baender .btn-sm').forEach((k) =>
      k.setAttribute('aria-current', String(Number(k.dataset.band) === st.viewBand)));
    markActive();
  }

  // Bands someone is receiving right now: listening there costs no receiver.
  function activeList() { return (st.status && st.status.active) || []; }

  function activeLabel(a) {
    if (a.band >= 0 && st.cfg.bands[a.band]) return st.cfg.bands[a.band].name.replace(/(\d)m\b/, '$1 m');
    return `${mhzLabel(a.lo, 1e4)}–${mhzLabel(a.hi, 1e4)} MHz`;
  }

  function markActive() {
    if (EMBED || !st.cfg) return;
    const act = activeList();
    document.querySelectorAll('#baender .btn-sm').forEach((k) => {
      const a = act.find((x) => x.band === Number(k.dataset.band));
      k.classList.toggle('sdr-aktiv', Boolean(a));
      k.title = a ? `${k.dataset.title}. Empfänger aktiv (${a.viewers + a.listeners} dabei): hier hören kostet keinen Empfänger.`
        : k.dataset.title;
    });
    const note = $('belegt');
    const full = st.status && st.status.free === 0;
    note.hidden = !full && st.view >= 0;
    if (note.hidden) return;
    // first sentence of the server's reason; the list below says the rest
    const head = (st.view < 0 && st.noViewReason ? st.noViewReason : 'Alle Empfänger sind belegt.').split(/(?<=\.) /)[0];
    note.replaceChildren(el('span', {}, act.length ? `${head} Ohne eigenen Empfänger hören geht hier:` : head));
    for (const a of act) {
      note.append(' ', el('button', { type: 'button', class: 'btn-sm', onclick: () => {
        if (a.band >= 0) { st.userBand = a.band; requestBand(a.band); } else tuneTo(a.center, true);
      } }, activeLabel(a)));
    }
  }

  function niceStep(x) {
    const p = 10 ** Math.floor(Math.log10(x));
    for (const m of [1, 2, 2.5, 5, 10]) if (m * p >= x) return m * p;
    return 10 * p;
  }
  function mhzLabel(hz, step) {
    const dec = step >= 1e6 ? 0 : step >= 1e5 ? 1 : step >= 1e4 ? 2 : 3;
    return (hz / 1e6).toFixed(dec).replace('.', ',');
  }

  // SVG like ribbonZeichnen() on /remote: ruler, passband at the dial, red dial
  // line, the part the waterfall shows, bookmark flags as buttons
  function drawRibbon() {
    const part = $('skala-teil');
    if (!st.freq) { part.hidden = true; return; }
    part.hidden = false;
    const band = bandEdges(st.freq);
    let lo, hi;
    if (band) { lo = band.lo; hi = band.hi; } else { lo = st.center - st.span / 2; hi = st.center + st.span / 2; }
    // keep the waterfall window visible
    if (st.center) { lo = Math.min(lo, st.center - st.span / 2); hi = Math.max(hi, st.center + st.span / 2); }
    const span = hi - lo;
    $('ribbon-name').textContent = band ? (/Rundfunk|CB/.test(band.name) ? band.name : `${band.name}-Band`) : 'Ausschnitt';
    $('ribbon-grenzen').textContent = `${mhzLabel(lo, span / 10)} bis ${mhzLabel(hi, span / 10)} MHz`;

    const skala = $('skala');
    const W = Math.max(200, skala.clientWidth), H = 70;
    const x = (hz) => ((hz - lo) / span) * W;
    const ns = 'http://www.w3.org/2000/svg';
    const svg = document.createElementNS(ns, 'svg');
    svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
    svg.setAttribute('width', W);
    svg.setAttribute('height', H);
    const add = (tag, attrs, text) => {
      const n = document.createElementNS(ns, tag);
      for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
      if (text != null) n.textContent = text;
      svg.append(n);
      return n;
    };
    for (const a of activeList()) {
      if (a.hi < lo || a.lo > hi || a.view === st.view) continue;
      const r = add('rect', { x: Math.max(0, x(a.lo)), y: 9, height: 4, rx: 2,
        width: Math.max(2, Math.min(W, x(a.hi)) - Math.max(0, x(a.lo))), class: 'sdr-aktiv-bereich' });
      const t = document.createElementNS(ns, 'title');
      t.textContent = `Empfänger aktiv: ${activeLabel(a)}`;
      r.append(t);
    }
    if (st.center && st.view >= 0) {
      add('rect', { x: x(st.center - st.span / 2), y: 0, height: 7, rx: 2,
        width: Math.max(2, x(st.center + st.span / 2) - x(st.center - st.span / 2)), class: 'sdr-ausschnitt' });
    }
    const big = niceStep(span / 9), small = big / 5;
    for (let t = Math.ceil(lo / small) * small; t <= hi; t += small) {
      const major = Math.abs(t / big - Math.round(t / big)) < 1e-6;
      add('line', { x1: x(t), x2: x(t), y1: major ? 51 : 55, y2: 59, stroke: 'currentColor', 'stroke-width': major ? 1.5 : 1, opacity: major ? 0.9 : 0.5 });
      if (major) add('text', { x: x(t), y: 69, 'text-anchor': x(t) < 30 ? 'start' : x(t) > W - 30 ? 'end' : 'middle', class: 'rm-skala-text' }, mhzLabel(t, big));
    }
    const pb = passbandRange();
    if (pb && st.freq >= lo && st.freq <= hi) {
      const [pl, ph] = [pb[0] - st.freq, pb[1] - st.freq];
      const a = x(st.freq + pl), e = x(st.freq + ph), slope = Math.min(6, (e - a) / 4);
      add('path', { d: `M${a},50 L${a + slope},41 L${e - slope},41 L${e},50`, fill: 'none', stroke: '#f5d90a', 'stroke-width': 2 });
      const lbl = (hz) => (hz > 0 ? '+' : '') + (Math.abs(hz) >= 1000 ? `${Math.round(hz / 100) / 10}k` : String(Math.round(hz)));
      add('text', { x: a - 3, y: 49, 'text-anchor': 'end', class: 'rm-skala-durchlass' }, lbl(pl));
      add('text', { x: e + 3, y: 49, 'text-anchor': 'start', class: 'rm-skala-durchlass' }, lbl(ph));
      add('line', { x1: x(st.freq), x2: x(st.freq), y1: 16, y2: 59, stroke: '#ef4444', 'stroke-width': 2 });
    }
    skala.replaceChildren(svg);
    for (const bm of st.bookmarks) {
      if (bm.freq < lo || bm.freq > hi) continue;
      const k = document.createElement('button');
      k.type = 'button';
      k.className = 'rm-marke';
      k.style.left = `${(x(bm.freq) / W) * 100}%`;
      k.title = `${bm.name} · ${(bm.freq / 1e6).toFixed(4)} MHz ${bm.mode.toUpperCase()}`;
      k.textContent = bm.name;
      k.addEventListener('click', (ev) => { ev.stopPropagation(); gotoBookmark(bm); });
      skala.appendChild(k);
    }
    st.ribbon = { lo, hi };
  }

  function ribbonHz(ev) {
    const r = $('skala').getBoundingClientRect();
    return st.ribbon.lo + ((ev.clientX - r.left) / r.width) * (st.ribbon.hi - st.ribbon.lo);
  }

  // ------------------------------------------------------------ audio

  const IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];
  const IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767];

  let actx = null, gainNode = null, nextTime = 0;

  function imaDecode(bytes, pred, index, out) {
    let o = 0;
    for (let i = 0; i < bytes.length; i++) {
      const b = bytes[i];
      for (let half = 0; half < 2; half++) {
        const code = half ? b >> 4 : b & 15;
        const step = IMA_STEP[index];
        let diff = step >> 3;
        if (code & 4) diff += step;
        if (code & 2) diff += step >> 1;
        if (code & 1) diff += step >> 2;
        pred += code & 8 ? -diff : diff;
        if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
        index += IMA_INDEX[code];
        if (index < 0) index = 0; else if (index > 88) index = 88;
        out[o++] = pred / 32768;
      }
    }
  }

  function setVolume() {
    const v = Number($('vol').value) / 100;
    if (gainNode) gainNode.gain.value = st.muted ? 0 : v * v * 2;
    $('vol-wert').textContent = `${$('vol').value} %`;
    store.set('vol', $('vol').value);
  }

  function setMuted(m) {
    st.muted = m;
    $('stumm').setAttribute('aria-pressed', String(m));
    setVolume();
  }

  function onAudio(buf) {
    const dv = new DataView(buf);
    updateMeter(dv.getInt16(1, true) / 10);
    if (!actx || !st.listening) return;
    const pred = dv.getInt16(3, true), index = dv.getUint8(5);
    const bytes = new Uint8Array(buf, 6);
    const rate = (st.cfg && st.cfg.audioRate) || 8000;
    const ab = actx.createBuffer(1, bytes.length * 2, rate);
    imaDecode(bytes, pred, index, ab.getChannelData(0));
    const now = actx.currentTime;
    if (nextTime < now + 0.03) nextTime = now + 0.25;   // underrun: rebuild the buffer
    if (nextTime > now + 1.0) return;                    // too much latency: drop
    const src = actx.createBufferSource();
    src.buffer = ab;
    src.connect(gainNode);
    src.start(nextTime);
    nextTime += ab.duration;
  }

  function updateMeter(db) {
    const bar = $('rm-balken-s');
    bar.style.setProperty('--breite', `${bar.clientWidth}px`);
    const pct = Math.max(0, Math.min(100, ((db + 130) / 110) * 100));
    $('smbar').style.width = `${pct}%`;
    $('smval').textContent = `${db.toFixed(0)} dBFS`;
  }

  async function toggleAudio() {
    if (st.listening || st.wantAudio) {
      st.wantAudio = false;
      send({ cmd: 'stop' });
      return;
    }
    if (!actx) {
      const Ctx = window.AudioContext || window.webkitAudioContext;
      if (!Ctx) { toast('Dieser Browser kann keinen Ton wiedergeben.', 'fehler'); return; }
      actx = new Ctx();
      gainNode = actx.createGain();
      gainNode.connect(actx.destination);
      setVolume();
    }
    if (actx.state === 'suspended') await actx.resume();
    st.wantAudio = true;
    nextTime = 0;
    send({ cmd: 'start' });
    sendTune();
  }

  function setListening(on) {
    st.listening = on;
    if (!on) st.wantAudio = false;
    const b = $('audio');
    b.textContent = on ? 'Audio stoppen' : 'Audio starten';
    b.setAttribute('aria-pressed', String(on));
    if (!on) { $('smbar').style.width = '0'; $('smval').textContent = '–'; }
    updateMeterInfo();
  }

  function updateMeterInfo() {
    const ant = (st.listening && st.audioAnt) ? st.audioAnt : st.viewAnt;
    const parts = [];
    if (ant) parts.push(`Antenne <strong>${esc(ant)}</strong>`);
    if (st.listening) parts.push(st.audioOwn ? 'Ton aus <strong>eigenem Empfänger</strong>' : 'Ton aus dem <strong>Wasserfall</strong>');
    if (st.status) parts.push(`Empfänger frei <strong>${st.status.free} von ${st.status.total}</strong>`);
    $('meter-werte').innerHTML = parts.map((p) => `<span>${p}</span>`).join('');
  }

  // ------------------------------------------------------------ frequency display (as on /remote)

  function renderFreq() {
    if (st.editing) return;
    const n = 8;
    const s = String(Math.max(0, Math.round(st.freq))).padStart(n, '0');
    let html = '', leading = true;
    for (let i = 0; i < n; i++) {
      const pos = n - 1 - i;
      if (s[i] !== '0' || pos <= 6) leading = false;
      html += `<span class="ziffer${leading ? ' fuehrend' : ''}" data-pos="${pos}">${s[i]}</span>`;
      if (pos === 6 || pos === 3) html += '<span class="trenner">.</span>';
    }
    $('freq').innerHTML = html;
  }

  function editFreq() {
    if (st.editing) return;
    st.editing = true;
    const el = $('freq');
    const inp = document.createElement('input');
    inp.inputMode = 'decimal';
    inp.value = (st.freq / 1e6).toFixed(6);
    inp.setAttribute('aria-label', 'Frequenz in MHz eintippen');
    el.replaceChildren(inp);
    inp.focus();
    inp.select();
    const done = (apply) => {
      if (!st.editing) return;
      st.editing = false;
      if (apply) {
        const raw = inp.value.trim().replace(',', '.');
        let hz = parseFloat(raw);
        if (isFinite(hz) && hz > 0) {
          // MHz as on /remote; large numbers are taken as kHz or Hz
          hz = hz < 100 ? hz * 1e6 : hz < 100000 ? hz * 1e3 : hz;
          tuneTo(hz);
        }
      }
      renderFreq();
    };
    inp.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); done(true); el.focus(); }
      if (e.key === 'Escape') { e.preventDefault(); done(false); el.focus(); }
      e.stopPropagation();
    });
    inp.addEventListener('blur', () => done(false));
  }

  // ------------------------------------------------------------ tuning and views

  function renderModes() {
    const sel = $('mode');
    sel.innerHTML = '';
    for (const [key, m] of Object.entries(MODES)) {
      const o = document.createElement('option');
      o.value = key;
      o.textContent = m.label;
      sel.appendChild(o);
    }
    sel.value = st.mode;
  }

  function renderBw() {
    const sel = $('bw');
    sel.innerHTML = '';
    MODES[st.mode].bws.forEach((bw, i) => {
      const o = document.createElement('option');
      o.value = i;
      o.textContent = bw[2];
      sel.appendChild(o);
    });
    sel.value = st.bw;
  }

  function setMode(mode, quiet) {
    if (!MODES[mode]) return;
    if (st.mode !== mode) { st.mode = mode; st.bw = 0; }
    st.step = DEFAULT_STEP[mode];
    $('step').value = st.step;
    $('mode').value = mode;
    renderBw();
    store.set('mode', mode);
    if (!quiet) tuneTo(st.freq);
  }

  let tuneTimer = 0;
  function sendTune() {
    if (EMBED || !st.freq) return;
    clearTimeout(tuneTimer);
    tuneTimer = setTimeout(() => {
      const bw = MODES[st.mode].bws[st.bw];
      send({ cmd: 'tune', freq: Math.round(st.freq), mode: st.mode, lo: bw[0], hi: bw[1] });
    }, 30);
  }

  function maxFreq() { return (st.cfg && st.cfg.maxFreq) || 61440000; }

  function tuneTo(f, snap) {
    if (snap) f = Math.round(f / st.step) * st.step;
    f = Math.max(0, Math.min(maxFreq(), Math.round(f)));
    const bandBefore = bandEdges(st.freq);
    st.freq = f;
    renderFreq();
    store.set('freq', f);
    ensureView(f);
    updatePassband();
    drawSpectrum();
    if (!EMBED) {
      drawRibbon();
      if (bandEdges(f) !== bandBefore) renderBookmarkBox();
    }
    sendTune();
  }

  // Waterfall follows the frequency: configured band if one covers it, otherwise a free centre.
  function ensureView(f, force) {
    if (!st.cfg || !st.ws || st.ws.readyState !== 1) return;
    // refused for lack of receivers: wait until one is free (status) or the user picks a band
    if (st.noView && !force) return;
    const inner = (st.span / 2) * VIEW_INNER;
    const covered = st.view >= 0 && Math.abs(f - st.center) < inner;
    if (!force && covered) return;
    // a band whose edges hold f (the server widens it over the whole band while
    // receivers are free), else one whose centre is near
    let best = -1, bestDist = Infinity;
    st.cfg.bands.forEach((b) => {
      const d = Math.abs(f - b.center);
      const inside = b.lo < b.hi && f >= b.lo && f <= b.hi;
      if ((inside || d < (st.cfg.span / 2) * VIEW_INNER) && d < bestDist) { best = b.id; bestDist = d; }
    });
    // the band view is already open but does not reach f (not widened): free view instead
    if (best >= 0 && !(st.viewBand === best && st.view >= 0 && !covered)) { requestBand(best); return; }
    const c = Math.round(f / VIEW_GRID) * VIEW_GRID;
    if (!force && st.view >= 0 && st.viewBand < 0 && Math.abs(c - st.center) < 1) return;
    if (st.pending === `c${c}`) return;
    st.pending = `c${c}`;
    send({ cmd: 'view', center: c });
  }

  function requestBand(id) {
    if (st.viewBand === id && st.view >= 0) {
      if (st.userBand === id) { st.userBand = null; gotoBandCentre(id); }
      return;
    }
    if (st.pending === `b${id}`) return;
    st.pending = `b${id}`;
    send({ cmd: 'band', id });
  }

  function gotoBandCentre(id) {
    const b = st.cfg.bands[id];
    setMode(MODES[b.mode] ? b.mode : 'usb', true);
    tuneTo(b.center);
  }

  function onView(m) {
    st.pending = null;
    if (m.id < 0) {
      // no waterfall: all receivers busy, or ours was taken by someone with priority
      st.view = -1;
      st.viewBand = -1;
      st.noView = true;
      st.noViewReason = m.reason || '';
      st.segMaps = null;
      clearHistory();
      if (!EMBED) { markBand(); drawRibbon(); }
      drawWaterfall();
      drawSpectrum();
      return;
    }
    st.noView = false;
    st.noViewReason = '';
    const layout = JSON.stringify(m.segments || []);
    const changed = st.view !== m.id || st.layout !== layout;
    st.layout = layout;
    st.view = m.id;
    st.viewBand = m.band;
    st.center = m.lo !== undefined ? (m.lo + m.hi) / 2 : m.center;
    st.span = m.lo !== undefined ? m.hi - m.lo : m.span;
    st.segs = m.segments && m.segments.length ? m.segments
      : [{ center: m.center, lo: m.center - m.span / 2, hi: m.center + m.span / 2 }];
    buildSegMaps();
    // the band view may not reach the tuned frequency (band not widened yet)
    setTimeout(() => ensureView(st.freq), 0);
    st.viewAnt = m.antenna || '';
    if (changed) clearHistory();
    if (st.userBand !== null && st.userBand === m.band) {
      st.userBand = null;
      gotoBandCentre(m.band);
    }
    if (!EMBED) { markBand(); drawRibbon(); updateMeterInfo(); }
    drawWfScale();
    updatePassband();
    drawWaterfall();
    drawSpectrum();
  }

  function clickTune(ev, canvas) {
    if (canvas === spec) {
      const b = bookmarkAt(ev);
      if (b) { gotoBookmark(b); return; }
    }
    const r = canvas.getBoundingClientRect();
    tuneTo(xToFreq(ev.clientX - r.left, r.width), true);
    notifyParent();
  }

  function wheelTune(ev) {
    ev.preventDefault();
    const dir = ev.deltaY < 0 || ev.deltaX < 0 ? 1 : -1;
    tuneTo(Math.round(st.freq / st.step) * st.step + dir * st.step);
    notifyParent();
  }

  // Embedded: tell the page around us where the user clicked (e.g. to retune the rig).
  function notifyParent() {
    if (!EMBED || window.parent === window) return;
    window.parent.postMessage({
      source: 'r2t2sdr', type: 'tune', freq: st.freq, mode: st.mode === 'fm' ? 'nfm' : st.mode,
    }, '*');
  }

  function hoverInfo(ev, canvas) {
    if (!st.center) return;
    const r = canvas.getBoundingClientRect();
    const f = xToFreq(ev.clientX - r.left, r.width);
    const b = canvas === spec ? bookmarkAt(ev) : null;
    canvas.style.cursor = b ? 'pointer' : '';
    $('hover').textContent = b ? `${b.name} · ${(b.freq / 1e6).toFixed(4)} MHz ${b.mode.toUpperCase()}`
      : `${(f / 1e6).toFixed(4)} MHz`;
  }

  function gotoBookmark(b) {
    setMode(MOD_ALIAS[b.mode] || 'usb', true);
    tuneTo(b.freq);
    notifyParent();
  }

  // ------------------------------------------------------------ OpenWebRX-compatible control

  function parseHash() {
    const h = location.hash.replace(/^#/, '');
    const f = /(?:^|,)freq=(\d+)/.exec(h);
    const m = /(?:^|,)mod=([a-z]+)/.exec(h);
    return { freq: f ? Number(f[1]) : 0, mod: m ? MOD_ALIAS[m[1]] : null };
  }

  function applyHash() {
    const h = parseHash();
    if (h.mod) setMode(h.mod, true);
    if (h.freq) tuneTo(h.freq);
    else if (h.mod) tuneTo(st.freq);
    return Boolean(h.freq);
  }

  window.UI = {
    setFrequency(f) {
      f = Number(f);
      if (!isFinite(f) || f <= 0 || f > maxFreq()) return false;
      tuneTo(f);
      return true;
    },
    getFrequency() { return st.freq; },
    setModulation(m) {
      const k = MOD_ALIAS[String(m || '').toLowerCase()];
      if (!k) return false;
      setMode(k);
      return true;
    },
    getModulation() { return st.mode === 'fm' ? 'nfm' : st.mode; },
  };

  // Cross-origin parents can do the same with postMessage({type: 'r2t2sdr:set', freq, mode}).
  window.addEventListener('message', (ev) => {
    const d = ev.data;
    if (!d || d.type !== 'r2t2sdr:set') return;
    if (d.mode) window.UI.setModulation(d.mode);
    if (d.freq) window.UI.setFrequency(d.freq);
  });

  // ------------------------------------------------------------ accounts and administration

  const ROLE_RANK = { nutzer: 1, station: 1, lesezeichen: 2, admin: 3 };
  const ROLE_LABEL = { nutzer: 'Nutzer', station: 'Station', lesezeichen: 'Lesezeichen', admin: 'Admin' };
  const ICON_PAPIERKORB = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor"'
    + ' stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'
    + '<path d="M4 7h16"/><path d="M10 11.5v5"/><path d="M14 11.5v5"/>'
    + '<path d="M6 7l.9 11.1A2 2 0 0 0 8.9 20h6.2a2 2 0 0 0 2-1.9L18 7"/>'
    + '<path d="M9.5 7V5.4a1 1 0 0 1 1-1h3a1 1 0 0 1 1 1V7"/></svg>';
  const ICON_STIFT = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor"'
    + ' stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'
    + '<path d="M4 20h4.2l9.4-9.4a2.1 2.1 0 0 0 0-3l-1.2-1.2a2.1 2.1 0 0 0-3 0L4 15.8V20Z"/>'
    + '<path d="M14.5 6.5l3 3"/></svg>';

  const can = (role) => (ROLE_RANK[st.role] || 0) >= ROLE_RANK[role];
  const token = () => store.get('token', null);

  // Errors go to the dialog that is open (top of it), otherwise to a toast.
  function showError(msg) {
    const open = [...document.querySelectorAll('dialog[open]')].pop();
    const box = open && open.querySelector('[data-fehler]');
    if (box) { box.textContent = msg; box.hidden = false; return; }
    toast(msg, 'fehler');
  }
  function clearError(dlg) {
    const box = dlg.querySelector('[data-fehler]');
    if (box) box.hidden = true;
  }
  function openDialog(id) {
    const d = $(id);
    clearError(d);
    if (!d.open) d.showModal();
    return d;
  }

  let askYes = null;
  function ask(title, text, yesLabel, onYes) {
    $('dlg-frage-titel').textContent = title;
    $('dlg-frage-text').textContent = text;
    $('dlg-frage-ja').innerHTML = `${ICON_PAPIERKORB}<span>${esc(yesLabel)}</span>`;
    askYes = onYes;
    openDialog('dlg-frage');
  }

  function renderAccount() {
    const logged = Boolean(st.user);
    $('anmelden').hidden = logged;
    $('konto-menu').hidden = !logged;
    if (!logged) $('konto-menu').removeAttribute('open');
    if (logged) {
      $('konto-name').textContent = st.user;
      $('menu-verwaltung-info').textContent = can('admin') ? 'Lesezeichen, Antennen, Benutzer, Station, Passwort'
        : can('lesezeichen') ? 'Lesezeichen, Passwort' : 'Passwort ändern';
    }
    document.querySelectorAll('#tabs .rm-tab').forEach((t) => { t.hidden = !can(t.dataset.rolle); });
    if (!logged && $('dlg-verwaltung').open) $('dlg-verwaltung').close();
    renderBookmarkBox();
    renderChatForm();
  }

  let pendingLogin = null, pendingPasswd = null;

  function onChallenge(m) {
    if (m.purpose === 'login' && pendingLogin) {
      const p = pendingLogin;
      pendingLogin = null;
      send({ cmd: 'login', user: p.user, proof: window.R2Kdf.proof(p.pass, m.salt, m.iter, m.nonce) });
    } else if (m.purpose === 'passwd' && pendingPasswd) {
      const p = pendingPasswd;
      pendingPasswd = null;
      send(Object.assign({ cmd: 'passwd', proof: window.R2Kdf.proof(p.old, m.salt, m.iter, m.nonce) },
        window.R2Kdf.newPassword(p.neu, PBKDF2_ITER)));
    }
  }

  function onLogin(m) {
    st.user = m.user;
    st.role = m.role;
    if (m.token) store.set('token', m.token);
    $('login-pass').value = '';
    if ($('dlg-login').open) {
      $('dlg-login').close();
      toast(`Angemeldet als ${m.user}.`, 'gut');
    }
    renderAccount();
    if ($('dlg-verwaltung').open) refreshTab();
  }

  function onLogout() {
    st.user = null;
    st.role = null;
    store.set('token', null);
    renderAccount();
  }

  // ---- bookmarks

  function onBookmarks(list) {
    st.bookmarks = list;
    renderBookmarkBox();
    renderBookmarkTable();
    drawSpectrum();
    if (!EMBED) drawRibbon();
  }

  /*
   * Data tables after the afu.tools design guide, built on afu-tabelle.js:
   * sortable heads, filter fields that move into the head on wide screens,
   * count line, footer with paging (10 per page unless the reader chose more).
   */
  function dataTable({ table, footer, key, start, value, filters, match, row, empty, columns, count }) {
    const tbody = table.tBodies[0];
    const pager = seitenFuss(footer, { schluessel: key, beiWechsel: () => draw(), obenAnkern: table });
    const sorting = sortKopf(table, () => draw(), start);
    filterKopf(table, filters);
    let rows = [];
    function draw() {
      const shown = rows.filter(match);
      const part = pager.ausschnitt(sortiereZeilen(shown, sorting.feld(), sorting.richtung(), value));
      tbody.replaceChildren(...(shown.length ? part.map(row)
        : [el('tr', {}, el('td', { colspan: String(columns), class: 'empty' }, empty))]));
      if (count) count(shown.length, rows.length);
    }
    for (const f of Object.values(filters)) {
      f.addEventListener(f.tagName === 'SELECT' ? 'change' : 'input', () => { pager.zuruecksetzen(); draw(); });
    }
    return { set(list) { rows = list; draw(); }, draw };
  }

  function bookmarkActions(b, withTune) {
    const box = el('div', { class: 'rm-knopfreihe' });
    if (withTune) box.append(el('button', { type: 'button', class: 'btn-sm', onclick: () => gotoBookmark(b) }, 'Abstimmen'));
    if (can('lesezeichen')) {
      box.append(
        el('button', { type: 'button', class: 'btn-sm btn-stift', title: 'Bearbeiten', 'aria-label': `${b.name} bearbeiten`,
          html: ICON_STIFT, onclick: () => bookmarkDialog(b) }),
        el('button', { type: 'button', class: 'btn-sm btn-weg', title: 'Löschen', 'aria-label': `${b.name} löschen`,
          html: ICON_PAPIERKORB, onclick: () => ask('Lesezeichen löschen?', `„${b.name}“ verschwindet für alle Besucher.`,
            'Löschen', () => send({ cmd: 'bm_del', id: b.id })) }));
    }
    return box;
  }

  const bookmarkValue = (b, field) => (field === 'freq' ? b.freq : field === 'mode' ? b.mode.toUpperCase() : b.name);
  const textMatch = (q, s) => !q || s.toLowerCase().includes(q.toLowerCase());
  // "7074" or "7.07" finds by the start of the kHz or MHz figure, "7000-7200" is a range
  // (numbers below 100 are MHz, the others kHz)
  function freqMatch(q, hz) {
    q = q.trim().replace(/,/g, '.');
    if (!q) return true;
    const toHz = (v) => { const n = parseFloat(v); return !isFinite(n) ? NaN : n < 100 ? n * 1e6 : n * 1e3; };
    const range = q.split(/\s*[-–]\s*/);
    if (range.length === 2 && range[0] && range[1]) {
      const a = toHz(range[0]), b = toHz(range[1]);
      if (isFinite(a) && isFinite(b)) return hz >= Math.min(a, b) && hz <= Math.max(a, b);
    }
    const khz = (hz / 1000).toFixed(2), mhz = (hz / 1e6).toFixed(4);
    return khz.startsWith(q) || mhz.startsWith(q);
  }
  let boxTable = null, adminTable = null, userTable = null;

  function setupTables() {
    for (const sel of document.querySelectorAll('.sdr-art-wahl')) {
      for (const [k, m] of Object.entries(MODES)) sel.append(el('option', { value: k }, m.label));
    }
    // box on the page (as lesezeichenZeigen() on /remote, with search, filter and paging)
    boxTable = dataTable({
      table: $('lz-box-tabelle'), footer: $('lz-box-fuss'), key: 'r2t2:lesezeichen:proseite',
      start: { feld: 'freq', richtung: 'ascending' }, value: bookmarkValue, columns: 4,
      filters: { bf: $('bf'), bq: $('bq'), bart: $('bart') },
      match: (b) => {
        const band = $('lz-nurband').checked ? bandEdges(st.freq) : null;
        return (!band || (b.freq >= band.lo && b.freq <= band.hi))
          && freqMatch($('bf').value, b.freq)
          && textMatch($('bq').value.trim(), b.name) && (!$('bart').value || b.mode === $('bart').value);
      },
      row: (b) => el('tr', {},
        el('td', { class: 'mono num' }, (b.freq / 1e6).toFixed(4)),
        el('td', {}, b.mode.toUpperCase()),
        el('td', {}, b.name),
        el('td', {}, bookmarkActions(b, true))),
      empty: 'Kein Lesezeichen passt zu diesem Filter.',
      count: (shown, all) => {
        const band = $('lz-nurband').checked ? bandEdges(st.freq) : null;
        $('lz-box-zahl').textContent = shown === all ? `${all} Lesezeichen`
          : `${shown} von ${all} Lesezeichen${band ? `, nur ${band.name}` : ''}`;
      },
    });
    // administration
    adminTable = dataTable({
      table: $('lz-tabelle'), footer: $('lz-fuss'), key: 'r2t2:verwaltung-lz:proseite',
      start: { feld: 'freq', richtung: 'ascending' }, value: bookmarkValue, columns: 4,
      filters: { lzq: $('lzq'), lzf: $('lzf'), lzart: $('lzart') },
      match: (b) => textMatch($('lzq').value.trim(), b.name) && freqMatch($('lzf').value, b.freq)
        && (!$('lzart').value || b.mode === $('lzart').value),
      row: (b) => el('tr', {},
        el('td', {}, b.name),
        el('td', { class: 'num mono' }, `${(b.freq / 1000).toFixed(2)} kHz`),
        el('td', {}, b.mode.toUpperCase()),
        el('td', {}, bookmarkActions(b, false))),
      empty: 'Kein Lesezeichen passt zu diesem Filter.',
      count: (shown, all) => {
        $('lz-zahl').textContent = shown === all ? `${all} Lesezeichen` : `${shown} von ${all} Lesezeichen`;
        // the export always takes the whole list (design guide: say so when a filter hides some)
        $('lz-export-hinweis').hidden = shown === all;
        $('lz-export-hinweis').textContent = `Exportieren nimmt alle ${all} Lesezeichen, auch die ${all - shown} gerade ausgefilterten.`;
      },
    });
    userTable = dataTable({
      table: $('nutzer-tabelle'), footer: $('nutzer-fuss'), key: 'r2t2:verwaltung-nutzer:proseite',
      start: { feld: 'name', richtung: 'ascending' }, columns: 4,
      value: (u, field) => (field === 'role' ? ROLE_LABEL[u.role] : field === 'online' ? (u.online ? 'ja' : 'nein') : u.name),
      filters: { nuq: $('nuq'), nurolle: $('nurolle') },
      match: (u) => textMatch($('nuq').value.trim(), u.name) && (!$('nurolle').value || u.role === $('nurolle').value),
      row: userRow,
      empty: 'Kein Konto passt zu diesem Filter.',
      count: (shown, all) => { $('nutzer-zahl').textContent = shown === all ? `${all} Konten` : `${shown} von ${all} Konten`; },
    });
  }

  // box on the page
  function renderBookmarkBox() {
    if (EMBED || !boxTable) return;
    const has = st.bookmarks.length > 0;
    $('lz-neu').hidden = !can('lesezeichen');
    $('lz-box-teil').hidden = !has;
    $('lz-box-leer').hidden = has;
    $('lz-box-leer').textContent = can('lesezeichen')
      ? 'Noch keine Lesezeichen. Mit ＋ oben rechts merkst du dir die eingestellte Frequenz.'
      : 'Noch keine Lesezeichen.';
    boxTable.set(st.bookmarks);
  }

  // table in the administration dialog
  function renderBookmarkTable() {
    if (EMBED || !adminTable) return;
    const has = st.bookmarks.length > 0;
    $('lz-leer').hidden = has;
    $('lz-teil').hidden = !has;
    adminTable.set(st.bookmarks);
  }

  let bmEdit = null;
  function bookmarkDialog(b) {
    bmEdit = b || null;
    $('dlg-lz-titel').textContent = b ? 'Lesezeichen bearbeiten' : 'Neues Lesezeichen';
    const sel = $('lz-mode');
    if (!sel.options.length) {
      for (const [k, m] of Object.entries(MODES)) {
        const o = document.createElement('option');
        o.value = k;
        o.textContent = m.label;
        sel.appendChild(o);
      }
    }
    $('lz-name').value = b ? b.name : '';
    $('lz-freq').value = ((b ? b.freq : st.freq) / 1000).toFixed(2);
    sel.value = MOD_ALIAS[b ? b.mode : st.mode] || 'usb';
    openDialog('dlg-lz');
    $('lz-name').focus();
  }

  function saveBookmark(ev) {
    ev.preventDefault();
    const khz = parseFloat($('lz-freq').value.trim().replace(',', '.'));
    if (!isFinite(khz) || khz <= 0) { showError('Frequenz bitte in kHz eingeben, z. B. 7074 oder 7074,5.'); return; }
    send({ cmd: 'bm_set', id: bmEdit ? bmEdit.id : 0, name: $('lz-name').value.trim(),
      freq: Math.round(khz * 1000), mode: $('lz-mode').value });
    $('dlg-lz').close();
  }

  // ---- import and export, OpenWebRX bookmarks.json: [{name, frequency, modulation}]

  // OpenWebRX modulations onto the five modes here (digital modes onto what carries them)
  const OWRX_MODE = {
    usb: 'usb', lsb: 'lsb', cw: 'cw', am: 'am', sam: 'am', nfm: 'fm', wfm: 'fm', fm: 'fm',
    drm: 'am', freedv: 'usb', rtty: 'usb', bpsk31: 'usb', bpsk63: 'usb', ft8: 'usb', ft4: 'usb',
    wspr: 'usb', jt65: 'usb', jt9: 'usb', js8: 'usb', fst4: 'usb', fst4w: 'usb', q65: 'usb',
    msk144: 'usb', sstv: 'usb', fax: 'usb', cwskimmer: 'cw', rttyskimmer: 'usb',
    dmr: 'fm', ysf: 'fm', dstar: 'fm', nxdn: 'fm', m17: 'fm', packet: 'fm', pocsag: 'fm',
    page: 'fm', ais: 'fm', adsb: 'fm', hfdl: 'usb', vdl2: 'am', acars: 'am',
    sitorb: 'usb', navtex: 'usb', dsc: 'usb', rtty170: 'usb', rtty450: 'usb', rtty85: 'usb',
    cwdecoder: 'cw', bpsk: 'usb', ism: 'fm', dab: 'fm', usbd: 'usb', lsbd: 'lsb',
  };
  const IMPORT_PART = 30;     // entries per message ...
  const IMPORT_BYTES = 3200;  // ... and bytes, a message may be 4 kB

  function exportBookmarks() {
    const list = st.bookmarks.map((b) => ({ name: b.name, frequency: Math.round(b.freq),
      modulation: b.mode === 'fm' ? 'nfm' : b.mode }));
    const blob = new Blob([JSON.stringify(list, null, 2)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'r2t2-lesezeichen.json';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }

  // accepts OpenWebRX ({name, frequency, modulation}) and this program's own format ({name, freq, mode})
  function parseBookmarks(text) {
    let data = JSON.parse(text);
    if (data && !Array.isArray(data)) data = data.bookmarks || data.list || [];
    if (!Array.isArray(data)) throw new Error('keine Liste');
    const items = [], unknown = new Set();
    let invalid = 0;
    for (const x of data) {
      const f = Number(x && (x.frequency ?? x.freq));
      const name = String((x && x.name) || '').trim();
      const raw = String((x && (x.modulation ?? x.mode)) || 'usb').toLowerCase();
      if (!isFinite(f) || f <= 0 || !name) { invalid++; continue; }
      let mode = OWRX_MODE[raw];
      if (!mode) { unknown.add(raw); mode = 'usb'; }
      items.push([Math.round(f), mode, name.slice(0, 60)]);
    }
    return { items, invalid, unknown: [...unknown] };
  }

  let importData = null, importSum = null;
  function importFile(file) {
    const reader = new FileReader();
    reader.onload = () => {
      try {
        importData = parseBookmarks(reader.result);
      } catch (e) {
        toast(`„${file.name}“ ist keine Lesezeichen-Datei (JSON wie bei OpenWebRX erwartet).`, 'fehler');
        return;
      }
      const d = importData;
      const tooHigh = d.items.filter((it) => it[0] > maxFreq()).length;
      let info = `${file.name}: ${d.items.length} Lesezeichen gefunden.`;
      if (tooHigh) info += ` ${tooHigh} liegen über ${(maxFreq() / 1e6).toFixed(0)} MHz und werden übersprungen.`;
      if (d.invalid) info += ` ${d.invalid} ohne Name oder Frequenz werden übersprungen.`;
      if (d.unknown.length) info += ` Unbekannte Betriebsarten (${d.unknown.join(', ')}) werden USB.`;
      $('import-info').textContent = info;
      openDialog('dlg-import');
    };
    reader.readAsText(file);
  }

  function runImport(ev) {
    ev.preventDefault();
    if (!importData || !importData.items.length) { showError('Die Datei enthält keine Lesezeichen.'); return; }
    const replace = document.querySelector('input[name="import-art"]:checked').value === 'replace';
    const items = importData.items.filter((it) => it[0] <= maxFreq());
    importSum = { added: 0, duplicates: 0, full: 0, invalid: importData.invalid + (importData.items.length - items.length) };
    // parts by size in bytes (UTF-8), the server takes messages up to 4 kB
    const enc = new TextEncoder(), parts = [[]];
    let size = 0;
    for (const it of items) {
      const n = enc.encode(JSON.stringify(it)).length + 1;
      if (parts[parts.length - 1].length && (size + n > IMPORT_BYTES || parts[parts.length - 1].length >= IMPORT_PART)) {
        parts.push([]);
        size = 0;
      }
      parts[parts.length - 1].push(it);
      size += n;
    }
    parts.forEach((part, i) => send({ cmd: 'bm_import', replace: replace && i === 0 ? 1 : 0,
      done: i === parts.length - 1 ? 1 : 0, items: part }));
    $('dlg-import').close();
  }

  function onImportResult(m) {
    if (!importSum) return;
    importSum.added += m.added;
    importSum.duplicates += m.duplicates;
    importSum.full += m.full;
    importSum.invalid += m.invalid;
    if (!m.done) return;
    const s = importSum;
    importSum = null;
    let text = `${s.added} Lesezeichen importiert.`;
    if (s.duplicates) text += ` ${s.duplicates} waren schon da.`;
    if (s.full) text += ` ${s.full} passten nicht mehr (höchstens 1000).`;
    if (s.invalid) text += ` ${s.invalid} übersprungen.`;
    toast(text, s.full ? 'warnung' : 'gut');
  }

  // ---- administration dialog

  let activeTab = null;
  function openAdmin(tab) {
    const tabs = [...document.querySelectorAll('#tabs .rm-tab')].filter((t) => !t.hidden);
    if (!tabs.length) return;
    const want = tab || activeTab;
    selectTab(tabs.find((t) => t.dataset.tab === want) ? want : tabs[0].dataset.tab);
    openDialog('dlg-verwaltung');
  }

  function selectTab(name) {
    activeTab = name;
    document.querySelectorAll('#tabs .rm-tab').forEach((t) => t.setAttribute('aria-selected', String(t.dataset.tab === name)));
    document.querySelectorAll('#dlg-verwaltung .rm-tab-seite').forEach((s) => { s.hidden = s.dataset.seite !== name; });
    clearError($('dlg-verwaltung'));
    refreshTab();
  }

  function refreshTab() {
    if (activeTab === 'nutzer') send({ cmd: 'users' });
    if (activeTab === 'ant') send({ cmd: 'antennas' });
    if (activeTab === 'station' && st.cfg) {
      $('st-title').value = st.cfg.title || '';
      $('st-callsign').value = st.cfg.callsign || '';
      $('st-location').value = st.cfg.location || '';
      $('st-locator').value = st.cfg.locator || '';
      $('st-access').value = st.cfg.access || 'open';
      $('st-chat').value = st.cfg.chat || 'all';
    }
    if (activeTab === 'konto') $('konto-info').textContent = `Angemeldet als ${st.user} (${ROLE_LABEL[st.role] || ''}).`;
    if (activeTab === 'lz') renderBookmarkTable();
  }

  function userRow(u) {
    const self = st.user && u.name.toLowerCase() === st.user.toLowerCase();
    const actions = el('div', { class: 'rm-knopfreihe' },
      el('button', { type: 'button', class: 'btn-sm btn-stift', title: 'Bearbeiten', 'aria-label': `${u.name} bearbeiten`,
        html: ICON_STIFT, onclick: () => userDialog(u) }));
    if (!self) {
      actions.append(el('button', { type: 'button', class: 'btn-sm btn-weg', title: 'Löschen', 'aria-label': `${u.name} löschen`,
        html: ICON_PAPIERKORB, onclick: () => ask('Konto löschen?', `„${u.name}“ wird gelöscht und sofort überall abgemeldet.`,
          'Konto löschen', () => send({ cmd: 'user_del', name: u.name })) }));
    }
    return el('tr', {},
      el('td', {}, u.name, self ? el('span', { class: 'muted' }, ' (du)') : null),
      el('td', {}, ROLE_LABEL[u.role] || u.role),
      el('td', {}, u.online ? 'ja' : 'nein'),
      el('td', {}, actions));
  }

  function onUsers(list) {
    if (userTable) userTable.set(list);
  }

  let userEdit = null;
  function userDialog(u) {
    userEdit = u || null;
    $('dlg-nutzer-titel').textContent = u ? `Konto ${u.name}` : 'Neues Konto';
    $('nu-name').value = u ? u.name : '';
    $('nu-name').readOnly = Boolean(u);
    $('nu-role').value = u ? u.role : 'nutzer';
    $('nu-pass').value = '';
    $('nu-pass').required = !u;
    $('nu-pass-hint').textContent = u ? 'Leer lassen, um es zu behalten. Ein neues Passwort meldet das Konto überall ab.' : 'Mindestens 8 Zeichen.';
    openDialog('dlg-nutzer');
    (u ? $('nu-role') : $('nu-name')).focus();
  }

  function saveUser(ev) {
    ev.preventDefault();
    const pw = $('nu-pass').value;
    if ((!userEdit || pw) && pw.length < 8) { showError('Das Passwort braucht mindestens 8 Zeichen.'); return; }
    const msg = { cmd: 'user_set', name: $('nu-name').value.trim(), role: $('nu-role').value };
    // only salt and hash leave the browser
    if (pw) Object.assign(msg, window.R2Kdf.newPassword(pw, PBKDF2_ITER));
    send(msg);
    $('dlg-nutzer').close();
  }

  function onAntennas(m) {
    const box = $('ant-liste');
    box.innerHTML = '';
    for (const a of m.list) {
      const k = document.createElement('form');
      k.className = 'rm-kasten';
      const id = `ant${a.input}`;
      k.innerHTML = `<div class="rm-kasten-kopf">Eingang ${a.input} (ANT${a.input})</div>
        <div class="rm-kasten-inhalt">
          <div class="field"><label for="${id}-name">Name</label>
            <input id="${id}-name" maxlength="40" value="${esc(a.name)}"></div>
          <div class="field"><label for="${id}-ranges">Bereiche <span class="einheit">kHz</span></label>
            <input id="${id}-ranges" value="${esc(a.ranges.replace(/,/g, ', '))}" placeholder="1810-2000, 3500-3800" spellcheck="false">
            <span class="field-hint">von-bis, mit Komma getrennt. Leer: dieser Eingang bekommt nur, was sonst keiner abdeckt.</span></div>
          <div class="sdr-zwei">
            <div class="field"><label for="${id}-gain">Verstärkung <span class="einheit">dB</span></label>
              <input id="${id}-gain" type="number" min="-9" max="32" step="1" inputmode="numeric" value="${a.gain}"></div>
            <div class="field"><label for="${id}-att">Abschwächer <span class="einheit">dB</span></label>
              <input id="${id}-att" type="number" min="0" max="31" step="1" inputmode="numeric" value="${a.att}"></div>
          </div>
          <label class="rm-schalter"><input type="radio" name="ant-default" id="${id}-def" ${m.default === a.input ? 'checked' : ''}> Für alle übrigen Frequenzen</label>
          <p class="btn-row"><button type="submit" class="btn">Speichern</button></p>
        </div>`;
      k.addEventListener('submit', (ev) => {
        ev.preventDefault();
        send({ cmd: 'ant_set', input: a.input, name: $(`${id}-name`).value.trim(),
          ranges: $(`${id}-ranges`).value.replace(/\s+/g, ''), gain: Number($(`${id}-gain`).value),
          att: Number($(`${id}-att`).value), default: $(`${id}-def`).checked ? 1 : 0 });
        toast(`Eingang ${a.input} gespeichert.`, 'gut');
      });
      box.appendChild(k);
    }
  }

  function onOk(what) {
    if (what === 'passwd') {
      ['pw-alt', 'pw-neu', 'pw-neu2'].forEach((id) => { $(id).value = ''; });
      toast('Passwort geändert.', 'gut');
    }
    if (what === 'station') toast('Station gespeichert.', 'gut');
  }

  function onOkMessage(m) {
    if (m.what === 'bm_import') onImportResult(m);
    else onOk(m.what);
  }

  // ------------------------------------------------------------ chat (as on /remote)

  function chatAllowed() {
    const mode = (st.cfg && st.cfg.chat) || 'all';
    return mode === 'all' || (mode === 'login' && Boolean(st.user));
  }

  function renderChatForm() {
    if (EMBED || !st.cfg) return;
    const mode = st.cfg.chat || 'all';
    $('chat-kasten').hidden = mode === 'off';
    $('chat-form').hidden = !chatAllowed();
    $('chat-name-feld').hidden = !chatAllowed() || Boolean(st.user);
    $('chat-hinweis').textContent = chatAllowed()
      ? 'Mit allen, die gerade zuhören. Die letzten 30 Nachrichten sieht, wer dazukommt; gespeichert wird nichts.'
      : 'Mitschreiben können hier nur angemeldete Nutzer. Lesen geht ohne Anmeldung.';
  }

  function onChat(m) {
    st.chat.push(m);
    if (st.chat.length > 200) st.chat.shift();
    if (EMBED) return;
    const box = $('chat-liste');
    const atEnd = box.scrollHeight - box.scrollTop - box.clientHeight < 4;
    box.innerHTML = st.chat.map((c) => {
      const own = st.user ? (!c.guest && c.who === st.user) : (c.guest && c.who === ($('chat-name').value.trim() || null));
      return `<div class="rm-chat-zeile"><span class="rm-chat-wer${own ? ' rm-chat-eigen' : ''}">${esc(c.who)}`
        + `${c.guest ? ' <span class="sdr-chat-gast">(Gast)</span>' : ''}</span>`
        + `<span class="rm-chat-zeit mono">${esc((c.ts || '').slice(11, 16))} UTC</span>`
        + `<span class="rm-chat-text">${esc(c.text)}</span></div>`;
    }).join('');
    if (atEnd) box.scrollTop = box.scrollHeight;
  }

  // ------------------------------------------------------------ report a problem (afu.tools)

  const AFU = 'https://afu.tools';
  async function checkOnline() {
    try {
      await fetch(`${AFU}/api/v1/health`, { mode: 'no-cors', cache: 'no-store' });
      st.online = true;
    } catch (e) {
      st.online = false;
    }
    $('melden').hidden = !st.online;
  }

  async function sendReport(ev) {
    ev.preventDefault();
    const c = st.cfg || {};
    const daten = {
      art: $('melden-art').value, titel: $('melden-titel').value.trim(), text: $('melden-text').value.trim(),
      rufzeichen: st.user || $('melden-ruf').value.trim(),
      station: `R2T2 WebSDR ${[c.callsign, c.location].filter(Boolean).join(' ')}`.trim(),
      geraet: 'R2T2 WebSDR', version: c.version || '',
      technisch: `Programm: R2T2 WebSDR ${c.version || ''}\nFrequenz: ${st.freq} Hz ${st.mode}\n`
        + `Browser: ${navigator.userAgent}\nFenster: ${innerWidth}x${innerHeight}`,
    };
    try {
      const r = await fetch(`${AFU}/api/v1/remote/melden`, { method: 'POST',
        headers: { 'Content-Type': 'text/plain' }, body: JSON.stringify(daten) });
      const d = await r.json();
      if (d.ok) {
        $('dlg-melden').close();
        ['melden-titel', 'melden-text'].forEach((id) => { $(id).value = ''; });
        toast('Danke, die Meldung ist angekommen.', 'gut');
      } else {
        showError(d.text || 'Das hat nicht geklappt.');
      }
    } catch (e) {
      showError('afu.tools ist gerade nicht erreichbar. Versuch es später noch einmal.');
    }
  }

  // ------------------------------------------------------------ websocket

  function send(obj) {
    if (st.ws && st.ws.readyState === 1) st.ws.send(JSON.stringify(obj));
  }

  function onConfig(c) {
    const first = !st.gotConfig;
    st.gotConfig = true;
    st.cfg = c;
    st.span = c.span;
    document.title = c.title || 'R2T2 WebSDR';
    $('title').textContent = c.callsign || c.title || 'R2T2 WebSDR';
    $('station').textContent = ['R2T2', c.location, c.locator].filter(Boolean).join(' · ');
    $('fuss-station').textContent = [c.callsign, c.location, c.locator].filter(Boolean).join(' · ') || c.title || 'R2T2 WebSDR';
    $('fuss-version').textContent = `Version ${c.version}`;
    renderChatForm();
    // later copies (station or antennas changed by an admin) only refresh the texts
    if (!first) return;
    if (!EMBED && token()) send({ cmd: 'auth', token: token() });
    if (!EMBED) renderBandButtons();
    st.view = -1;
    st.viewBand = -1;
    st.pending = null;
    if (!st.freq) {
      if (!applyHash()) {
        const saved = store.get('freq', 0);
        const b = c.bands[0];
        setMode(store.get('mode', b ? b.mode : 'usb'), true);
        tuneTo(saved || (b ? b.center : 7100000));
      }
    } else {
      ensureView(st.freq, true);
    }
    if (st.wantAudio) { send({ cmd: 'start' }); sendTune(); }
  }

  function onStatus(m) {
    st.status = m;
    $('users').textContent = `${m.users} ${m.users === 1 ? 'Besucher' : 'Besucher'} · ${m.listeners} hören`;
    updateMeterInfo();
    // a receiver is free again: get the waterfall back
    if (st.noView && m.free > 0) { st.noView = false; ensureView(st.freq, true); }
    if (!EMBED) { markActive(); drawRibbon(); }
  }

  function setConn(on) {
    const el = $('conn');
    el.textContent = on ? 'online' : 'offline';
    el.classList.toggle('an', on);
    el.classList.toggle('aus', !on);
  }

  let retry = 1000;
  function connect() {
    const u = new URL('ws', location.href);
    u.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const ws = new WebSocket(u.href);
    ws.binaryType = 'arraybuffer';
    st.ws = ws;
    let ping = 0;
    ws.onopen = () => {
      st.gotConfig = false;
      st.chat = [];
      setConn(true);
      retry = 1000;
      ping = setInterval(() => send({ cmd: 'ping' }), 10000);
    };
    ws.onclose = () => {
      clearInterval(ping);
      setConn(false);
      const want = st.wantAudio || st.listening;
      setListening(false);
      st.wantAudio = want;
      setTimeout(connect, retry);
      retry = Math.min(retry * 2, 15000);
    };
    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') {
        let m;
        try { m = JSON.parse(ev.data); } catch (e) { return; }
        switch (m.type) {
          case 'config': onConfig(m); break;
          case 'view': onView(m); break;
          case 'status': onStatus(m); break;
          case 'audio':
            setListening(m.on);
            if (m.on) st.wantAudio = true;
            st.audioAnt = m.antenna || '';
            st.audioOwn = Boolean(m.own);
            updateMeterInfo();
            break;
          case 'bookmarks': onBookmarks(m.list); break;
          case 'chat': onChat(m); break;
          case 'challenge': onChallenge(m); break;
          case 'login': onLogin(m); break;
          case 'logout': onLogout(); break;
          case 'users': onUsers(m.list); break;
          case 'antennas': onAntennas(m); break;
          case 'ok': onOkMessage(m); break;
          case 'error': st.pending = null; showError(m.msg); break;
          default: break;
        }
        return;
      }
      const u8 = new Uint8Array(ev.data);
      if (u8[0] === 1) {
        if (u8[1] === st.view) onSegment(u8[2], u8.subarray(3));
      } else if (u8[0] === 2) {
        onAudio(ev.data);
      }
    };
  }

  // ------------------------------------------------------------ wiring

  function tickClock() {
    $('utc').textContent = `${new Date().toISOString().slice(11, 19)} UTC`;
  }

  // light/dark key in the header, same behaviour and storage key as afu.tools
  function wireTheme() {
    const k = document.querySelector('[data-theme-knopf]');
    const root = document.documentElement;
    const now = () => root.dataset.theme
      || (window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark');
    const show = () => { k.firstElementChild.textContent = now() === 'light' ? '🌙' : '☀️'; };
    show();
    k.addEventListener('click', () => {
      const next = now() === 'light' ? 'dark' : 'light';
      root.dataset.theme = next;
      try { localStorage.setItem('afu.tools:theme', next); } catch (e) { /* storage unavailable */ }
      show();
    });
  }

  function wireDialogs() {
    document.querySelectorAll('dialog [data-schliessen]').forEach((b) =>
      b.addEventListener('click', () => b.closest('dialog').close()));
    document.querySelectorAll('dialog').forEach((d) =>
      d.addEventListener('click', (e) => { if (e.target === d) d.close(); }));
    $('dlg-frage-ja').addEventListener('click', () => {
      $('dlg-frage').close();
      if (askYes) askYes();
      askYes = null;
    });

    $('anmelden').addEventListener('click', () => {
      $('login-pass').value = '';
      openDialog('dlg-login');
      $('login-user').focus();
    });
    $('form-login').addEventListener('submit', (ev) => {
      ev.preventDefault();
      clearError($('dlg-login'));
      // the password stays in the browser: ask for a challenge, answer with a proof
      pendingLogin = { user: $('login-user').value.trim(), pass: $('login-pass').value };
      send({ cmd: 'challenge', user: pendingLogin.user, purpose: 'login' });
    });
    // account menu: closes on a click elsewhere and on Escape, as on afu.tools
    const menu = $('konto-menu');
    document.addEventListener('click', (ev) => { if (!menu.contains(ev.target)) menu.removeAttribute('open'); });
    document.addEventListener('keydown', (ev) => { if (ev.key === 'Escape') menu.removeAttribute('open'); });
    $('abmelden').addEventListener('click', () => {
      menu.removeAttribute('open');
      send({ cmd: 'logout', token: token() });
      onLogout();
    });
    $('menu-verwaltung').addEventListener('click', () => {
      menu.removeAttribute('open');
      openAdmin(activeTab && activeTab !== 'konto' ? activeTab : null);
    });
    document.querySelectorAll('#tabs .rm-tab').forEach((t) => t.addEventListener('click', () => selectTab(t.dataset.tab)));

    $('lz-neu').addEventListener('click', () => bookmarkDialog(null));
    $('lz-plus').addEventListener('click', () => bookmarkDialog(null));
    $('form-lz').addEventListener('submit', saveBookmark);
    $('lz-nurband').addEventListener('change', renderBookmarkBox);
    $('lz-export').addEventListener('click', exportBookmarks);
    $('lz-import').addEventListener('click', () => { $('lz-datei').value = ''; $('lz-datei').click(); });
    $('lz-datei').addEventListener('change', (e) => { if (e.target.files[0]) importFile(e.target.files[0]); });
    $('form-import').addEventListener('submit', runImport);
    $('nutzer-plus').addEventListener('click', () => userDialog(null));
    $('form-nutzer').addEventListener('submit', saveUser);

    $('form-station').addEventListener('submit', (ev) => {
      ev.preventDefault();
      send({ cmd: 'station_set', title: $('st-title').value.trim(), callsign: $('st-callsign').value.trim().toUpperCase(),
        location: $('st-location').value.trim(), locator: $('st-locator').value.trim(),
        access: $('st-access').value, chat: $('st-chat').value });
    });
    $('form-passwort').addEventListener('submit', (ev) => {
      ev.preventDefault();
      if ($('pw-neu').value !== $('pw-neu2').value) { showError('Die beiden neuen Passwörter stimmen nicht überein.'); return; }
      if ($('pw-neu').value.length < 8) { showError('Das neue Passwort braucht mindestens 8 Zeichen.'); return; }
      pendingPasswd = { old: $('pw-alt').value, neu: $('pw-neu').value };
      send({ cmd: 'challenge', user: st.user, purpose: 'passwd' });
    });

    $('chat-name').value = store.get('chatname', '');
    $('chat-name').addEventListener('change', () => store.set('chatname', $('chat-name').value.trim()));
    $('chat-form').addEventListener('submit', (ev) => {
      ev.preventDefault();
      const text = $('chat-text').value.trim();
      if (!text) return;
      const msg = { cmd: 'chat', text };
      if (!st.user) {
        const name = $('chat-name').value.trim();
        if (!name) { toast('Bitte erst deinen Namen oder dein Rufzeichen eintragen.', 'warnung'); $('chat-name').focus(); return; }
        msg.name = name;
      }
      send(msg);
      $('chat-text').value = '';
    });

    $('melden').addEventListener('click', () => openDialog('dlg-melden'));
    $('form-melden').addEventListener('submit', sendReport);
  }

  function init() {
    st.mode = MODES[store.get('mode', 'usb')] ? store.get('mode', 'usb') : 'usb';
    st.step = DEFAULT_STEP[st.mode];
    $('step').value = st.step;
    $('vol').value = store.get('vol', 70);
    renderModes();
    renderBw();
    renderFreq();
    setConn(false);
    showLevels();

    [spec, wf].forEach((c) => {
      c.addEventListener('click', (e) => clickTune(e, c));
      c.addEventListener('wheel', wheelTune, { passive: false });
      c.addEventListener('mousemove', (e) => hoverInfo(e, c));
      c.addEventListener('mouseleave', () => { $('hover').textContent = ''; });
    });
    window.addEventListener('hashchange', applyHash);

    if (!EMBED) {
      wireTheme();
      setupTables();
      wireDialogs();
      renderAccount();
      $('audio').addEventListener('click', toggleAudio);
      $('vol').addEventListener('input', setVolume);
      $('stumm').addEventListener('click', () => setMuted(!st.muted));
      $('mode').addEventListener('change', (e) => setMode(e.target.value));
      $('bw').addEventListener('change', (e) => { st.bw = Number(e.target.value); tuneTo(st.freq); });
      $('step').addEventListener('change', (e) => { st.step = Number(e.target.value); });
      $('ab').addEventListener('click', () => tuneTo(Math.round(st.freq / st.step) * st.step - st.step));
      $('auf').addEventListener('click', () => tuneTo(Math.round(st.freq / st.step) * st.step + st.step));
      const sendSquelch = () => {
        const on = $('sql-art').value === 'schwelle';
        $('sql-feld').hidden = !on;
        $('sqlval').textContent = `${$('sql').value} dBFS`;
        send({ cmd: 'squelch', level: on ? Number($('sql').value) : -999 });
      };
      $('sql-art').addEventListener('change', sendSquelch);
      $('sql').addEventListener('input', sendSquelch);
      $('wfmin').addEventListener('input', (e) => { st.wfMin = Math.min(Number(e.target.value), st.wfMax - 5); showLevels(); });
      $('wfmax').addEventListener('input', (e) => { st.wfMax = Math.max(Number(e.target.value), st.wfMin + 5); showLevels(); });
      $('auto').addEventListener('click', () => { if (st.lastBins) autoLevels(st.lastBins); });

      const sk = $('skala');
      sk.addEventListener('click', (ev) => { if (st.ribbon) tuneTo(ribbonHz(ev), true); });
      sk.addEventListener('wheel', wheelTune, { passive: false });

      const fr = $('freq');
      fr.addEventListener('click', editFreq);
      fr.addEventListener('wheel', (e) => {
        if (st.editing) return;
        e.preventDefault();
        const z = e.target.closest && e.target.closest('.ziffer');
        const mult = z ? 10 ** Number(z.dataset.pos) : st.step;
        tuneTo(st.freq + (e.deltaY < 0 ? mult : -mult));
      }, { passive: false });
      fr.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); editFreq(); } });
      document.addEventListener('keydown', (e) => {
        if (e.target instanceof HTMLInputElement || e.target instanceof HTMLSelectElement
          || e.target instanceof HTMLTextAreaElement || document.querySelector('dialog[open]')) return;
        if (e.key === 'ArrowRight' || e.key === 'ArrowUp') { tuneTo(st.freq + st.step); e.preventDefault(); }
        if (e.key === 'ArrowLeft' || e.key === 'ArrowDown') { tuneTo(st.freq - st.step); e.preventDefault(); }
        if (e.key === 'm' || e.key === 'M') setMuted(!st.muted);
      });
      tickClock();
      setInterval(tickClock, 1000);
      checkOnline();
      setInterval(checkOnline, 5 * 60 * 1000);
      window.addEventListener('online', checkOnline);
    }

    if (window.ResizeObserver) new ResizeObserver(resize).observe($('wfbox'));
    else window.addEventListener('resize', resize);
    if (!EMBED && window.ResizeObserver) new ResizeObserver(() => drawRibbon()).observe($('skala'));
    resize();
    connect();
  }

  init();
})();
