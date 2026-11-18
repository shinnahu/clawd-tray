#!/usr/bin/env node
/*
 * Builds src/frames.h and src/clawd.ico from the Clawd art that ships inside YOUR installed
 * Claude desktop app. The art is not part of this repository; this script reads it locally.
 *
 *   node tools/make-frames.js [path\to\ion-dist.asar]
 *
 * Without an argument it looks for the Microsoft Store install (Get-AppxPackage Claude) and the
 * classic install under %LOCALAPPDATA%\AnthropicClaude.
 *
 * Pipeline: asar archive -> "Clawd-Laptop" Lottie (pixel art drawn with rectangles on a 50-unit grid)
 * -> per-frame cell grids -> mirrored like the app shows it -> resampled at 2/3 so Clawd fills a
 * 16x16 tray icon -> emote frames (check, ?, !) -> C header with a 4-bit palette.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { execFileSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const OUT_H = path.join(ROOT, 'src', 'frames.h');
const OUT_ICO = path.join(ROOT, 'src', 'clawd.ico');

// ------------------------------------------------------------------ find the app archive
function findAsar() {
  const arg = process.argv[2];
  if (arg) return arg;
  const candidates = [];
  try {
    const loc = execFileSync('powershell', ['-NoProfile', '-Command', '(Get-AppxPackage -Name Claude).InstallLocation'],
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim().split(/\r?\n/).filter(Boolean);
    for (const l of loc) candidates.push(path.join(l, 'app', 'resources', 'ion-dist.asar'));
  } catch (_) { /* not a Store install */ }
  const la = process.env.LOCALAPPDATA;
  if (la) {
    const base = path.join(la, 'AnthropicClaude');
    try {
      for (const d of fs.readdirSync(base).filter(n => n.startsWith('app-')).sort().reverse())
        candidates.push(path.join(base, d, 'resources', 'ion-dist.asar'));
    } catch (_) { /* not a classic install */ }
  }
  const hit = candidates.find(p => fs.existsSync(p));
  if (!hit) {
    console.error('Could not find the Claude desktop app (ion-dist.asar). Pass its path as an argument.');
    process.exit(1);
  }
  return hit;
}

// ------------------------------------------------------------------ asar reader
function* asarFiles(file) {
  const fd = fs.openSync(file, 'r');
  const head = Buffer.alloc(16);
  fs.readSync(fd, head, 0, 16, 0);
  const headerSize = head.readUInt32LE(4), jsonLen = head.readUInt32LE(12);
  const jb = Buffer.alloc(jsonLen);
  fs.readSync(fd, jb, 0, jsonLen, 16);
  const header = JSON.parse(jb.toString());
  const base = 8 + headerSize;
  const stack = [[header, '']];
  while (stack.length) {
    const [node, prefix] = stack.pop();
    for (const [name, v] of Object.entries(node.files || {})) {
      const p = prefix + '/' + name;
      if (v.files) { stack.push([v, p]); continue; }
      if (v.unpacked) continue;
      yield { path: p, read: () => { const b = Buffer.alloc(v.size); fs.readSync(fd, b, 0, v.size, base + Number(v.offset)); return b; } };
    }
  }
}
function findLottie(asar, name) {
  const BS = String.fromCharCode(92);
  for (const f of asarFiles(asar)) {
    if (!/\.(js|json)$/.test(f.path)) continue;
    const s = f.read().toString('latin1');
    if (!s.includes('"nm":"' + name + '"')) continue;
    let i = 0;
    while ((i = s.indexOf('{"v":"', i)) >= 0) {
      let d = 0, j = i, inStr = false;
      for (; j < s.length; j++) {
        const c = s[j];
        if (inStr) { if (c === BS) j++; else if (c === '"') inStr = false; continue; }
        if (c === '"') inStr = true; else if (c === '{') d++; else if (c === '}' && --d === 0) break;
      }
      const text = s.slice(i, j + 1);
      try { const json = JSON.parse(text); if (json.nm === name) return json; } catch (_) { /* not it */ }
      i = j + 1;
    }
  }
  return null;
}

