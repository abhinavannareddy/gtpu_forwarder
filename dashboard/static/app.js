/* GTP-U Control Room - front end. No dependencies, no network calls beyond the local API. */
(function () {
  'use strict';

  const $ = (sel, root) => (root || document).querySelector(sel);
  const el = (tag, attrs, ...kids) => {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (k === 'class') n.className = v; else if (k === 'text') n.textContent = v; else n.setAttribute(k, v);
    }
    kids.forEach(k => n.append(k));
    return n;
  };
  const css = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  const nf = new Intl.NumberFormat('en-US');
  const fmtInt = n => nf.format(Math.round(n));

  function fmtRate(n) {            // packets or bits per second, compact
    if (n >= 1e6) return (n / 1e6).toFixed(2) + ' M';
    if (n >= 1e4) return (n / 1e3).toFixed(1) + ' k';
    return fmtInt(n);
  }
  function fmtBits(bps) {
    if (bps >= 1e9) return (bps / 1e9).toFixed(2) + ' Gbit/s';
    if (bps >= 1e6) return (bps / 1e6).toFixed(2) + ' Mbit/s';
    if (bps >= 1e3) return (bps / 1e3).toFixed(1) + ' kbit/s';
    return Math.round(bps) + ' bit/s';
  }
  function fmtBytes(b) {
    const u = ['B', 'KB', 'MB', 'GB', 'TB'];
    let i = 0;
    while (b >= 1024 && i < u.length - 1) { b /= 1024; i++; }
    return (i ? b.toFixed(1) : Math.round(b)) + ' ' + u[i];
  }
  function fmtUptime(s) {
    s = Math.floor(s);
    const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
    if (d) return `${d}d ${h}h`;
    if (h) return `${h}h ${m}m`;
    return m ? `${m}m ${s % 60}s` : `${s}s`;
  }

  function toast(msg, isErr) {
    const t = el('div', { class: 'toast' + (isErr ? ' err' : ''), role: 'status', text: msg });
    $('#toasts').append(t);
    setTimeout(() => t.remove(), 4200);
  }

  async function api(method, path, body) {
    const res = await fetch(path, {
      method, headers: body ? { 'Content-Type': 'application/json' } : undefined,
      body: body ? JSON.stringify(body) : undefined,
    });
    let data = null;
    try { data = await res.json(); } catch (e) { /* non-JSON error page */ }
    if (!res.ok && !(data && data.ok === false)) throw new Error((data && data.error) || res.statusText);
    return data;
  }

  /* ---------------- theme ---------------- */
  try {
    const saved = localStorage.getItem('theme');
    if (saved) document.documentElement.dataset.theme = saved;
  } catch (e) { /* storage blocked */ }
  $('#theme').addEventListener('click', () => {
    const cur = document.documentElement.dataset.theme ||
      (matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark');
    const next = cur === 'dark' ? 'light' : 'dark';
    document.documentElement.dataset.theme = next;
    try { localStorage.setItem('theme', next); } catch (e) { /* ignore */ }
    drawAll();
  });

  /* ---------------- data feed ---------------- */
  const HISTORY_MAX = 240;
  let history = [];
  let latest = { online: false };
  let synced = false;                     // traffic controls adopt server state once

  function pushSample(s) {
    const last = history[history.length - 1];
    if (last && last.t === s.t) return;
    history.push({ t: s.t, pps_rx: s.pps_rx, pps_fwd: s.pps_fwd, pps_drop: s.pps_drop, bps: s.bps });
    if (history.length > HISTORY_MAX) history.shift();
  }

  function onSnapshot(s) {
    latest = s;
    const conn = $('#conn');
    $('#offline').hidden = !!s.online;
    if (!s.online) {
      conn.className = 'pill pill-bad'; conn.lastChild.textContent = 'offline';
      if (s.error) $('#offline-msg').textContent = s.error + '.';
      return;
    }
    conn.className = 'pill pill-on'; conn.lastChild.textContent = 'live';
    pushSample(s);
    renderKpis(s); renderDrops(s); renderTrafficStatus(s);
    if (!synced && s.traffic) { syncTrafficControls(s.traffic); synced = true; }
    drawAll();
  }

  async function boot() {
    try {
      const h = await api('GET', '/api/history');
      history = h.samples.map(x => ({ t: x.t, pps_rx: x.pps_rx, pps_fwd: x.pps_fwd, pps_drop: x.pps_drop, bps: x.bps }));
      onSnapshot(await api('GET', '/api/stats'));
    } catch (e) { onSnapshot({ online: false, error: 'Dashboard server unreachable' }); }
    const es = new EventSource('/api/stream');
    es.onmessage = ev => onSnapshot(JSON.parse(ev.data));
    es.onerror = () => { if (es.readyState === EventSource.CLOSED) onSnapshot({ online: false, error: 'Lost connection to the dashboard server' }); };
    refreshRoutes();
    setInterval(refreshRoutes, 2000);
  }

  /* ---------------- KPIs ---------------- */
  function renderKpis(s) {
    $('#k-pps').textContent = fmtRate(s.pps_rx);
    $('#k-fwd').textContent = fmtInt(s.forwarded);
    $('#k-fwd-sub').textContent = s.rx ? `${((s.forwarded / s.rx) * 100).toFixed(1)}% of ingress` : ' ';
    $('#k-drop').textContent = fmtInt(s.dropped);
    $('#k-drop-sub').textContent = s.rx ? `${((s.dropped / s.rx) * 100).toFixed(1)}% of ingress` : ' ';
    const bits = fmtBits(s.bps).split(' ');
    $('#k-bps').textContent = bits[0];
    $('#k-bps-unit').textContent = ' ' + bits[1];
    $('#k-echo').textContent = fmtInt(s.echo_replied);
    $('#k-up').textContent = fmtUptime(s.uptime_s);
    $('#k-routes').textContent = `${s.routes} route${s.routes === 1 ? '' : 's'} active`;
    renderEngine(s);
  }

  function renderEngine(s) {
    const chip = (label, on) => el('li', { class: on === false ? 'off' : '' }, el('i'), label);
    $('#engine').replaceChildren(
      chip(`gtpu-fwd ${s.version || ''}`.trim()),
      chip(`${s.workers || 1} worker${s.workers === 1 ? '' : 's'}`),
      chip(s.strict_inner ? 'strict inner-IP check' : 'inner packets not inspected', !!s.strict_inner),
      chip(s.error_indication ? `Error Indications on (${fmtInt(s.error_indications || 0)} sent)` : 'Error Indications off', !!s.error_indication));
  }

  function sparkline(id, key, colorVar) {
    const c = $(id), dpr = devicePixelRatio || 1;
    const w = c.clientWidth, h = c.clientHeight;
    if (!w) return;
    c.width = w * dpr; c.height = h * dpr;
    const g = c.getContext('2d'); g.scale(dpr, dpr);
    const data = history.slice(-60).map(p => p[key]);
    if (data.length < 2) return;
    const max = Math.max(...data, 1);
    const x = i => (i / (data.length - 1)) * w, y = v => h - 3 - (v / max) * (h - 8);
    const col = css(colorVar);
    g.beginPath(); data.forEach((v, i) => (i ? g.lineTo(x(i), y(v)) : g.moveTo(x(i), y(v))));
    g.lineTo(w, h); g.lineTo(0, h); g.closePath();
    const grad = g.createLinearGradient(0, 0, 0, h);
    grad.addColorStop(0, col + '55'); grad.addColorStop(1, col + '00');
    g.fillStyle = grad; g.fill();
    g.beginPath(); data.forEach((v, i) => (i ? g.lineTo(x(i), y(v)) : g.moveTo(x(i), y(v))));
    g.strokeStyle = col; g.lineWidth = 2; g.lineJoin = 'round'; g.stroke();
  }

  /* ---------------- main chart ---------------- */
  const SERIES = [
    { key: 'pps_rx', label: 'Received', color: '--c-rx', on: true },
    { key: 'pps_fwd', label: 'Forwarded', color: '--c-fwd', on: true },
    { key: 'pps_drop', label: 'Dropped', color: '--c-drop', on: true },
  ];
  let hoverIdx = null;
  const geom = { l: 52, r: 14, t: 12, b: 26 };

  const legend = $('#legend');
  SERIES.forEach(s => {
    const b = el('button', { type: 'button', 'aria-pressed': 'true' }, el('i'), s.label);
    b.firstChild.style.setProperty('--c', `var(${s.color})`);
    b.addEventListener('click', () => {
      s.on = !s.on; b.setAttribute('aria-pressed', String(s.on)); drawChart();
    });
    legend.append(el('li', {}, b));
  });

  function niceMax(v) {
    if (v <= 10) return 10;
    const p = Math.pow(10, Math.floor(Math.log10(v)));
    for (const m of [1, 2, 2.5, 5, 10]) if (v <= m * p) return m * p;
    return 10 * p;
  }

  function drawChart() {
    const c = $('#chart'), dpr = devicePixelRatio || 1;
    const w = c.clientWidth, h = c.clientHeight;
    if (!w || !h) return;
    c.width = w * dpr; c.height = h * dpr;
    const g = c.getContext('2d'); g.scale(dpr, dpr);
    const pw = w - geom.l - geom.r, ph = h - geom.t - geom.b;
    const active = SERIES.filter(s => s.on);
    const max = niceMax(Math.max(1, ...history.flatMap(p => active.map(s => p[s.key]))));
    const X = i => geom.l + pw - ((history.length - 1 - i) / (HISTORY_MAX - 1)) * pw;   // newest sample at the right edge
    const Y = v => geom.t + ph - (v / max) * ph;
    const grid = css('--line'), ink = css('--ink-3');

    g.font = '11px system-ui, sans-serif'; g.textBaseline = 'middle';
    for (let i = 0; i <= 4; i++) {
      const v = (max / 4) * i, y = Y(v);
      g.strokeStyle = grid; g.globalAlpha = i ? .55 : 1; g.lineWidth = 1;
      g.beginPath(); g.moveTo(geom.l, y + .5); g.lineTo(w - geom.r, y + .5); g.stroke(); g.globalAlpha = 1;
      g.fillStyle = ink; g.textAlign = 'right'; g.fillText(fmtRate(v), geom.l - 8, y);
    }
    g.textAlign = 'center'; g.textBaseline = 'top';
    [[0, '-2 min'], [.5, '-1 min'], [1, 'now']].forEach(([f, t]) => {
      g.textAlign = f === 0 ? 'left' : f === 1 ? 'right' : 'center';
      g.fillText(t, geom.l + f * pw, h - geom.b + 8);
    });

    if (history.length < 2) {
      g.fillStyle = ink; g.textAlign = 'center'; g.textBaseline = 'middle';
      g.fillText('Waiting for samples…', w / 2, h / 2); return;
    }
    active.forEach(s => {
      g.beginPath();
      history.forEach((p, i) => (i ? g.lineTo(X(i), Y(p[s.key])) : g.moveTo(X(i), Y(p[s.key]))));
      g.strokeStyle = css(s.color); g.lineWidth = 2; g.lineJoin = 'round'; g.lineCap = 'round'; g.stroke();
    });
    if (hoverIdx !== null && history[hoverIdx]) {
      const x = X(hoverIdx);
      g.strokeStyle = ink; g.globalAlpha = .6; g.setLineDash([3, 3]);
      g.beginPath(); g.moveTo(x + .5, geom.t); g.lineTo(x + .5, geom.t + ph); g.stroke();
      g.setLineDash([]); g.globalAlpha = 1;
      active.forEach(s => {
        const y = Y(history[hoverIdx][s.key]);
        g.beginPath(); g.arc(x, y, 5, 0, 7); g.fillStyle = css('--panel'); g.fill();   // surface ring
        g.beginPath(); g.arc(x, y, 3.5, 0, 7); g.fillStyle = css(s.color); g.fill();
      });
    }
    c._X = X;
  }

  function onChartMove(ev) {
    const c = $('#chart'), r = c.getBoundingClientRect();
    if (!c._X || history.length < 2) return;
    const mx = ev.clientX - r.left;
    let best = 0, bd = Infinity;
    history.forEach((_, i) => { const d = Math.abs(c._X(i) - mx); if (d < bd) { bd = d; best = i; } });
    hoverIdx = best;
    const p = history[best], tip = $('#tip');
    tip.replaceChildren(el('b', { text: new Date(p.t * 1000).toLocaleTimeString() }),
      ...SERIES.filter(s => s.on).map(s => {
        const i = el('i'); i.style.setProperty('--c', `var(${s.color})`);
        return el('div', {}, el('span', {}, i, s.label), el('b', { text: fmtInt(p[s.key]) + ' /s' }));
      }));
    tip.hidden = false;
    const x = c._X(best);
    tip.style.left = Math.min(x + 14, r.width - 170) + 'px';
    tip.style.top = '14px';
    drawChart();
  }
  $('#chart').addEventListener('pointermove', onChartMove);
  $('#chart').addEventListener('pointerleave', () => { hoverIdx = null; $('#tip').hidden = true; drawChart(); });

  $('#tbl-toggle').addEventListener('click', ev => {
    const on = ev.currentTarget.getAttribute('aria-pressed') !== 'true';
    ev.currentTarget.setAttribute('aria-pressed', String(on));
    $('#chart-wrap').hidden = on; $('#chart-table').hidden = !on;
    if (on) renderChartTable();
  });
  function renderChartTable() {
    const rows = history.slice(-30).reverse().map(p => el('tr', {},
      el('td', { text: new Date(p.t * 1000).toLocaleTimeString() }),
      ...SERIES.map(s => el('td', { class: 'num', text: fmtInt(p[s.key]) }))));
    $('#chart-table').replaceChildren(el('table', {},
      el('thead', {}, el('tr', {}, el('th', { text: 'Time' }), ...SERIES.map(s => el('th', { class: 'num', text: s.label + ' /s' })))),
      el('tbody', {}, ...rows)));
  }

  function drawAll() {
    drawChart();
    sparkline('#s-pps', 'pps_rx', '--c-rx'); sparkline('#s-fwd', 'pps_fwd', '--c-fwd');
    sparkline('#s-drop', 'pps_drop', '--c-drop'); sparkline('#s-bps', 'bps', '--c-rx');
    if (!$('#chart-table').hidden) renderChartTable();
  }
  addEventListener('resize', drawAll);

  /* ---------------- drops ---------------- */
  const DROP_INFO = {
    no_route: ['No route for TEID', 'Valid packet, but no tunnel is configured.'],
    bad_inner_ip: ['Invalid inner IP packet', 'Strict mode: inner IPv4/IPv6 header is malformed.'],
    unsupported_type: ['Unsupported message type', 'Only G-PDU and Echo Request are handled.'],
    too_short: ['Datagram too short', 'Shorter than the 8-byte mandatory header.'],
    bad_version: ['Wrong GTP version', 'Version field is not 1.'],
    bad_protocol_type: ['GTP′ (PT bit clear)', 'Charging variant, not user-plane.'],
    length_mismatch: ['Length field mismatch', 'Header length disagrees with the datagram size.'],
    bad_extension_header: ['Bad extension header', 'Zero-length or out-of-bounds extension chain.'],
  };
  function renderDrops(s) {
    const items = Object.entries(s.drops || {}).map(([k, v]) => [k, v]);
    items.push(['no_route', s.drop_no_route], ['unsupported_type', s.drop_unsupported_type]);
    const rows = items.filter(([, v]) => v > 0).sort((a, b) => b[1] - a[1]);
    const total = rows.reduce((a, [, v]) => a + v, 0);
    $('#drops-empty').hidden = rows.length > 0;
    const max = rows.length ? rows[0][1] : 1;
    $('#drops').replaceChildren(...rows.map(([k, v]) => {
      const [name, why] = DROP_INFO[k] || [k, ''];
      const fill = el('div', { class: 'fill' }); fill.style.width = (v / max * 100) + '%';
      return el('li', {},
        el('div', { class: 'row' }, el('span', { text: name }), el('span', { text: `${fmtInt(v)} · ${(v / total * 100).toFixed(0)}%` })),
        el('div', { class: 'track' }, fill), el('div', { class: 'why', text: why }));
    }));
  }

  /* ---------------- routes ---------------- */
  let routes = [];
  let armed = null;

  let routesSeq = 0;
  async function refreshRoutes() {
    const seq = ++routesSeq;                // a slower, older response must not overwrite a newer one
    try {
      const res = await api('GET', '/api/routes');
      if (seq === routesSeq) { routes = res.routes; renderRoutes(); }
    } catch (e) { /* offline banner covers it */ }
  }
  function renderRoutes() {
    const q = $('#route-filter').value.trim().toLowerCase();
    const total = routes.reduce((a, r) => a + r.pkts, 0) || 1;
    const rows = routes.filter(r => !q || String(r.in_teid).includes(q) || String(r.out_teid).includes(q) ||
      (r.label || '').toLowerCase().includes(q) || r.next_hop.includes(q)).sort((a, b) => a.in_teid - b.in_teid);
    $('#routes-empty').hidden = rows.length > 0;
    $('#route-table tbody').replaceChildren(...rows.map(r => {
      const share = el('div', { class: 'share' }, el('div'));
      share.firstChild.style.width = (r.pkts / total * 100) + '%';
      share.title = `${(r.pkts / total * 100).toFixed(1)}% of forwarded packets`;
      const del = el('button', { class: 'btn link' + (armed === r.in_teid ? ' armed' : ''), type: 'button',
        'aria-label': `Delete route ${r.in_teid}`, text: armed === r.in_teid ? 'Confirm' : 'Delete' });
      del.addEventListener('click', async () => {
        if (armed !== r.in_teid) { armed = r.in_teid; renderRoutes(); setTimeout(() => { if (armed === r.in_teid) { armed = null; renderRoutes(); } }, 3000); return; }
        armed = null;
        try { await api('DELETE', `/api/routes/${r.in_teid}`); toast(`Route ${r.in_teid} removed`); } catch (e) { toast(e.message, true); }
        refreshRoutes();
      });
      return el('tr', {}, el('td', { text: r.label || '–' }), el('td', { class: 'num mono', text: r.in_teid }),
        el('td', { class: 'num mono', text: r.out_teid }), el('td', { class: 'mono', text: r.next_hop }),
        el('td', { class: 'num', text: fmtInt(r.pkts) }), el('td', { class: 'num', text: fmtBytes(r.bytes) }),
        el('td', {}, share), el('td', {}, del));
    }));
  }
  $('#route-filter').addEventListener('input', renderRoutes);
  $('#route-form').addEventListener('submit', async ev => {
    ev.preventDefault();
    const form = ev.currentTarget;           // currentTarget is null once we await
    const f = new FormData(form);
    try {
      const res = await api('POST', '/api/routes', {
        in_teid: Number(f.get('in_teid')), out_teid: Number(f.get('out_teid')), next_hop: String(f.get('next_hop')).trim() });
      if (res.ok) { toast(`Route ${f.get('in_teid')} added`); form.reset(); } else toast(res.error, true);
    } catch (e) { toast(e.message, true); }
    refreshRoutes();
  });

  /* ---------------- packet lab ---------------- */
  const P = window.GtpuPacket;
  let labOpts = Object.assign({}, P.DEFAULTS);
  let currentPreset = 'valid';
  let lastBuilt = null;

  function buildPresets() {
    $('#presets').replaceChildren(...P.PRESETS.map(p => {
      const b = el('button', { type: 'button', class: 'chip ' + (p.expect === 'drop' ? 'bad' : 'good'), 'aria-pressed': 'false', 'data-id': p.id, text: p.label });
      b.addEventListener('click', () => selectPreset(p.id));
      return b;
    }));
  }
  function selectPreset(id) {
    const p = P.PRESETS.find(x => x.id === id);
    currentPreset = id;
    labOpts = Object.assign({}, P.DEFAULTS, p.opts);
    document.querySelectorAll('#presets .chip').forEach(c => c.setAttribute('aria-pressed', String(c.dataset.id === id)));
    $('#preset-blurb').textContent = p.blurb;
    verdictSeq++; syncForm(); renderLab(); $('#verdict').hidden = true;
  }
  function syncForm() {
    const form = $('#lab-form');
    for (const inp of form.elements) {
      if (!inp.name) continue;
      if (inp.type === 'checkbox') inp.checked = !!labOpts[inp.name]; else inp.value = labOpts[inp.name];
    }
  }
  $('#lab-form').addEventListener('input', ev => {
    const inp = ev.target;
    if (!inp.name) return;
    labOpts[inp.name] = inp.type === 'checkbox' ? inp.checked :
      inp.tagName === 'SELECT' && (inp.name === 'ext' || inp.name === 'payloadKind') ? inp.value : (inp.value === '' ? 0 : Number(inp.value));
    labOpts.truncateTo = null;
    currentPreset = null;
    document.querySelectorAll('#presets .chip').forEach(c => c.setAttribute('aria-pressed', 'false'));
    $('#preset-blurb').textContent = 'Custom packet.';
    renderLab();
  });

  function renderLab() {
    lastBuilt = P.build(labOpts);
    const { bytes, fields } = lastBuilt;
    const fieldOf = i => fields.findIndex(f => i >= f.start && i < f.end);
    const colorOf = f => `var(--${f.cls})`;
    const grid = $('#hex');
    grid.replaceChildren(...bytes.map((b, i) => {
      const fi = fieldOf(i), s = el('span', { text: P.toHex([b]), 'data-f': fi });
      s.style.setProperty('--fc', colorOf(fields[fi])); s.title = fields[fi].name;
      return s;
    }));
    $('#byte-count').textContent = `(${bytes.length} bytes)`;
    $('#field-legend').replaceChildren(...fields.map((f, fi) => {
      const li = el('li', {}, el('i'), el('b', { text: f.name }), el('span', { text: f.desc }));
      li.firstChild.style.setProperty('--fc', colorOf(f));
      const set = on => grid.querySelectorAll('span').forEach(s => {
        s.classList.toggle('hl', on && s.dataset.f === String(fi)); s.classList.toggle('dim', on && s.dataset.f !== String(fi));
      });
      li.addEventListener('mouseenter', () => set(true)); li.addEventListener('mouseleave', () => set(false));
      return li;
    }));
  }

  const ACTION_TEXT = {
    forward: r => `The TEID is rewritten ${r.route.in_teid} → ${r.route.out_teid} and the datagram is sent to ${r.route.next_hop}.`,
    echo_response: () => 'Path-management check: the forwarder answers with an Echo Response carrying the same sequence number.',
    drop: r => `Dropped and counted as “${(DROP_INFO[r.reason] || [r.reason])[0]}”. ${(DROP_INFO[r.reason] || [])[1] || ''}`,
  };
  let verdictSeq = 0;
  function showVerdict(r, extra) {
    const v = $('#verdict');
    v.hidden = false;
    v.className = 'verdict ' + r.action;
    const tag = { forward: 'FORWARD', echo_response: 'ECHO RESPONSE', drop: 'DROP' }[r.action];
    const meta = r.status === 'ok'
      ? `type ${r.msg_type} · TEID ${r.teid} · header ${r.header_len} B · payload ${r.payload_len} B`
      : `parser status: ${r.status} · ${r.length} bytes`;
    v.replaceChildren(el('span', { class: 'tag', text: tag }), el('p', { text: ACTION_TEXT[r.action](r) }),
      el('p', { class: 'meta', text: meta }), ...(extra ? [el('p', { class: 'meta', text: extra })] : []));
  }
  async function labInspect() {
    const r = await api('POST', '/api/inspect', { hex: P.toHex(lastBuilt.bytes) });
    if (!r.ok) throw new Error(r.error);
    return r;
  }
  $('#btn-inspect').addEventListener('click', async () => {
    const seq = ++verdictSeq;
    try { const r = await labInspect(); if (seq === verdictSeq) showVerdict(r, 'Dry run: nothing was sent.'); }
    catch (e) { toast(e.message, true); }
  });
  async function labSend(count) {
    const seq = ++verdictSeq;
    try {
      const verdict = await labInspect();
      const sent = await api('POST', '/api/send', { hex: P.toHex(lastBuilt.bytes), count });
      let extra = `Sent ${sent.sent} datagram${sent.sent > 1 ? 's' : ''}.`;
      if (sent.reply_hex) {
        const rb = P.fromHex(sent.reply_hex);
        extra += ` Reply received: ${P.MSG_TYPES[rb[1]] || 'type ' + rb[1]}, ${rb.length} bytes (${sent.reply_hex}).`;
        if (rb[1] === 26 && rb.length >= 17) {
          const td = ((rb[13] << 24) | (rb[14] << 16) | (rb[15] << 8) | rb[16]) >>> 0;
          extra += ` Error Indication: the peer reports it has no tunnel for TEID ${td}.`;
        }
      }
      if (seq === verdictSeq) showVerdict(verdict, extra);
      toast(`Sent ${sent.sent} packet${sent.sent > 1 ? 's' : ''}`);
    } catch (e) { toast(e.message, true); }
  }
  $('#btn-send').addEventListener('click', () => labSend(1));
  $('#btn-burst').addEventListener('click', () => labSend(100));

  /* ---------------- traffic generator ---------------- */
  const MIX = [['valid', 'Valid G-PDU', 80], ['unknown', 'Unknown TEID', 8], ['malformed', 'Malformed', 8], ['echo', 'Echo Request', 4]];
  const ppsFromSlider = v => Math.round(10 * Math.pow(2000, v / 1000));
  const sliderFromPps = p => Math.round(1000 * Math.log(p / 10) / Math.log(2000));
  const mixInputs = {};

  function buildMix() {
    $('#tg-mix').replaceChildren(...MIX.map(([k, label, def]) => {
      const inp = el('input', { type: 'range', min: '0', max: '100', value: String(def), 'aria-label': label + ' share' });
      const out = el('output', { text: def + '%' });
      mixInputs[k] = { inp, out };
      inp.addEventListener('input', () => { updateMixLabels(); pushTraffic(); });
      return el('div', { class: 'mix-row' }, el('span', { text: label }), inp, out);
    }));
    updateMixLabels();
  }
  function mixValues() { return Object.fromEntries(MIX.map(([k]) => [k, Number(mixInputs[k].inp.value)])); }
  function updateMixLabels() {
    const v = mixValues(), sum = Object.values(v).reduce((a, b) => a + b, 0) || 1;
    MIX.forEach(([k]) => { mixInputs[k].out.textContent = Math.round(v[k] / sum * 100) + '%'; });
  }
  function updatePpsLabel() { $('#tg-pps-out').textContent = fmtInt(ppsFromSlider(Number($('#tg-pps').value))) + ' pkt/s'; }
  $('#tg-pps').addEventListener('input', () => { updatePpsLabel(); pushTraffic(); });

  let pushTimer = null;
  function pushTraffic() {                  // live-update only while running
    if (!latest.traffic || !latest.traffic.running) return;
    clearTimeout(pushTimer);
    pushTimer = setTimeout(() => startTraffic(true), 250);
  }
  async function startTraffic(quiet) {
    const mix = mixValues();
    if (!Object.values(mix).some(v => v > 0)) { toast('Give at least one packet kind a share', true); return; }
    try { await api('POST', '/api/traffic', { pps: ppsFromSlider(Number($('#tg-pps').value)), mix }); if (!quiet) toast('Traffic started'); }
    catch (e) { toast(e.message, true); }
  }
  $('#tg-start').addEventListener('click', () => startTraffic(false));
  $('#tg-stop').addEventListener('click', async () => {
    try { await api('DELETE', '/api/traffic'); toast('Traffic stopped'); } catch (e) { toast(e.message, true); }
  });
  function syncTrafficControls(t) {
    if (t.running) $('#tg-pps').value = sliderFromPps(Math.max(10, t.pps));
    MIX.forEach(([k]) => { if (t.mix && k in t.mix) mixInputs[k].inp.value = t.mix[k]; });
    updatePpsLabel(); updateMixLabels();
  }
  function renderTrafficStatus(s) {
    const t = s.traffic || {};
    const pill = $('#tg-state');
    pill.className = 'pill ' + (t.running ? 'pill-on' : 'pill-off');
    pill.lastChild.textContent = t.running ? 'running' : 'idle';
    $('#tg-sent').textContent = t.running ? `${fmtInt(t.sent)} packets sent in ${fmtUptime(t.elapsed_s)}` : 'Generator is stopped.';
  }

  /* ---------------- hero animation ---------------- */
  const SVGNS = 'http://www.w3.org/2000/svg';
  const reduceMotion = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const dotsG = $('#dots'), burstG = $('#bursts');
  const live = [];                          // active dots

  function spawn(x0, y0, x1, y1, dur, cls, onDone) {
    const c = document.createElementNS(SVGNS, 'circle');
    c.setAttribute('r', '5'); c.setAttribute('class', cls);
    dotsG.append(c);
    live.push({ c, x0, y0, x1, y1, dur, t0: performance.now(), onDone });
  }
  function burst(x, y) {
    const r = document.createElementNS(SVGNS, 'circle');
    r.setAttribute('cx', x); r.setAttribute('cy', y); r.setAttribute('r', '4');
    r.setAttribute('fill', 'none'); r.setAttribute('stroke', css('--c-drop')); r.setAttribute('stroke-width', '2');
    burstG.append(r);
    const t0 = performance.now();
    (function step(now) {
      const k = (now - t0) / 500;
      if (k >= 1) { r.remove(); return; }
      r.setAttribute('r', String(4 + k * 18)); r.setAttribute('opacity', String(1 - k));
      requestAnimationFrame(step);
    })(t0);
  }
  function frame(now) {
    for (let i = live.length - 1; i >= 0; i--) {
      const d = live[i], k = Math.min(1, (now - d.t0) / d.dur);
      d.c.setAttribute('cx', d.x0 + (d.x1 - d.x0) * k); d.c.setAttribute('cy', d.y0 + (d.y1 - d.y0) * k);
      if (k >= 1) { d.c.remove(); live.splice(i, 1); if (d.onDone) d.onDone(); }
    }
    requestAnimationFrame(frame);
  }
  function ingress() {
    spawn(86, 175, 318, 175, 1100, 'dot-in', () => {
      const rx = latest.pps_rx || 0, dropShare = rx ? (latest.pps_drop || 0) / rx : 0;
      if (Math.random() < dropShare) {
        spawn(380, 227, 380, 290, 700, 'dot-drop', () => burst(380, 290));
      } else {
        spawn(442, 175, 656, 175, 1100, 'dot-ok');
      }
    });
  }
  function startHero() {
    if (reduceMotion) return;
    requestAnimationFrame(frame);
    (function tick() {
      const pps = latest.online ? latest.pps_rx || 0 : 0;
      const rate = pps ? Math.min(14, 1 + Math.log10(pps + 1) * 3) : 0;      // dots per second
      if (rate && document.visibilityState === 'visible' && live.length < 60) ingress();
      setTimeout(tick, rate ? 1000 / rate : 400);
    })();
  }

  /* ---------------- init ---------------- */
  $('#drop-lbl').setAttribute('y', '312');
  buildPresets(); buildMix(); updatePpsLabel();
  $('#tg-pps').value = sliderFromPps(400); updatePpsLabel();
  selectPreset('valid');
  startHero();
  boot();
})();
