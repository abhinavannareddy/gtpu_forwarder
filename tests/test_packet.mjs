// Unit tests for the dashboard's packet builder: node --test tests/test_packet.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const P = require('../dashboard/static/packet.js');

test('valid G-PDU matches the byte layout used by the C unit tests', () => {
  const { bytes } = P.build({ teid: 0x11223344, payloadLen: 20 });
  assert.equal(P.toHex(bytes.slice(0, 8)), '30ff001411223344');
  assert.equal(bytes.length, 28);
});

test('sequence number adds four optional bytes and counts them in the length', () => {
  const { bytes, fields } = P.build({ s: true, seq: 0xbeef, payloadLen: 3, teid: 1 });
  assert.equal(bytes[0], 0x32);
  assert.equal((bytes[2] << 8) | bytes[3], 4 + 3);
  assert.equal(P.toHex(bytes.slice(8, 12)), 'beef0000');
  assert.deepEqual(fields.map(f => f.name), ['Flags', 'Message type', 'Length', 'TEID', 'Sequence', 'N-PDU', 'Next ext', 'Payload']);
});

test('extension presets set the E flag and chain correctly', () => {
  const ok = P.build({ ext: 'valid', payloadLen: 2 }).bytes;
  assert.equal(ok[0] & 4, 4);
  assert.equal(ok[11], 0x85);
  assert.equal(ok[12], 1);
  const bad = P.build({ ext: 'zero', payloadLen: 2 }).bytes;
  assert.equal(bad[12], 0);
});

test('malformed presets produce the intended defect', () => {
  assert.equal(P.build({ version: 2 }).bytes[0] >> 5, 2);
  assert.equal(P.build({ pt: 0 }).bytes[0] & 0x10, 0);
  const l = P.build({ lengthDelta: 40, payloadLen: 10 }).bytes;
  assert.equal((l[2] << 8) | l[3], 50);
  const t = P.build({ truncateTo: 5 });
  assert.equal(t.bytes.length, 5);
  assert.equal(t.fields.at(-1).end, 5);
  assert.ok(t.fields.every(f => f.end <= 5));
});

test('hex round trip and validation', () => {
  assert.deepEqual(P.fromHex('30 ff 00'), [0x30, 0xff, 0]);
  assert.equal(P.fromHex('3'), null);
  assert.equal(P.fromHex('zz'), null);
  assert.equal(P.toHex([0, 15, 255], ' '), '00 0f ff');
});

test('every preset builds and fields tile the packet without gaps', () => {
  for (const p of P.PRESETS) {
    const { bytes, fields } = P.build(p.opts);
    let pos = 0;
    for (const f of fields) { assert.equal(f.start, pos, p.id); pos = f.end; }
    assert.equal(pos, bytes.length, p.id);
  }
});

function ones(bytes) {
  let s = 0;
  for (let i = 0; i < bytes.length; i += 2) s += (bytes[i] << 8) | bytes[i + 1];
  while (s >> 16) s = (s & 0xffff) + (s >> 16);
  return s;
}

test('inner IPv4 payload has a valid header checksum, and the bad variant does not', () => {
  const good = P.build({ payloadKind: 'ipv4', payloadLen: 60 });
  const inner = good.bytes.slice(8);
  assert.equal(inner.length, 60);
  assert.equal(inner[0], 0x45);
  assert.equal(ones(inner.slice(0, 20)), 0xffff);
  const bad = P.build({ payloadKind: 'ipv4-badsum', payloadLen: 60 }).bytes.slice(8);
  assert.notEqual(ones(bad.slice(0, 20)), 0xffff);
});

test('inner IPv6 payload has a consistent payload-length field', () => {
  const inner = P.build({ payloadKind: 'ipv6', payloadLen: 80 }).bytes.slice(8);
  assert.equal(inner[0] >> 4, 6);
  assert.equal(((inner[4] << 8) | inner[5]) + 40, inner.length);
});

test('tiny payload sizes are raised to a full inner header', () => {
  assert.equal(P.build({ payloadKind: 'ipv4', payloadLen: 3 }).bytes.length, 8 + 20);
  assert.equal(P.build({ payloadKind: 'ipv6', payloadLen: 3 }).bytes.length, 8 + 40);
});
