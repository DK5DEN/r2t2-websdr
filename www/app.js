'use strict';

/*
 * R2T2 WebSDR page. Two faces:
 *  - opened directly: full receiver with header, waterfall and control panel
 *  - embedded (inside an iframe, or ?embed): scale, spectrum and waterfall only,
 *    no audio. Compatible with how afu.tools/remote drives an external OpenWebRX:
 *    it loads <base>/#freq=<Hz>,mod=<usb|lsb|cw|am|nfm> and calls
 *    window.UI.setFrequency() / setModulation(). A click in the waterfall is
 *    reported to the parent page with postMessage.
 * All URLs are relative, so the page also works behind the afu-remote tunnel.
 */
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

  const st = {
    cfg: null,
    ws: null,
    view: -1,
    viewBand: -1,
    center: 0,
    span: 192000,
    pending: null,
    userBand: null,
    freq: 0,
    mode: 'usb',
    bw: 0,
    step: 100,
    listening: false,
    wantAudio: false,
    wfMin: 40,
    wfMax: 120,
    autoFrames: 8,
    smooth: null,
    lastBins: null,
    editing: false,
  };

  const PREFIX = EMBED ? 'r2t2e.' : 'r2t2.';
  const store = {
    get(k, d) { try { const v = localStorage.getItem(PREFIX + k); return v === null ? d : JSON.parse(v); } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem(PREFIX + k, JSON.stringify(v)); } catch (e) { /* storage unavailable */ } },
  };

  // ------------------------------------------------------------ toasts

  function toast(msg, kind = 'info') {
    if (EMBED) { console.warn('r2t2sdr:', msg); return; }
    const t = document.createElement('div');
    t.className = `sdr-toast ${kind}`;
    t.textContent = msg;
    $('toasts').appendChild(t);
    setTimeout(() => t.remove(), 5000);
  }

  // ------------------------------------------------------------ colour map

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

  function cssVar(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim() || '#8a93b3';
  }

  // ------------------------------------------------------------ canvases

  const spec = $('spectrum'), scale = $('scale'), wf = $('waterfall');
  const specCtx = spec.getContext('2d'), scaleCtx = scale.getContext('2d'), wfCtx = wf.getContext('2d');
  // history in data resolution, scaled onto the visible canvas
  const hist = document.createElement('canvas');
  hist.width = 1024;
  hist.height = 400;
  const histCtx = hist.getContext('2d');
  const line = histCtx.createImageData(1024, 1);
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
    [spec, scale, wf].forEach(fitCanvas);
    wfCtx.imageSmoothingEnabled = false;
    drawWaterfall();
    drawScale();
    drawSpectrum();
    updatePassband();
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

  function drawSpectrum() {
    const w = spec.width, h = spec.height;
    specCtx.fillStyle = '#05070f';
    specCtx.fillRect(0, 0, w, h);
    const lo = st.wfMin - 10, hi = st.wfMax + 20, range = hi - lo;

    specCtx.strokeStyle = 'rgba(238,240,250,0.07)';
    specCtx.lineWidth = 1;
    specCtx.fillStyle = 'rgba(138,147,179,0.8)';
    specCtx.font = `${Math.round(10 * dpr())}px system-ui, sans-serif`;
    for (let db = Math.ceil(lo / 10) * 10; db <= hi; db += 10) {
      const y = h - ((db - lo) / range) * h;
      specCtx.beginPath();
      specCtx.moveTo(0, y + 0.5);
      specCtx.lineTo(w, y + 0.5);
      specCtx.stroke();
      if (!EMBED || h > 80) specCtx.fillText(`${db - 170}`, 4, y - 2);
    }

    const pb = passbandRange();
    if (pb) {
      const x0 = freqToX(pb[0], w), x1 = freqToX(pb[1], w);
      specCtx.fillStyle = 'rgba(106,166,255,0.16)';
      specCtx.fillRect(x0, 0, Math.max(1, x1 - x0), h);
      specCtx.fillStyle = 'rgba(106,166,255,0.95)';
      specCtx.fillRect(Math.round(freqToX(st.freq, w)), 0, Math.max(1, Math.round(dpr())), h);
    }

    const s = st.smooth;
    if (!s) return;
    specCtx.beginPath();
    for (let i = 0; i < s.length; i++) {
      const x = (i / (s.length - 1)) * w;
      const y = h - ((s[i] - lo) / range) * h;
      if (i === 0) specCtx.moveTo(x, y); else specCtx.lineTo(x, y);
    }
    specCtx.strokeStyle = '#8fc2ff';
    specCtx.lineWidth = 1.2 * dpr();
    specCtx.stroke();
    specCtx.lineTo(w, h);
    specCtx.lineTo(0, h);
    specCtx.closePath();
    specCtx.fillStyle = 'rgba(106,166,255,0.13)';
    specCtx.fill();
  }

  function drawScale() {
    const w = scale.width, h = scale.height, r = dpr();
    scaleCtx.clearRect(0, 0, w, h);
    if (!st.center) return;
    const start = st.center - st.span / 2, end = st.center + st.span / 2;
    const pxPerKHz = (w / st.span) * 1000;
    const major = [1, 2, 5, 10, 20, 25, 50].find((k) => k * pxPerKHz > 70 * r) || 100;
    const minor = major / 5;
    const muted = EMBED ? '#8a93b3' : cssVar('--muted');
    scaleCtx.strokeStyle = muted;
    scaleCtx.fillStyle = EMBED ? '#eef0fa' : cssVar('--fg');
    scaleCtx.font = `${Math.round(11 * r)}px system-ui, sans-serif`;
    scaleCtx.textAlign = 'center';
    scaleCtx.textBaseline = 'bottom';
    scaleCtx.beginPath();
    for (let f = Math.ceil(start / (minor * 1000)) * minor * 1000; f <= end; f += minor * 1000) {
      const x = Math.round(freqToX(f, w)) + 0.5;
      const isMajor = Math.abs(f / 1000 / major - Math.round(f / 1000 / major)) < 1e-6;
      scaleCtx.moveTo(x, h);
      scaleCtx.lineTo(x, h - (isMajor ? 6 : 3) * r);
      if (isMajor) scaleCtx.fillText((f / 1000).toFixed(0), x, h - 6 * r);
    }
    scaleCtx.stroke();
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
  }

  function onWaterfall(bins) {
    if (st.autoFrames > 0 && --st.autoFrames === 0) autoLevels(bins);
    if (!st.smooth || st.smooth.length !== bins.length) st.smooth = Float32Array.from(bins);
    else for (let i = 0; i < bins.length; i++) st.smooth[i] += (bins[i] - st.smooth[i]) * 0.35;
    st.lastBins = bins;
    pushWaterfall(bins);
    drawSpectrum();
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
    if (gainNode) gainNode.gain.value = v * v * 2;
    store.set('vol', $('vol').value);
  }

  function onAudio(buf) {
    const dv = new DataView(buf);
    updateSmeter(dv.getInt16(1, true) / 10);
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

  function updateSmeter(db) {
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
    b.textContent = on ? '■ Stopp' : '▶ Hören';
    b.classList.toggle('an', on);
    if (!on) { $('smbar').style.width = '0'; $('smval').textContent = '– dBFS'; }
  }

  // ------------------------------------------------------------ frequency display

  function renderFreq() {
    if (st.editing) return;
    const el = $('freq');
    const digits = String(Math.round(st.freq)).padStart(8, '0');
    const first = Math.min(digits.search(/[1-9]/) < 0 ? 7 : digits.search(/[1-9]/), 7);
    let html = '';
    for (let i = 0; i < digits.length; i++) {
      const pos = digits.length - 1 - i;
      const cls = i < first ? 'ziffer fuehrend' : 'ziffer';
      html += `<span class="${cls}" data-mult="${10 ** pos}">${digits[i]}</span>`;
      if (pos === 6 || pos === 3) html += '<span class="trenner">.</span>';
    }
    el.innerHTML = html;
  }

  function editFreq() {
    if (st.editing) return;
    st.editing = true;
    const el = $('freq');
    const inp = document.createElement('input');
    inp.inputMode = 'decimal';
    inp.value = (st.freq / 1000).toFixed(2);
    inp.setAttribute('aria-label', 'Frequenz in kHz');
    el.innerHTML = '';
    el.appendChild(inp);
    inp.focus();
    inp.select();
    const done = (apply) => {
      if (!st.editing) return;
      st.editing = false;
      const khz = parseFloat(inp.value.trim().replace(',', '.'));
      if (apply && isFinite(khz) && khz > 0) tuneTo(khz * 1000);
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
    st.freq = f;
    renderFreq();
    store.set('freq', f);
    ensureView(f);
    updatePassband();
    drawSpectrum();
    sendTune();
  }

  // Waterfall follows the frequency: configured band if one covers it, otherwise a free centre.
  function ensureView(f, force) {
    if (!st.cfg || !st.ws || st.ws.readyState !== 1) return;
    const inner = (st.span / 2) * VIEW_INNER;
    if (!force && st.view >= 0 && Math.abs(f - st.center) < inner) return;
    let best = -1, bestDist = Infinity;
    st.cfg.bands.forEach((b) => {
      const d = Math.abs(f - b.center);
      if (d < inner && d < bestDist) { best = b.id; bestDist = d; }
    });
    if (best >= 0) { requestBand(best); return; }
    const c = Math.round(f / VIEW_GRID) * VIEW_GRID;
    if (!force && st.view >= 0 && st.viewBand < 0 && Math.abs(c - st.center) < 1) return;
    if (st.pending === `c${c}`) return;
    st.pending = `c${c}`;
    send({ cmd: 'view', center: c });
  }

  function requestBand(id) {
    if (st.viewBand === id && st.view >= 0) return;
    if (st.pending === `b${id}`) return;
    st.pending = `b${id}`;
    send({ cmd: 'band', id });
  }

  function onView(m) {
    const changed = st.view !== m.id;
    st.pending = null;
    st.view = m.id;
    st.viewBand = m.band;
    st.center = m.center;
    st.span = m.span;
    if (changed) clearHistory();
    syncBandSelect();
    if (st.userBand !== null && st.userBand === m.band) {
      // band chosen in the list: go to its centre with its default mode
      const b = st.cfg.bands[m.band];
      st.userBand = null;
      setMode(MODES[b.mode] ? b.mode : 'usb', true);
      tuneTo(b.center);
    }
    drawScale();
    updatePassband();
    drawWaterfall();
    drawSpectrum();
  }

  function syncBandSelect() {
    const sel = $('band');
    let free = sel.querySelector('option[data-frei]');
    if (st.viewBand >= 0) {
      if (free) free.remove();
      sel.value = st.viewBand;
    } else {
      if (!free) {
        free = document.createElement('option');
        free.dataset.frei = '1';
        sel.insertBefore(free, sel.firstChild);
      }
      free.value = 'frei';
      free.textContent = `Frei, ${(st.center / 1e6).toFixed(3)} MHz`;
      sel.value = 'frei';
    }
  }

  function clickTune(ev, canvas) {
    const r = canvas.getBoundingClientRect();
    const f = xToFreq(ev.clientX - r.left, r.width);
    tuneTo(f, true);
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
    $('hover').textContent = `${(f / 1000).toFixed(2)} kHz`;
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

  // ------------------------------------------------------------ websocket

  function send(obj) {
    if (st.ws && st.ws.readyState === 1) st.ws.send(JSON.stringify(obj));
  }

  function onConfig(c) {
    st.cfg = c;
    st.span = c.span;
    document.title = c.title || 'R2T2 WebSDR';
    $('title').textContent = c.title || 'R2T2 WebSDR';
    $('station').textContent = [c.callsign, c.location, c.locator].filter(Boolean).join(' · ');
    const sel = $('band');
    sel.innerHTML = '';
    c.bands.forEach((b) => {
      const o = document.createElement('option');
      o.value = b.id;
      o.textContent = `${b.name} (${(b.center / 1e6).toFixed(3)} MHz)`;
      sel.appendChild(o);
    });

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
    $('users').textContent = `${m.users} Nutzer · ${m.listeners} hören`;
    $('fuss').innerHTML = `R2T2 WebSDR ${st.cfg ? st.cfg.version : ''} · ${m.free} von ${m.total} Empfängern frei`
      + ' · Schnittstelle: <a href="api/status">api/status</a>, <a href="api/config">api/config</a>';
  }

  function setConn(on) {
    const el = $('conn');
    el.textContent = on ? 'Verbunden' : 'Getrennt';
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
          case 'audio': setListening(m.on); if (m.on) st.wantAudio = true; break;
          case 'error': st.pending = null; toast(m.msg, 'fehler'); break;
          default: break;
        }
        return;
      }
      const u8 = new Uint8Array(ev.data);
      if (u8[0] === 1) {
        if (u8[1] === st.view) onWaterfall(u8.subarray(2));
      } else if (u8[0] === 2) {
        onAudio(ev.data);
      }
    };
  }

  // ------------------------------------------------------------ wiring

  function tickClock() {
    const d = new Date();
    $('utc').textContent = `${d.toISOString().slice(11, 19)} UTC`;
    $('datum').textContent = d.toLocaleDateString('de-DE', { timeZone: 'UTC', weekday: 'short', day: '2-digit', month: '2-digit', year: 'numeric' });
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

    [spec, wf, scale].forEach((c) => {
      c.addEventListener('click', (e) => clickTune(e, c));
      c.addEventListener('wheel', wheelTune, { passive: false });
      c.addEventListener('mousemove', (e) => hoverInfo(e, c));
      c.addEventListener('mouseleave', () => { $('hover').textContent = ''; });
    });
    window.addEventListener('hashchange', applyHash);

    if (!EMBED) {
      $('audio').addEventListener('click', toggleAudio);
      $('vol').addEventListener('input', setVolume);
      $('mode').addEventListener('change', (e) => setMode(e.target.value));
      $('band').addEventListener('change', (e) => {
        if (e.target.value === 'frei') return;
        st.userBand = Number(e.target.value);
        requestBand(st.userBand);
      });
      $('bw').addEventListener('change', (e) => { st.bw = Number(e.target.value); tuneTo(st.freq); });
      $('step').addEventListener('change', (e) => { st.step = Number(e.target.value); });
      $('ab').addEventListener('click', () => tuneTo(Math.round(st.freq / st.step) * st.step - st.step));
      $('auf').addEventListener('click', () => tuneTo(Math.round(st.freq / st.step) * st.step + st.step));
      $('sql').addEventListener('input', (e) => {
        const v = Number(e.target.value);
        const off = v <= -140;
        $('sqlval').textContent = off ? 'aus' : `${v} dBFS`;
        send({ cmd: 'squelch', level: off ? -999 : v });
      });
      $('wfmin').addEventListener('input', (e) => { st.wfMin = Math.min(Number(e.target.value), st.wfMax - 5); });
      $('wfmax').addEventListener('input', (e) => { st.wfMax = Math.max(Number(e.target.value), st.wfMin + 5); });
      $('auto').addEventListener('click', () => { if (st.lastBins) autoLevels(st.lastBins); });

      const fr = $('freq');
      fr.addEventListener('click', editFreq);
      fr.addEventListener('wheel', (e) => {
        e.preventDefault();
        const mult = Number(e.target.dataset && e.target.dataset.mult) || st.step;
        tuneTo(st.freq + (e.deltaY < 0 ? mult : -mult));
      }, { passive: false });
      fr.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') { e.preventDefault(); editFreq(); }
      });
      document.addEventListener('keydown', (e) => {
        if (e.target instanceof HTMLInputElement || e.target instanceof HTMLSelectElement) return;
        if (e.key === 'ArrowRight' || e.key === 'ArrowUp') { tuneTo(st.freq + st.step); e.preventDefault(); }
        if (e.key === 'ArrowLeft' || e.key === 'ArrowDown') { tuneTo(st.freq - st.step); e.preventDefault(); }
      });
      tickClock();
      setInterval(tickClock, 1000);
    }

    if (window.ResizeObserver) new ResizeObserver(resize).observe($('wfbox'));
    else window.addEventListener('resize', resize);
    // scale colours come from the theme tokens
    const scheme = window.matchMedia && window.matchMedia('(prefers-color-scheme: light)');
    if (scheme && scheme.addEventListener) scheme.addEventListener('change', drawScale);
    resize();
    connect();
  }

  init();
})();
