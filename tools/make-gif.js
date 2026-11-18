#!/usr/bin/env node
/*
 * Renders docs/demo.gif (the README preview) from src/frames.h, so run tools/make-frames.js first.
 * Uses the 24 px frames (the app's art at its own resolution), scaled up with hard pixel edges.
 *
 *   node tools/make-gif.js [scale]
 */
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const SCALE = Number(process.argv[2]) || 5;
const W = 24 * SCALE + 160, H = 24 * SCALE + 16;
const BG = [0x20, 0x20, 0x20];   // dark taskbar

// ------------------------------------------------------------------ frames from the generated header
const h = fs.readFileSync(path.join(ROOT, 'src', 'frames.h'), 'utf8');
const pal = h.match(/PALETTE\[8\] = \{([^}]*)\}/)[1].split(',').map(s => parseInt(s, 16));
const block = h.slice(h.indexOf('FRAMES24['), h.indexOf('};', h.indexOf('FRAMES24[')));
const frames = {};
for (const line of block.split('\n')) {
  const m = line.match(/\{([\d,]+)\}, \/\/ (\S+)/);
  if (m) frames[m[2]] = m[1].split(',').map(Number);
}

// ------------------------------------------------------------------ the story: same timings as the app
const story = [];
const hold = (name, ms) => story.push([name, ms]);
const run = (a, b, ms) => { for (let t = a; t <= b; t++) hold('L' + t, ms); };
hold('L0', 1400); hold('blink', 120); hold('L0', 900); hold('blink', 120); hold('L0', 140); hold('blink', 120); hold('L0', 700);
run(0, 16, 83);                                // crouch, pull the laptop out
for (let i = 0; i < 2; i++) run(17, 32, 95);   // type
run(33, 42, 83);                               // put it away
hold('done_pop', 140); hold('done', 1300); hold('done_blink', 120); hold('done', 900);
hold('L0', 500);
for (let i = 0; i < 3; i++) { hold('ask', 600); hold('ask_up', 600); }
hold('ask_blink', 120); hold('ask', 500);
hold('L0', 400); hold('err', 1200); hold('err_blink', 120); hold('err', 700);

// ------------------------------------------------------------------ GIF89a encoder (16-colour, LZW)
const COLORS = [BG, ...pal.slice(1).map(c => [(c >> 16) & 255, (c >> 8) & 255, c & 255])];
while (COLORS.length < 16) COLORS.push([0, 0, 0]);

function render(name) {
  const f = frames[name];
  if (!f) throw new Error('missing frame ' + name);
  const px = new Uint8Array(W * H);   // index 0 = background
  const ox = (W - 24 * SCALE) >> 1, oy = (H - 24 * SCALE) >> 1;
  for (let y = 0; y < 24; y++) for (let x = 0; x < 24; x++) {
    const b = f[y * 12 + (x >> 1)], idx = (x & 1) ? (b & 15) : (b >> 4);
    if (!idx) continue;
    for (let a = 0; a < SCALE; a++) px.fill(idx, (oy + y * SCALE + a) * W + ox + x * SCALE, (oy + y * SCALE + a) * W + ox + x * SCALE + SCALE);
  }
  return px;
}
function lzw(indices, minCode) {
  const clear = 1 << minCode, eoi = clear + 1, out = [];
  let size = minCode + 1, next = eoi + 1, table = new Map(), cur = 0, shift = 0;
  const emit = c => { cur |= c << shift; shift += size; while (shift >= 8) { out.push(cur & 255); cur >>>= 8; shift -= 8; } };
  emit(clear);
  let prefix = indices[0];
  for (let i = 1; i < indices.length; i++) {
    const k = indices[i], key = (prefix << 8) | k, code = table.get(key);
    if (code !== undefined) { prefix = code; continue; }
    emit(prefix);
    if (next === 4096) { emit(clear); next = eoi + 1; size = minCode + 1; table = new Map(); }
    else { if (next >= (1 << size)) size++; table.set(key, next++); }
    prefix = k;
  }
  emit(prefix); emit(eoi);
  if (shift > 0) out.push(cur & 255);
  return Buffer.from(out);
}
const u16 = n => Buffer.from([n & 255, (n >> 8) & 255]);
function subBlocks(data) {
  const parts = [];
  for (let i = 0; i < data.length; i += 255) { const c = data.subarray(i, i + 255); parts.push(Buffer.from([c.length]), c); }
  parts.push(Buffer.from([0]));
  return Buffer.concat(parts);
}

// merge consecutive identical frames
const merged = [];
for (const [name, ms] of story) {
  const last = merged[merged.length - 1];
  if (last && last[0] === name) last[1] += ms; else merged.push([name, ms]);
}
const parts = [
  Buffer.from('GIF89a'), u16(W), u16(H), Buffer.from([0xF3, 0, 0]),
  Buffer.from(COLORS.flat()),
  Buffer.from([0x21, 0xFF, 0x0B]), Buffer.from('NETSCAPE2.0'), Buffer.from([3, 1, 0, 0, 0]),   // loop forever
];
for (const [name, ms] of merged) {
  parts.push(Buffer.from([0x21, 0xF9, 4, 0x04]), u16(Math.max(2, Math.round(ms / 10))), Buffer.from([0, 0]));
  parts.push(Buffer.from([0x2C]), u16(0), u16(0), u16(W), u16(H), Buffer.from([0]));
  parts.push(Buffer.from([4]), subBlocks(lzw(render(name), 4)));
}
parts.push(Buffer.from([0x3B]));
const out = path.join(ROOT, 'docs', 'demo.gif');
fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, Buffer.concat(parts));
console.log('Wrote', path.relative(ROOT, out), W + 'x' + H, merged.length, 'frames', (fs.statSync(out).size / 1024).toFixed(1) + ' KB');