// ------------------------------------------------------------------ Lottie -> cell grids
const valAt = (p, t) => {
  if (!p) return null;
  if (p.a !== 1) return p.k;
  let cur = p.k[0].s;
  for (const k of p.k) if (k.t <= t) cur = k.s;
  return cur;
};
const LAYER_CHAR = { black: 'K', orange: 'O', dark_orange: 'o', color_3: 'L' };
function rasterize(j) {
  const frames = [];
  for (let t = j.ip; t < j.op; t++) {
    const cells = new Map();
    for (let li = j.layers.length - 1; li >= 0; li--) {         // bottom layer first
      const L = j.layers[li];
      if (t < L.ip || t >= L.op) continue;
      const shapes = L.shapes || [];
      for (let gi = shapes.length - 1; gi >= 0; gi--) {
        const g = shapes[gi];
        if (g.ty !== 'gr') continue;
        const tr = g.it.find(x => x.ty === 'tr'), fl = g.it.find(x => x.ty === 'fl');
        const o1 = tr ? valAt(tr.o, t) : 100, o2 = fl.o ? valAt(fl.o, t) : 100;
        if (!((Array.isArray(o1) ? o1[0] : o1) * (Array.isArray(o2) ? o2[0] : o2) > 0)) continue;
        const P = (tr && valAt(tr.p, t)) || [0, 0], A = (tr && valAt(tr.a, t)) || [0, 0], S = (tr && valAt(tr.s, t)) || [100, 100];
        for (const r of g.it) {
          if (r.ty !== 'rc') continue;
          const p = valAt(r.p, t), s = valAt(r.s, t);
          const x0 = P[0] + (p[0] - s[0] / 2 - A[0]) * S[0] / 100, y0 = P[1] + (p[1] - s[1] / 2 - A[1]) * S[1] / 100;
          const w = s[0] * S[0] / 100, h = s[1] * S[1] / 100;
          for (let yy = 0; yy < Math.round(h / 50); yy++)
            for (let xx = 0; xx < Math.round(w / 50); xx++) {
              let cx = Math.round((x0 - 36) / 50) + xx;
              const cy = Math.round((y0 - 1) / 50) + yy;
              cx = -cx - 1;                                       // mirrored, as the app displays it
              cells.set(cx + ',' + cy, LAYER_CHAR[L.nm]);
            }
        }
      }
    }
    frames.push(cells);
  }
  return frames;
}

// ------------------------------------------------------------------ 2/3 resample into 16x16
const Y0 = 15;
const at = i => Math.floor(1.5 * i + 0.5);
function camera(t) {                       // which 50-grid cell lands on column 0, and laptop nudge
  if (t <= 8 || t >= 38) return { X0: -38, lx: 0 };
  if (t <= 11) return { X0: -40, lx: 3 };
  if (t <= 14) return { X0: -42, lx: 5 };
  if (t <= 35) return { X0: -43, lx: 5 };
  if (t === 36) return { X0: -41, lx: 3 };
  return { X0: -39, lx: 0 };
}
function resample(grid, { X0, lx }) {
  const c = Array.from({ length: 16 }, () => Array(16).fill('.'));
  for (let j = 0; j < 16; j++) for (let i = 0; i < 16; i++) {
    const v = grid.get((X0 + at(i)) + ',' + (Y0 + at(j)));
    if (v && v !== 'L') { c[j][i] = v; continue; }
    let hit = false;                        // thin laptop lines: area test so they survive
    for (let dy = 0; dy < 2 && !hit; dy++) for (let dx = 0; dx < 2 && !hit; dx++)
      if (grid.get((X0 + Math.floor(1.5 * i) + dx - lx) + ',' + (Y0 + Math.floor(1.5 * j) + dy)) === 'L') hit = true;
    if (hit) c[j][i] = 'L';
  }
  return c;
}
const LAPTOP = [[0, 9], [0, 10], [0, 11], [0, 12], [0, 13], [0, 14], [1, 14], [2, 14], [3, 14], [4, 14]];
const oneWideEyes = f => { for (let y = 0; y < 16; y++) for (let x = 1; x < 16; x++) if (f[y][x] === 'K' && f[y][x - 1] === 'K') f[y][x] = 'O'; return f; };
const withLaptop = f => { for (const [x, y] of LAPTOP) if (f[y][x] === '.') f[y][x] = 'L'; return f; };
function tray16(grids, t) {
  const g = grids[t];
  if (t >= 15 && t <= 32) return withLaptop(oneWideEyes(resample(g, { X0: -42, lx: 99 })));   // typing: mascot + mini laptop
  if (t >= 33 && t <= 35) return oneWideEyes(resample(g, { X0: -42, lx: 4 }));
  if (t >= 12 && t <= 14) return withLaptop(resample(g, { X0: -40, lx: 99 }));
  return resample(g, camera(t));
}

// ------------------------------------------------------------------ emotes, frame table, sequences
const clone = f => f.map(r => r.slice());
function blink(f) {
  const g = clone(f);
  for (let y = 0; y < 15; y++) for (let x = 0; x < 16; x++)
    if (f[y][x] === 'K' && f[y + 1][x] === 'K') { g[y][x] = 'O'; g[y + 1][x] = 'o'; }
  return g;
}
const EMOTE = {
  check: ['....G', '...GG', 'G.GG.', 'GGG..', '.G...'],
  ask: ['.YY.', 'Y..Y', '..Y.', '....', '..Y.'],
  bang: ['RR', 'RR', 'RR', '..', 'RR'],
};
function stamp(f, glyph, x0, y0) {
  const g = clone(f);
  glyph.forEach((row, j) => [...row].forEach((v, i) => { if (v !== '.' && y0 + j >= 0 && y0 + j < 16) g[y0 + j][x0 + i] = v; }));
  return g;
}

