/* packet.js - GTP-U packet builder with per-field annotations (3GPP TS 29.281).
 * Pure functions, no DOM: loaded by the browser and by `node --test`. */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.GtpuPacket = factory();
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const MSG_TYPES = {
    1: 'Echo Request', 2: 'Echo Response', 26: 'Error Indication',
    31: 'Supported Extension Headers Notification', 254: 'End Marker', 255: 'G-PDU',
  };

  const DEFAULTS = Object.freeze({
    version: 1, pt: 1, e: false, s: false, pn: false,
    msgType: 255, teid: 1000, seq: 1, npdu: 0,
    ext: 'none',            // none | valid | zero  (needs the E flag, set automatically)
    payloadLen: 32, lengthDelta: 0, truncateTo: null,
  });

  function u32(v) { return [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255]; }

  /* Returns { bytes: number[], fields: [{name, start, end, cls, desc}] } (end exclusive). */
  function build(opts) {
    const o = Object.assign({}, DEFAULTS, opts);
    const ext = o.ext !== 'none';
    const e = o.e || ext;
    const hasOpt = e || o.s || o.pn;
    const bytes = [], fields = [];
    const field = (name, cls, desc, data) => {
      fields.push({ name, cls, desc, start: bytes.length, end: bytes.length + data.length });
      bytes.push(...data);
    };

    const flags = ((o.version & 7) << 5) | ((o.pt & 1) << 4) | (e ? 4 : 0) | (o.s ? 2 : 0) | (o.pn ? 1 : 0);
    // Length is patched once the body is known.
    field('Flags', 'f-flags',
      `version ${o.version}, PT ${o.pt}, E=${+e} S=${+o.s} PN=${+o.pn}. Version must be 1 and PT must be 1.`, [flags]);
    field('Message type', 'f-type', `${o.msgType} = ${MSG_TYPES[o.msgType] || 'unknown'}`, [o.msgType & 255]);
    field('Length', 'f-len', 'Bytes after the first 8. Must equal the real datagram size minus 8.', [0, 0]);
    field('TEID', 'f-teid', `Tunnel endpoint ID ${o.teid >>> 0}`, u32(o.teid >>> 0));

    if (hasOpt) {
      field('Sequence', 'f-opt', `Sequence number ${o.seq & 0xffff} (valid only if S=1)`, [(o.seq >> 8) & 255, o.seq & 255]);
      field('N-PDU', 'f-opt', 'N-PDU number (valid only if PN=1)', [o.npdu & 255]);
      field('Next ext', 'f-opt', ext ? 'Next extension header type 0x85 (PDU Session Container)' : 'No extension header follows',
        [ext ? 0x85 : 0]);
    }
    if (ext) {
      if (o.ext === 'zero') {
        field('Ext header', 'f-ext', 'Declares length 0: illegal, and would loop forever if trusted.', [0, 0, 0, 0]);
      } else {
        field('Ext header', 'f-ext', 'One 4-byte extension: length=1 unit, 2 content bytes, next=0 (end of chain).', [1, 0x09, 0x00, 0]);
      }
    }
    const payload = Array.from({ length: Math.max(0, o.payloadLen | 0) }, (_, i) => (0x41 + (i % 26)));
    if (payload.length) field('Payload', 'f-pay', `${payload.length} bytes of inner packet (never inspected)`, payload);

    const len = (bytes.length - 8 + (o.lengthDelta | 0)) & 0xffff;
    bytes[2] = len >> 8; bytes[3] = len & 255;

    if (o.truncateTo !== null && o.truncateTo < bytes.length) {
      bytes.length = Math.max(0, o.truncateTo);
      for (let i = fields.length - 1; i >= 0; i--) {
        if (fields[i].start >= bytes.length) fields.splice(i, 1);
        else if (fields[i].end > bytes.length) fields[i].end = bytes.length;
      }
    }
    return { bytes, fields };
  }

  function toHex(bytes, sep) {
    return bytes.map(function (b) { return (b < 16 ? '0' : '') + b.toString(16); }).join(sep || '');
  }

  function fromHex(str) {
    const clean = String(str).replace(/\s+/g, '');
    if (clean.length % 2 || /[^0-9a-f]/i.test(clean)) return null;
    const out = [];
    for (let i = 0; i < clean.length; i += 2) out.push(parseInt(clean.substr(i, 2), 16));
    return out;
  }

  /* One-click scenarios, each with the verdict the forwarder should reach. */
  const PRESETS = [
    { id: 'valid', label: 'Valid G-PDU', expect: 'forward', opts: { teid: 1000 },
      blurb: 'A well-formed user-plane packet for a routed TEID.' },
    { id: 'seq', label: 'With sequence number', expect: 'forward', opts: { teid: 1001, s: true, seq: 4242 },
      blurb: 'S flag set, so 4 optional header bytes carry a sequence number.' },
    { id: 'ext', label: 'Extension header', expect: 'forward', opts: { teid: 1002, ext: 'valid' },
      blurb: 'E flag set with a PDU Session Container; the parser walks the chain.' },
    { id: 'echo', label: 'Echo Request', expect: 'echo_response', opts: { msgType: 1, teid: 0, s: true, seq: 7, payloadLen: 0 },
      blurb: 'Path-management keepalive. The forwarder answers with an Echo Response.' },
    { id: 'unknown', label: 'Unknown TEID', expect: 'drop', opts: { teid: 0xdead },
      blurb: 'Valid packet, but no route exists for this tunnel.' },
    { id: 'version', label: 'Wrong version', expect: 'drop', opts: { version: 2 },
      blurb: 'Version field is 2. Only GTP version 1 is user-plane.' },
    { id: 'pt', label: 'GTP′ (PT=0)', expect: 'drop', opts: { pt: 0 },
      blurb: 'Protocol-type bit cleared marks the charging variant GTP′.' },
    { id: 'length', label: 'Length mismatch', expect: 'drop', opts: { lengthDelta: 40 },
      blurb: 'Length field claims 40 more bytes than the datagram has.' },
    { id: 'short', label: 'Truncated', expect: 'drop', opts: { truncateTo: 5 },
      blurb: 'Datagram ends inside the mandatory 8-byte header.' },
    { id: 'zeroext', label: 'Zero-length extension', expect: 'drop', opts: { ext: 'zero' },
      blurb: 'A hostile extension header that would never advance the parser.' },
    { id: 'type26', label: 'Error Indication', expect: 'drop', opts: { msgType: 26 },
      blurb: 'A message type this forwarder does not handle.' },
  ];

  return { build, toHex, fromHex, PRESETS, DEFAULTS, MSG_TYPES };
});