function build(lottie) {
  const grids = rasterize(lottie);
  if (grids.length < 43) throw new Error('Unexpected Clawd-Laptop animation (only ' + grids.length + ' frames)');
  const F = [], idx = {};
  const add = (name, f) => { idx[name] = F.length; F.push([name, f]); };
  for (let t = 0; t <= 42; t++) add('L' + t, tray16(grids, t));
  const front = F[idx.L0][1];
  add('blink', blink(front));
  add('done', stamp(front, EMOTE.check, 11, 0)); add('done_blink', stamp(blink(front), EMOTE.check, 11, 0)); add('done_pop', stamp(front, EMOTE.check, 11, 1));
  add('ask', stamp(front, EMOTE.ask, 12, 0)); add('ask_up', stamp(front, EMOTE.ask, 12, -1)); add('ask_blink', stamp(blink(front), EMOTE.ask, 12, 0));
  add('err', stamp(front, EMOTE.bang, 14, 0)); add('err_blink', stamp(blink(front), EMOTE.bang, 14, 0));
  const seq = (a, b, ms) => { const r = []; for (let t = a; t <= b; t++) r.push([idx['L' + t], ms]); return r; };
  const SEQ = { TAKE_OUT: seq(0, 16, 83), TYPE: seq(17, 32, 95), PUT_AWAY: seq(33, 42, 83) };

  const PAL = { '.': 0, O: 1, o: 2, K: 3, L: 4, G: 5, Y: 6, R: 7 };
  let h = '// Generated from the Claude desktop app\'s own Clawd-Laptop animation (+ emotes). 16x16, 4-bit palette.\n';
  h += 'static const unsigned int PALETTE[8] = {0x00000000,0xFFD97656,0xFFBE684D,0xFF000000,0xFFB4B4B4,0xFF2ECC60,0xFFFFBE28,0xFFF04646};\n';
  h += '#define NFRAMES ' + F.length + '\nstatic const unsigned char FRAMES[NFRAMES][128] = {\n';
  for (const [n, f] of F) {
    const bytes = [];
    for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x += 2) bytes.push((PAL[f[y][x]] << 4) | PAL[f[y][x + 1]]);
    h += ' {' + bytes.join(',') + '}, // ' + n + '\n';
  }
  h += '};\n';
  for (const [n, i] of Object.entries(idx)) if (!/^L\d+$/.test(n)) h += '#define F_' + n.toUpperCase() + ' ' + i + '\n';
  h += '#define F_FRONT ' + idx.L0 + '\n';
  for (const [k, v] of Object.entries(SEQ)) h += 'static const unsigned short SEQ_' + k + '[][2] = {' + v.map(([a, b]) => '{' + a + ',' + b + '}').join(',') + '};\n';
  return { header: h, front };
}

// ------------------------------------------------------------------ .ico (PNG entries)
const CRC = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c; });
const crc32 = b => { let c = -1; for (const x of b) c = CRC[(c ^ x) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
const chunk = (type, data) => {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type), data]);
  const c = Buffer.alloc(4); c.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, c]);
};
function png(w, h, rgba) {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) Buffer.from(rgba.buffer, y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  const ih = Buffer.alloc(13); ih.writeUInt32BE(w, 0); ih.writeUInt32BE(h, 4); ih[8] = 8; ih[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
function ico(front) {
  const RGB = { O: [217, 118, 86], o: [190, 104, 77], K: [0, 0, 0], L: [180, 180, 180] };
  const imgs = [16, 32, 48, 256].map(S => {
    const a = new Uint8Array(S * S * 4);
    for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) {
      const c = RGB[front[Math.floor(y * 16 / S)][Math.floor(x * 16 / S)]];
      if (c) { const p = (y * S + x) * 4; a[p] = c[0]; a[p + 1] = c[1]; a[p + 2] = c[2]; a[p + 3] = 255; }
    }
    return [S, png(S, S, a)];
  });
  const hdr = Buffer.alloc(6); hdr.writeUInt16LE(1, 2); hdr.writeUInt16LE(imgs.length, 4);
  let off = 6 + 16 * imgs.length; const dir = [];
  for (const [S, p] of imgs) {
    const d = Buffer.alloc(16); d[0] = S >= 256 ? 0 : S; d[1] = S >= 256 ? 0 : S;
    d.writeUInt16LE(1, 4); d.writeUInt16LE(32, 6); d.writeUInt32LE(p.length, 8); d.writeUInt32LE(off, 12);
    off += p.length; dir.push(d);
  }
  return Buffer.concat([hdr, ...dir, ...imgs.map(i => i[1])]);
}

// ------------------------------------------------------------------ main
const asar = findAsar();
console.log('Reading Clawd art from', asar);
const lottie = findLottie(asar, 'Clawd-Laptop');
if (!lottie) { console.error('The "Clawd-Laptop" animation was not found in this version of the app.'); process.exit(1); }
const { header, front } = build(lottie);
fs.writeFileSync(OUT_H, header);
fs.writeFileSync(OUT_ICO, ico(front));
console.log('Wrote', path.relative(ROOT, OUT_H), 'and', path.relative(ROOT, OUT_ICO));
