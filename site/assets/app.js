(() => {
'use strict';
document.documentElement.classList.add('js');
/* ---------- i18n core ---------- */
const LANGS = ['es', 'en', 'de', 'fr', 'pt', 'ja', 'it', 'ca'];
window.I18N = window.I18N || {};
let LANG = 'es';
const t = (k, ...a) => { const d = I18N[LANG], e = I18N.es; const s = d && d[k] != null ? d[k] : (e && e[k] != null ? e[k] : k); return s.replace(/\{(\d+)\}/g, (m, i) => (a[+i] != null ? a[+i] : '')); };
const tr = t;
const hooks = []; const onLang = f => hooks.push(f);
function langPref() {
  const q = ((location.search.match(/[?&]lang=([a-zA-Z]{2})/) || [])[1] || '').toLowerCase();
  if (LANGS.includes(q)) return q;
  let s = null; try { s = localStorage.getItem('l606lang'); } catch (e) { }
  if (LANGS.includes(s)) return s;
  const n = ((navigator.languages && navigator.languages[0]) || navigator.language || 'es').slice(0, 2).toLowerCase();
  return LANGS.includes(n) ? n : 'es';
}
function loadLang(l) {
  return new Promise(res => {
    if (l === 'es' || I18N[l]) return res();
    const s = document.createElement('script'); s.src = 'assets/i18n/' + l + '.js'; s.onload = res; s.onerror = res; document.head.appendChild(s);
  });
}
function applyDOM() {
  const d = document; d.documentElement.lang = LANG;
  d.querySelectorAll('[data-i18n]').forEach(e => { e.textContent = t(e.dataset.i18n); });
  d.querySelectorAll('[data-i18n-html]').forEach(e => { e.innerHTML = t(e.dataset.i18nHtml); });
  d.querySelectorAll('[data-i18n-attr]').forEach(e => e.dataset.i18nAttr.split('|').forEach(p => { const [a, k] = p.split(':'); e.setAttribute(a, t(k)); }));
  d.title = t('meta.title');
  const m = d.querySelector('meta[name=description]'); if (m) m.setAttribute('content', t('meta.desc'));
  const sel = d.getElementById('lang'); if (sel) sel.value = LANG;
}
function main() {
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
const NS = 'http://www.w3.org/2000/svg';
const TC = ['#3d7bff', '#19d3e6', '#ffd21f', '#ff4fd8', '#f4f4f4', '#ff8a1f'];
const C = { play: '#2ee86b', rec: '#ff3b3b', dub: '#a45cff', arm: '#ff9a1f', yel: '#ffd21f', yelD: '#6b5a12', cy: '#19d3e6', cyD: '#0e5963', roy: '#2a3a8c' };
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const lerp = (a, b, k) => a + (b - a) * k;
function mix(hex, a, base = '#2a2a2a') {
  const p = h => [1, 3, 5].map(i => parseInt(h.slice(i, i + 2), 16));
  const x = p(hex), y = p(base);
  return '#' + x.map((v, i) => Math.round(y[i] + (v - y[i]) * a).toString(16).padStart(2, '0')).join('');
}
const vdim = i => mix(TC[i], .22), dim = i => mix(TC[i], .45);

/* ---------- ticker: pure-function-of-time animations, paused offscreen ---------- */
const loops = [];
function reg(el, fn) {
  const o = { el, fn, vis: true };
  loops.push(o);
  if ('IntersectionObserver' in window) new IntersectionObserver(e => { o.vis = e[0].isIntersecting; }, { rootMargin: '80px' }).observe(el);
  if (reduce) fn(0.001, 0);
  return o;
}
let last = 0;
function frame(ts) {
  const t = ts / 1000, dt = Math.min(.1, t - last); last = t;
  for (const o of loops) if (o.vis) { try { o.fn(t, dt); } catch (e) { console.error(e); } }
  requestAnimationFrame(frame);
}
if (!reduce) requestAnimationFrame(frame);

/* ---------- 3x5 pixel font + OLED screen ---------- */
// Rebuild glyphs from explicit rows for correctness
const ROWS = {
A:['.#.','#.#','###','#.#','#.#'],B:['##.','#.#','##.','#.#','##.'],C:['.##','#..','#..','#..','.##'],D:['##.','#.#','#.#','#.#','##.'],
E:['###','#..','##.','#..','###'],F:['###','#..','##.','#..','#..'],G:['.##','#..','#.#','#.#','.##'],H:['#.#','#.#','###','#.#','#.#'],
I:['###','.#.','.#.','.#.','###'],J:['..#','..#','..#','#.#','.#.'],K:['#.#','#.#','##.','#.#','#.#'],L:['#..','#..','#..','#..','###'],
M:['#.#','###','###','#.#','#.#'],N:['##.','#.#','#.#','#.#','#.#'],O:['.#.','#.#','#.#','#.#','.#.'],P:['##.','#.#','##.','#..','#..'],
Q:['.#.','#.#','#.#','##.','.##'],R:['##.','#.#','##.','#.#','#.#'],S:['.##','#..','.#.','..#','##.'],T:['###','.#.','.#.','.#.','.#.'],
U:['#.#','#.#','#.#','#.#','###'],V:['#.#','#.#','#.#','#.#','.#.'],W:['#.#','#.#','###','###','#.#'],X:['#.#','#.#','.#.','#.#','#.#'],
Y:['#.#','#.#','.#.','.#.','.#.'],Z:['###','..#','.#.','#..','###'],
0:['###','#.#','#.#','#.#','###'],1:['.#.','##.','.#.','.#.','###'],2:['##.','..#','.#.','#..','###'],3:['##.','..#','.#.','..#','##.'],
4:['#.#','#.#','###','..#','..#'],5:['###','#..','##.','..#','##.'],6:['.##','#..','###','#.#','###'],7:['###','..#','.#.','.#.','.#.'],
8:['###','#.#','###','#.#','###'],9:['###','#.#','###','..#','##.'],
'.':['...','...','...','...','.#.'],':':['...','.#.','...','.#.','...'],'-':['...','...','###','...','...'],'+':['...','.#.','###','.#.','...'],
'/':['..#','..#','.#.','#..','#..'],'>':['#..','.#.','..#','.#.','#..'],'<':['..#','.#.','#..','.#.','..#'],',':['...','...','...','.#.','#..'],
'!':['.#.','.#.','.#.','...','.#.'],'%':['#.#','..#','.#.','#..','#.#'],'*':['...','#.#','.#.','#.#','...'],' ':['...','...','...','...','...'],
};
class Screen {
  constructor(canvas) { this.cv = canvas; canvas.width = 128; canvas.height = 64; this.c = canvas.getContext('2d'); this.fg = '#f0eee0'; this.dm = '#6d6c64'; }
  clear() { this.c.fillStyle = '#000'; this.c.fillRect(0, 0, 128, 64); }
  px(x, y, col) { this.c.fillStyle = col || this.fg; this.c.fillRect(x | 0, y | 0, 1, 1); }
  rect(x, y, w, h, col) { this.c.fillStyle = col || this.fg; this.c.fillRect(x, y, w, h); }
  vline(x, y0, y1, col) { this.rect(x | 0, y0, 1, y1 - y0 + 1, col); }
  text(s, x, y, align = 'l', col, inv) {
    s = String(s).toUpperCase(); const w = s.length * 4 - 1;
    if (align === 'r') x -= w; else if (align === 'c') x -= Math.floor(w / 2);
    if (inv) { this.rect(x - 2, y - 1, w + 4, 7, col || this.fg); col = '#000'; }
    for (const ch of s) { const g = ROWS[ch] || ROWS[' ']; g.forEach((r, j) => { for (let i = 0; i < 3; i++) if (r[i] === '#') this.px(x + i, y + j, col); }); x += 4; }
  }
  dial(cx, cy, r, v, col) {
    for (let a = 135; a <= 405; a += 4) { const q = a * Math.PI / 180; this.px(Math.round(cx + Math.cos(q) * r), Math.round(cy + Math.sin(q) * r), col); }
    const q = (135 + 270 * clamp(v, 0, 1)) * Math.PI / 180;
    for (let k = 1; k < r - 1; k += .5) this.px(Math.round(cx + Math.cos(q) * k), Math.round(cy + Math.sin(q) * k), col);
  }
  box(cx, y, w, h, txt) { this.c.strokeStyle = this.fg; this.c.lineWidth = 1; this.c.strokeRect(cx - w / 2 + .5, y + .5, w - 1, h - 1); this.text(txt, cx, y + Math.floor((h - 5) / 2), 'c'); }
}

/* ---------- waveform model ---------- */
const hash = n => { n = Math.sin(n * 127.1 + 311.7) * 43758.5453; return n - Math.floor(n); };
function amp(bar, seed, dubbed) {
  const s16 = Math.floor(bar * 16), bp = (bar * 4) % 1, env = Math.exp(-bp * 3.2);
  let a = .16 + .8 * env * (.4 + .6 * hash(s16 + seed * 13));
  if (hash(s16 * 3.1 + seed) > .72) a *= .55;
  if (dubbed) a = Math.min(1, a + .28 * hash(s16 * 7 + seed + 5));
  return Math.min(1, a);
}
function drawWave(sc, o) {
  sc.clear();
  if (o.head) sc.text(o.head, 0, 0);
  if (o.right) sc.text(o.right, 127, 0, 'r');
  const T = 9, B = 58, M = 33, lo = o.lo, hi = o.hi, span = hi - lo;
  const bx = b => Math.round((b - lo) / span * 127);
  const inB = o.inB ?? 0, outB = o.outB ?? o.len;
  for (let x = 0; x < 128; x++) {
    const b0 = lo + x / 128 * span;
    if (b0 >= o.len || (o.recTo != null && b0 > o.recTo)) continue;
    let p = 0;
    for (let k = 0; k < 4; k++) p = Math.max(p, amp(lo + (x + k / 4) / 128 * span, o.seed || 1, o.dub));
    const h = Math.max(1, Math.round(p * 21));
    const inside = b0 >= inB && b0 < outB;
    sc.vline(x, M - h, M + h, inside ? sc.fg : sc.dm);
  }
  // ticks counted from IN
  const k0 = Math.ceil(lo - inB - 1e-6), k1 = Math.floor(hi - inB + 1e-6);
  for (let k = k0; k <= k1; k++) {
    const x = bx(inB + k); if (x < 0 || x > 127) continue;
    const ph = ((k % 4) + 4) % 4 === 0, h = ph ? 4 : 2;
    sc.vline(x, T, T + h - 1); sc.vline(x, B - h + 1, B);
  }
  // beat dots
  const b0 = Math.ceil((lo - inB) * 4 - 1e-6), b1 = Math.floor((hi - inB) * 4 + 1e-6);
  if (span <= 16) for (let j = b0; j <= b1; j++) { const x = bx(inB + j / 4); if (x >= 0 && x < 128 && j % 4) sc.px(x, 61, sc.dm); }
  // markers
  if (o.markers) {
    for (const [b, isIn] of [[inB, true], [outB, false]]) {
      if (b < lo - 1e-6 || b > hi + 1e-6) continue;
      const x = clamp(bx(b), 0, 127);
      for (let y = T; y <= B; y += 2) sc.px(x, y);
      if (isIn) { sc.rect(x, T, 3, 2); sc.px(x, T + 2); sc.px(x + 1, T + 2); } else { sc.rect(x - 2, T, 3, 2); sc.px(x - 1, T + 2); sc.px(x, T + 2); }
    }
  }
  if (o.pos != null) { const x = clamp(bx(o.pos), 0, 127); sc.vline(x, T - 1, B + 1, '#fff'); }
  if (o.over) o.over(sc);
}
/* ---------- LED colours (mirrors pad_color in looper_synth.c) ---------- */
const newState = () => ({ t: [0, 1, 2, 3, 4, 5].map(() => ({ st: 'empty', mute: false, armed: false, countin: false, fx: false, pend: false })), sel: 0, shift: false, arm: false, solo: 0, countIn: true, recAct: 0, fixlen: 0, quant: false, len: 0 });
const FIXL = ['#4a4a4a', '#6a6a6a', '#b8b8b8', '#e4e4e4', '#e4e4e4', '#ffffff'];
const fixlevel = b => b <= 0 ? FIXL[0] : b === 1 ? FIXL[1] : b === 2 ? FIXL[2] : b <= 4 ? FIXL[3] : FIXL[5];
function padColors(s) {
  const any = s.t.some(x => x.st === 'play' || x.st === 'dub' || x.st === 'rec');
  const o = [[], [], [], []];
  o[0][0] = { c: any ? '#ffffff' : '#6a6a6a' };
  o[1][0] = { c: any ? '#ffe94d' : '#4a4a4a' };
  o[2][0] = s.arm ? { c: C.arm, b: 1 } : { c: '#4a4a4a' };
  o[3][0] = { c: s.shift ? '#ffffff' : '#6a6a6a' };
  s.t.forEach((t, i) => {
    const c = i + 1, e = t.st === 'empty';
    o[0][c] = t.armed ? { c: C.arm, b: 1 } : t.countin ? { c: '#ffe94d', b: 1 } : e ? (t.pend ? { c: C.rec, b: 1 } : { c: vdim(i) }) : t.st === 'rec' ? { c: C.rec } : t.st === 'dub' ? { c: C.dub } : { c: vdim(i) };
    o[1][c] = e ? { c: null } : t.mute ? { c: '#4a4a4a' } : (t.st === 'play' || t.st === 'dub') ? { c: C.play } : t.st === 'rec' ? { c: vdim(i) } : { c: dim(i) };
    o[2][c] = s.solo === c ? { c: TC[i], b: 1 } : t.mute ? { c: '#4a4a4a' } : e ? { c: vdim(i) } : { c: TC[i] };
    o[3][c] = s.shift ? { c: t.fx ? '#ffffff' : vdim(i) } : { c: s.sel === c ? TC[i] : vdim(i) };
  });
  o[3][7] = { c: s.countIn ? C.yel : C.yelD };
  o[2][7] = { c: s.recAct ? C.dub : C.roy };
  o[1][7] = { c: fixlevel(s.fixlen) };
  o[0][7] = { c: s.quant ? C.cy : C.cyD };
  return o;
}
/* ---------- Device (inline SVG clone) ---------- */
const tpl = $('#tpl-move');
const BTN = { loop: [1605, 360, 'Loop'], x: [1605, 478, 'Delete (X)'], undo: [1605, 594, 'Undo'] };
class Device {
  constructor(host, opt = {}) {
    this.host = host; host.classList.add('device');
    const svg = tpl.content.firstElementChild.cloneNode(true);
    svg.removeAttribute('width'); svg.removeAttribute('height'); svg.setAttribute('role', 'img'); svg.setAttribute('aria-label', 'Ableton Move');
    $$('defs', svg).forEach(d => d.remove());
    $$('[clip-path]', svg).forEach(d => d.removeAttribute('clip-path'));
    this.pad = {}; this.knob = {}; this.stepEl = {};
    $$('[id]', svg).forEach(el => {
      const id = el.id; el.removeAttribute('id');
      let m;
      if ((m = id.match(/^pad-(\d+)$/))) { const n = +m[1] - 1, row = 4 - Math.floor(n / 8), col = n % 8 + 1; el.classList.add('pad'); el.dataset.row = row; el.dataset.col = col; this.pad[row + ',' + col] = el; }
      else if ((m = id.match(/^top-knob-(\d)$/))) { this.knob[+m[1]] = el; el.classList.add('hot-knob'); }
      else if ((m = id.match(/^step-(\d+)$/))) { this.stepEl[+m[1]] = el; el.classList.add('stepb'); }
      else if (id === 'display') this.display = el;
    });
    this.svg = svg; host.appendChild(svg);
    this.over = document.createElementNS(NS, 'g'); svg.appendChild(this.over);
    for (const i in this.knob) {
      const b = this.knob[i].querySelector('path').getBBox(); const cx = b.x + b.width / 2, cy = b.y + b.height / 2;
      const ln = document.createElementNS(NS, 'line'); ln.setAttribute('x1', cx); ln.setAttribute('y1', cy); ln.setAttribute('x2', cx); ln.setAttribute('y2', cy - 20); ln.setAttribute('class', 'kn-ind');
      this.knob[i].appendChild(ln); this.knob[i]._ln = ln; this.knob[i]._c = [cx, cy]; this.setKnob(i, .5);
    }
    this.btn = {};
    for (const k in BTN) {
      const [x, y] = BTN[k]; const c = document.createElementNS(NS, 'circle');
      c.setAttribute('cx', x); c.setAttribute('cy', y); c.setAttribute('r', 41); c.setAttribute('class', 'hit'); c.dataset.hot = k; c.setAttribute('tabindex', 0); c.setAttribute('role', 'button');
      this.over.appendChild(c); this.btn[k] = c;
    }
    this.stepHit = {};
    for (let n = 1; n <= 16; n++) {
      const b = this.stepEl[n].querySelector('path').getBBox(); const c = document.createElementNS(NS, 'circle');
      c.setAttribute('cx', b.x + b.width / 2); c.setAttribute('cy', b.y + b.height / 2); c.setAttribute('r', 26); c.setAttribute('class', 'hit'); c.dataset.hot = 'step'; c.dataset.n = n;
      c.setAttribute('role', 'button'); c.setAttribute('tabindex', n === 1 ? 0 : -1);
      this.over.appendChild(c); this.stepHit[n] = c; this.stepEl[n]._c = [b.x + b.width / 2, b.y + b.height / 2];
    }
    const db = this.display.querySelector('path').getBBox();
    const dh = document.createElementNS(NS, 'rect'); dh.setAttribute('x', db.x); dh.setAttribute('y', db.y); dh.setAttribute('width', db.width); dh.setAttribute('height', db.height); dh.setAttribute('class', 'hit'); dh.dataset.hot = 'display'; dh.setAttribute('tabindex', 0); dh.setAttribute('role', 'img');
    this.over.appendChild(dh); this.btn.display = dh;
    for (const k in this.pad) { const el = this.pad[k]; el.dataset.hot = 'pad'; el.setAttribute('tabindex', 0); el.setAttribute('role', 'button'); }
    for (const i in this.knob) { const el = this.knob[i]; el.dataset.hot = 'knob'; el.dataset.n = i; el.setAttribute('tabindex', 0); el.setAttribute('role', 'button'); }
    if (opt.screen) {
      const d = document.createElement('div'); d.className = 'dscreen'; const cv = document.createElement('canvas'); cv.setAttribute('aria-hidden', 'true'); d.appendChild(cv); host.appendChild(d); this.screen = new Screen(cv);
    }
    this.relabel(); onLang(() => this.relabel());
    this.setPadsBase();
    if (opt.labels) this.initLabels();
  }
  relabel() {
    for (const k in this.pad) { const [r, c] = k.split(',').map(Number); this.pad[k].setAttribute('aria-label', t('dev.pad', r, c)); }
    for (const i in this.knob) this.knob[i].setAttribute('aria-label', t('dev.knob', i));
    for (let n = 1; n <= 16; n++) this.stepHit[n].setAttribute('aria-label', t('dev.step', n));
    for (const k in BTN) this.btn[k].setAttribute('aria-label', BTN[k][2]);
    this.btn.display.setAttribute('aria-label', t('dev.display'));
  }
  initLabels() {
    const g = document.createElementNS(NS, 'g'); g.setAttribute('class', 'lbls'); g.setAttribute('aria-hidden', 'true'); this.svg.appendChild(g);
    const add = (x, y, txt, fs, cls, anchor) => { const e = document.createElementNS(NS, 'text'); e.setAttribute('x', x); e.setAttribute('y', y); e.setAttribute('font-size', fs); e.setAttribute('text-anchor', anchor || 'middle'); if (cls) e.setAttribute('class', cls); e.textContent = txt; g.appendChild(e); return e; };
    const X = [398, 551, 703, 856, 1008, 1160, 1312, 1465], Y = { 4: 245, 3: 362, 2: 479, 1: 596 };
    const c1 = { 4: 'SHIFT', 3: 'ARM', 2: 'STOP ALL', 1: 'ALL' }, c8 = { 4: 'COUNT', 3: 'REC>PLAY', 2: 'LEN', 1: 'QUANT' }, tr = { 4: 'SEL', 3: 'MUTE', 2: 'PLAY/STOP', 1: 'REC' };
    for (let r = 4; r >= 1; r--) {
      add(X[0], Y[r] + 6, c1[r], 20); add(X[7], Y[r] + 6, c8[r], 20);
      for (let c = 1; c <= 6; c++) { add(X[c], Y[r] - 3, tr[r], 18); add(X[c], Y[r] + 22, 'T' + c, 16, 'n'); }
    }
    this.kl = []; for (let i = 1; i <= 8; i++) this.kl.push(add(this.knob[i]._c[0], this.knob[i]._c[1] + 56, '', 19));
    for (let n = 1; n <= 16; n++) add(this.stepEl[n]._c[0], this.stepEl[n]._c[1] + 46, n, 17, 'n');
    add(326, 738, 'LEN', 19, '', 'end');
    add(1605, 420, 'LOOP', 18); add(1605, 538, 'X DEL', 18); add(1605, 654, 'UNDO', 18); add(1710, 421, 'M MUTE', 18); add(76, 790, 'PLAY/STOP', 16); add(180, 790, 'REC', 18);
    this.setKnobLabels(0);
  }
  setKnobLabels(sel) {
    if (!this.kl || this._ks === sel) return; this._ks = sel;
    const L = sel ? ['IN', 'OUT', 'VOL', 'PAN', 'FX', 'AMT', 'SPD', 'REV'] : ['VOL 1', 'VOL 2', 'VOL 3', 'VOL 4', 'VOL 5', 'VOL 6', 'IN', 'MASTER'];
    this.kl.forEach((e, i) => { e.textContent = L[i]; });
  }
  setPadsBase() { for (const k in this.pad) this.setPad(...k.split(',').map(Number), null, false); }
  setPad(r, c, col, blink) {
    const g = this.pad[r + ',' + c]; if (!g) return;
    g.querySelector('path').style.fill = col || '#2c2c2c';
    g.style.filter = col && col !== '#2c2c2c' && !/^#[2-5]/.test(col) ? `drop-shadow(0 0 9px ${col}99)` : 'none';
    g.classList.toggle('blink', !!blink);
  }
  apply(s) {
    const o = padColors(s);
    for (let r = 1; r <= 4; r++) for (let c = 1; c <= 8; c++) { const p = o[r - 1][c - 1]; this.setPad(r, c, p.c, p.b); }
    if (s.len != null) this.setLen(s.len);
  }
  setLen(n) {
    for (let i = 1; i <= 16; i++) { const p = this.stepEl[i].querySelector('path'); p.style.fill = n === 0 ? '#2a2a2a' : i === n ? '#ffffff' : i < n ? '#6c6c6c' : '#2a2a2a'; }
  }
  setKnob(i, v) { const k = this.knob[i]; if (!k) return; k._ln.setAttribute('transform', `rotate(${-135 + 270 * clamp(v, 0, 1)} ${k._c[0]} ${k._c[1]})`); }
  dimCols(cols) { for (const k in this.pad) { const c = +k.split(',')[1]; this.pad[k].classList.toggle('dimcol', cols && !cols.includes(c)); } }
  focus(list) { for (const k in this.pad) this.pad[k].classList.toggle('dimcol', !!list && !list.some(([r, c]) => k === r + ',' + c)); }
  ring(list) {
    $$('.ring', this.over).forEach(n => n.remove());
    (list || []).forEach(q => {
      let el;
      if (q.pad) { const b = this.pad[q.pad[0] + ',' + q.pad[1]].querySelector('path').getBBox(); el = document.createElementNS(NS, 'rect'); el.setAttribute('x', b.x - 5); el.setAttribute('y', b.y - 5); el.setAttribute('width', b.width + 10); el.setAttribute('height', b.height + 10); el.setAttribute('rx', 10); }
      else if (q.btn) { const [x, y] = BTN[q.btn]; el = document.createElementNS(NS, 'circle'); el.setAttribute('cx', x); el.setAttribute('cy', y); el.setAttribute('r', 46); }
      else if (q.knob) { const k = this.knob[q.knob]; el = document.createElementNS(NS, 'circle'); el.setAttribute('cx', k._c[0]); el.setAttribute('cy', k._c[1]); el.setAttribute('r', 38); }
      else if (q.step) { const [x, y] = this.stepEl[q.step]._c; el = document.createElementNS(NS, 'circle'); el.setAttribute('cx', x); el.setAttribute('cy', y); el.setAttribute('r', 30); }
      if (el) { el.setAttribute('class', 'ring'); this.over.appendChild(el); }
    });
  }
  bind(cb) {
    const call = (ev, act) => { const tg = ev.target.closest('[data-hot]'); if (!tg) return; const k = tg.dataset.hot;
      const data = k === 'pad' ? { row: +tg.dataset.row, col: +tg.dataset.col } : k === 'knob' ? { n: +tg.dataset.n } : k === 'step' ? { n: +tg.dataset.n } : { name: k };
      cb(k, data, act, ev); };
    this.host.addEventListener('pointerover', e => call(e, 'hover'));
    this.host.addEventListener('focusin', e => call(e, 'focus'));
    this.host.addEventListener('click', e => call(e, 'click'));
    this.host.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); call(e, 'click'); } });
  }
}

/* ---------- shared helpers ---------- */
// LED illustration: a pad swatch that cycles through states (click = next). states: [{c, b, k}], k = i18n key of the caption
function ledDemo(el, states, per = 1.9) {
  el.innerHTML = '<button type="button" class="lsw" aria-hidden="true" tabindex="-1"></button><span class="lcap"></span>';
  const sw = $('.lsw', el), cap = $('.lcap', el); let i = 0, t0 = null, hold = 0, shown = -1, lang = '';
  const show = () => { const s = states[i]; sw.style.setProperty('--lc', s.c); sw.classList.toggle('blink', !!s.b); const txt = (s.lit ? s.lit + ' ' : '') + (s.k ? t(s.k) : ''); if (cap.textContent !== txt) cap.textContent = txt; };
  el.addEventListener('click', () => { i = (i + 1) % states.length; hold = performance.now() + 6000; show(); });
  reg(el, tt => { if (t0 === null) t0 = tt; if (!reduce && performance.now() > hold) { const n = Math.floor((tt - t0) / per) % states.length; if (n !== i) i = n; } show(); });
  show();
}
// card <-> device link: hover/focus/click on a card rings its pad; otherwise the ring cycles
function cardLink(dev, cardsEl, col, list) {
  let pin = 0, cur = -1; const rows = list || [4, 3, 2, 1];
  const setRing = r => { if (r === cur) return; cur = r; dev.ring(r ? [{ pad: [r, col] }] : []); $$('.pcard', cardsEl).forEach(c => c.classList.toggle('hl', +c.dataset.pad === r)); };
  $$('.pcard', cardsEl).forEach(c => { const r = +c.dataset.pad; ['pointerenter', 'focusin', 'click'].forEach(ev => c.addEventListener(ev, () => { pin = r; setRing(r); })); c.addEventListener('pointerleave', () => { pin = 0; }); });
  reg(cardsEl, tt => { if (pin) { setRing(pin); return; } if (reduce) { setRing(rows[0]); return; } setRing(rows[Math.floor(tt / 2.6) % rows.length]); });
  return { pick: r => { pin = r; setRing(r); } };
}
const sixteenths = b => { const k = Math.round(b * 16); return k < 16 ? `+${k}/16` : k % 16 === 0 ? `+${k / 16}B` : `+${Math.floor(k / 16)}B ${k % 16}/16`; };
const lenLabel = b => { const k = Math.round(b * 16), bars = Math.floor(k / 16), r = k % 16; return r === 0 ? `${bars} ${bars === 1 ? 'BAR' : 'BARS'}` : bars === 0 ? `${r}/16` : `${bars}B ${r}/16`; };
// Schwung-style knob page (128x64): o = {title, state, e:[{lab,txt,v,b}], focus}
function drawKnobPage(sc, o) {
  sc.clear(); sc.text(o.title, 0, 0); sc.text(o.state, 127, 0, 'r');
  o.e.forEach((e, i) => {
    const cx = 16 + 32 * (i % 4), top = i < 4, cy = top ? 17 : 41, ly = top ? 27 : 51;
    if (e.b) sc.box(cx, cy - 8, 18, 17, e.txt); else sc.dial(cx, cy, 8, e.v, sc.fg);
    if (o.focus === i) sc.text(e.txt, cx, ly, 'c', sc.fg, true); else sc.text(e.lab, cx, ly, 'c');
  });
}
/* ---------- reveal + scrollspy + nav + language ---------- */
const io = 'IntersectionObserver' in window ? new IntersectionObserver(es => es.forEach(e => { if (e.isIntersecting) { e.target.classList.add('in'); io.unobserve(e.target); } }), { threshold: .08 }) : null;
$$('.reveal').forEach(n => io ? io.observe(n) : n.classList.add('in'));
const links = $$('#nav-list a'); const secs = links.map(a => $(a.getAttribute('href'))).filter(Boolean);
const navw = $('#navw'), menuBtn = $('#menu-btn'), navUl = $('#nav-list');
function spy() {
  let cur = null; const y = innerHeight * .3;
  secs.forEach(s => { if (s.getBoundingClientRect().top < y) cur = s; });
  links.forEach(a => { const on = cur && a.getAttribute('href') === '#' + cur.id; a.classList.toggle('on', !!on); a.toggleAttribute('aria-current', !!on); if (on) { document.documentElement.style.setProperty('--sec-nav', getComputedStyle(cur).getPropertyValue('--sec') || ''); if (navUl.scrollWidth > navUl.clientWidth + 2) { const l = a.offsetLeft - navUl.clientWidth / 2 + a.clientWidth / 2; if (Math.abs(navUl.scrollLeft - l) > 40) navUl.scrollTo({ left: l, behavior: reduce ? 'auto' : 'smooth' }); } } });
}
addEventListener('scroll', spy, { passive: true }); spy();
const closeMenu = () => { navw.classList.remove('open'); menuBtn.setAttribute('aria-expanded', 'false'); };
menuBtn.addEventListener('click', () => { const o = navw.classList.toggle('open'); menuBtn.setAttribute('aria-expanded', String(o)); });
links.forEach(a => a.addEventListener('click', closeMenu));
addEventListener('keydown', e => { if (e.key === 'Escape') closeMenu(); });
document.addEventListener('click', e => { if (navw.classList.contains('open') && !e.target.closest('#navw,#menu-btn')) closeMenu(); });
$('#lang').addEventListener('change', e => setLang(e.target.value));
function setLang(l) {
  if (!LANGS.includes(l)) return;
  loadLang(l).then(() => {
    LANG = l; try { localStorage.setItem('l606lang', l); } catch (e) { }
    try { if (/[?&]lang=/.test(location.search)) { const u = new URL(location.href); u.searchParams.set('lang', l); history.replaceState(null, '', u); } } catch (e) { }
    applyDOM(); hooks.forEach(f => { try { f(); } catch (er) { console.error(er); } });
  });
}
$$('.cmd button').forEach(b => b.addEventListener('click', () => { const x = b.parentElement.querySelector('pre').innerText.trim(); (navigator.clipboard ? navigator.clipboard.writeText(x) : Promise.reject()).then(() => { b.textContent = t('cmd.copied'); setTimeout(() => { b.textContent = t(b.dataset.i18n); }, 1400); }).catch(() => { }); }));
/* =====================================================================
   HERO attract loop
   ===================================================================== */
(() => {
  const host = $('#hero-device'); if (!host) return;
  const dev = new Device(host, { screen: true, labels: true }); const s = newState();
  const BAR = 2, LOOP = 2 * BAR;
  const T = (i, st, extra) => () => { Object.assign(s.t[i], { armed: false, countin: false, mute: false }, { st }, extra || {}); };
  const ev = [
    [0.3, () => { s.t.forEach(t => Object.assign(t, { st: 'empty', mute: false, armed: false, countin: false })); s.solo = 0; s.sel = 0; s.shift = false; s.arm = false; }],
    [0.7, () => { s.t[0].countin = true; }], [2.0, T(0, 'rec')], [4.0, T(0, 'play')],
    [4.8, T(1, 'rec')], [6.4, T(1, 'play')], [7.2, T(2, 'rec')], [8.6, T(2, 'play')],
    [9.4, T(0, 'dub')], [10.8, T(0, 'play')], [11.4, T(3, 'rec')], [12.6, T(3, 'play')],
    [13.2, T(4, 'rec')], [14.2, T(4, 'play')], [14.8, () => { s.t[1].mute = true; }],
    [15.6, () => { s.arm = true; s.t[5].armed = true; }], [17.0, () => { s.arm = false; T(5, 'rec')(); }], [18.6, T(5, 'play')],
    [19.2, () => { s.t[1].mute = false; }], [20.2, () => { s.sel = 3; }], [22.4, () => { s.sel = 0; s.t.forEach(t => { if (t.st !== 'empty') t.st = 'stop'; }); }],
    [23.6, () => { s.t.forEach(t => { if (t.st !== 'empty') t.st = 'play'; }); }], [26.4, () => { s.t.forEach(t => Object.assign(t, { st: 'empty', mute: false })); }],
  ];
  const TOT = 27.6; let idx = 0, t0 = null, tAt = 0;
  const live = () => { const r = s.t.findIndex(t => t.st === 'rec'); if (r >= 0) return [r, 'REC']; const a = s.t.findIndex(t => t.armed); if (a >= 0) return [a, 'ARM']; const c = s.t.findIndex(t => t.countin); if (c >= 0) return [c, 'COUNT']; const d = s.t.findIndex(t => t.st === 'dub'); if (d >= 0) return [d, 'DUB']; const l = s.t.map((t, i) => [t, i]).filter(x => x[0].st === 'play' || x[0].st === 'stop'); if (l.length) return [l[l.length - 1][1], l[l.length - 1][0].st === 'play' ? 'PLAY' : 'STOP']; return [-1, 'EMPTY']; };
  const recStart = {};
  function run(t) {
    if (reduce) { // static pleasant frame
      if (!idx) { s.t.forEach((x, i) => { x.st = i === 4 ? 'dub' : 'play'; }); s.t[1].mute = true; idx = 1; dev.apply(s); }
      draw(3.1); return;
    }
    if (t0 === null) t0 = t; const tt = (t - t0) % TOT;
    if (tt < tAt) idx = 0; tAt = tt;
    while (idx < ev.length && ev[idx][0] <= tt) { const prev = s.t.map(x => x.st).join(); ev[idx][1](); const now = s.t.map(x => x.st); now.forEach((st, i) => { if (st === 'rec') recStart[i] = tt; }); idx++; dev.apply(s); }
    draw(tt);
  }
  function draw(tt) {
    dev.setKnobLabels(s.sel);
    const [i, st] = live(); const sc = dev.screen;
    if (i < 0) { drawWave(sc, { head: 'LOOPER 606', right: 'MIX', lo: 0, hi: 4, len: 0, inB: 0, outB: 4, seed: 1 }); return; }
    const len = LOOP; let recTo = null, pos = ((tt * 1) / BAR) % len;
    if (st === 'REC') { const el = tt - (recStart[i] ?? tt); recTo = el / BAR; pos = recTo; }
    drawWave(sc, { head: (st === 'ARM' ? 'ARM ' : 'PISTA ') + (i + 1), right: st, lo: 0, hi: st === 'REC' ? 2 : len, len: st === 'REC' ? 2 : len, recTo, inB: 0, outB: len, seed: i + 1, dub: st === 'DUB', pos: (st === 'ARM' || st === 'COUNT') ? null : pos, markers: st === 'PLAY' && s.sel > 0 });
  }
  dev.apply(s); reg(host, run);
  // left column: click a control on the drawing to see what it does
  const info = $('#hero-info'); let last = null;
  const render = () => {
    if (!info) return;
    if (!last) return infoShow(info, t('hero.info.t'), [], t('hero.info.d'));
    const [k, d] = last;
    if (k === 'pad') { const [n, desc, sh] = PADINFO(d.row, d.col); infoShow(info, n, [t('map.at', t('row.' + d.row), d.col), d.col === 1 ? 'MASTER' : d.col === 8 ? 'REC SETTINGS' : t('map.track', d.col - 1)], desc, sh); }
    else if (k === 'knob') { const v = d.view; const e = KNOBINFO(v, d.n); infoShow(info, e[0], [t(v === 'mix' ? 'map.viewmix' : 'map.viewpista')], e[1]); }
    else if (k === 'step') infoShow(info, t('map.step', d.n), [t('map.steptag')], t('map.stepd', d.n));
    else infoShow(info, t('bt.' + d.name + '.n'), [t('map.btn')], t('bt.' + d.name + '.d'));
  };
  dev.bind((k, d, act) => {
    if (act !== 'click') return;
    if (k === 'knob') d.view = s.sel ? 'pista' : 'mix';
    last = [k, d]; render();
    dev.ring(k === 'pad' ? [{ pad: [d.row, d.col] }] : k === 'knob' ? [{ knob: d.n }] : k === 'step' ? [{ step: d.n }] : BTN[d.name] ? [{ btn: d.name }] : []);
  });
  render(); onLang(render);
})();
function infoShow(info, title, tags, body, sh) { info.innerHTML = ''; const h = document.createElement('h4'); h.textContent = title; tags.forEach(x => { const sp = document.createElement('span'); sp.className = 'tag'; sp.textContent = x; h.appendChild(sp); }); info.appendChild(h); const p = document.createElement('p'); p.textContent = body; info.appendChild(p); if (sh) { const q = document.createElement('p'); q.className = 'shiftline'; q.textContent = sh; info.appendChild(q); } }
/* =====================================================================
   MAPA interactivo
   ===================================================================== */
const PADINFO = (r, c) => {
  if (c === 1) return [t(`pi.1.${r}.n`), t(`pi.1.${r}.d`), t(`pi.1.${r}.s`)];
  if (c === 8) return [t(`pi.8.${r}.n`), t(`pi.8.${r}.d`), t(`pi.8.${r}.s`)];
  const n = c - 1;
  return [t(`pi.t.${r}.n`, n), t(`pi.t.${r}.d`, n), t(`pi.t.${r}.s`, n)];
};
const KNOBINFO = (view, i) => [t(`ki.${view}.${i}.n`), t(`ki.${view}.${i}.d`)];
(() => {
  const host = $('#map-device'); if (!host) return;
  const dev = new Device(host); const s = newState();
  s.t.forEach((x, i) => { x.st = i === 5 ? 'empty' : 'play'; }); s.t[2].mute = true; s.t[3].st = 'rec'; s.t[4].st = 'dub';
  let shift = false, view = 'pista', col = 0, last = null;
  const info = $('#map-info');
  const show = (title, tags, body, sh) => { info.innerHTML = ''; const h = document.createElement('h4'); h.textContent = title; tags.forEach(x => { const sp = document.createElement('span'); sp.className = 'tag'; sp.textContent = x; h.appendChild(sp); }); info.appendChild(h); const p = document.createElement('p'); p.textContent = body; info.appendChild(p); if (sh) { const q = document.createElement('p'); q.className = 'shiftline'; q.textContent = sh; info.appendChild(q); } };
  function refresh() {
    const sh = { ...s, shift }; dev.apply(sh); dev.dimCols(col ? (col === 1 ? [1] : col === 8 ? [8] : [2, 3, 4, 5, 6, 7]) : null);
    $$('.setmode').forEach(b => b.setAttribute('aria-pressed', String((b.dataset.k === 'shift') === shift)));
    $$('[data-view]').forEach(b => b.setAttribute('aria-pressed', String(b.dataset.view === view)));
    for (let i = 1; i <= 8; i++) dev.setKnob(i, [.7, .55, .62, .5, .3, .45, .5, .8][i - 1]);
  }
  const render = () => {
    if (!last) return show(t('map.hint.t'), [], t('map.hint.d'));
    const [k, d] = last;
    if (k === 'pad') { const [n, desc, sh] = PADINFO(d.row, d.col); show(n, [t('map.at', t('row.' + d.row), d.col), d.col === 1 ? 'MASTER' : d.col === 8 ? 'REC SETTINGS' : t('map.track', d.col - 1)], desc, sh); }
    else if (k === 'knob') { const e = KNOBINFO(view, d.n); show(e[0], [t(view === 'mix' ? 'map.viewmix' : 'map.viewpista')], e[1]); }
    else if (k === 'step') show(t('map.step', d.n), [t('map.steptag')], t('map.stepd', d.n));
    else show(t('bt.' + d.name + '.n'), [t('map.btn')], t('bt.' + d.name + '.d'));
  };
  dev.bind((k, d, act) => { last = [k, d]; render(); });
  $$('.setmode').forEach(b => b.addEventListener('click', () => { shift = b.dataset.k === 'shift'; refresh(); }));
  $$('[data-view]').forEach(b => b.addEventListener('click', () => { view = b.dataset.view; refresh(); render(); }));
  $$('.colleg button').forEach(b => b.addEventListener('click', () => { const v = +b.dataset.col; col = col === v ? 0 : v; $$('.colleg button').forEach(x => x.setAttribute('aria-pressed', String(+x.dataset.col === col))); refresh(); }));
  refresh(); render(); onLang(render);
})();

/* =====================================================================
   PRIMER LOOP walkthrough
   ===================================================================== */
(() => {
  const host = $('#walk-device'); if (!host) return;
  const dev = new Device(host, { screen: true });
  const sc = new Screen($('#walk-screen'));
  const S = o => { const s = newState(); s.countIn = true; return Object.assign(s, o || {}); };
  const mk = (st, ex) => Object.assign({ st: 'empty', mute: false, armed: false, countin: false, fx: false, pend: false }, { st }, ex || {});
  const rest = [1, 2, 3, 4, 5];
  const scenes = [
    { st: S(), ring: [], scr: { head: 'LOOPER 606', right: 'MIX', mode: 'empty' }, led: ['#444', 0] },
    { st: S({ t: [mk('empty', { countin: true })].concat(rest.map(() => mk('empty'))) }), ring: [{ pad: [1, 2] }], scr: { head: 'REC 1', right: 'COUNT', mode: 'count' }, led: ['#ffe94d', 1] },
    { st: S({ t: [mk('rec')].concat(rest.map(() => mk('empty'))) }), ring: [], scr: { head: 'PISTA 1', right: 'REC', mode: 'rec' }, led: [C.rec, 0] },
    { st: S({ t: [mk('play')].concat(rest.map(() => mk('empty'))) }), ring: [{ pad: [1, 2] }], scr: { head: 'PISTA 1', right: 'PLAY', mode: 'play' }, led: [C.play, 0] },
    { st: S({ t: [mk('dub')].concat(rest.map(() => mk('empty'))) }), ring: [{ pad: [1, 2] }], scr: { head: 'PISTA 1', right: 'DUB', mode: 'dub' }, led: [C.dub, 0] },
    { st: S({ t: [mk('play')].concat(rest.map(() => mk('empty'))) }), ring: [{ pad: [1, 2] }], scr: { head: 'PISTA 1', right: 'PLAY', mode: 'dubbed' }, led: [C.play, 0] },
    { st: S({ t: [mk('play', { mute: true })].concat(rest.map(() => mk('empty'))) }), ring: [{ pad: [3, 2] }], scr: { head: 'PISTA 1', right: 'MUTE', mode: 'dubbed' }, led: ['#4a4a4a', 0] },
    { st: S({ t: [mk('stop')].concat(rest.map(() => mk('empty'))) }), ring: [{ pad: [2, 2] }], scr: { head: 'PISTA 1', right: 'STOP', mode: 'dubbed', still: true }, led: [dim(0), 0] },
    { st: S(), ring: [{ btn: 'x' }, { pad: [2, 2] }], scr: { head: 'LOOPER 606', right: 'CLEAR 1', mode: 'empty' }, led: ['#444', 0] },
  ];
  let cur = 0, auto = !reduce, t0 = 0, lastSw = 0; const DUR = 4.2;
  const title = $('#walk-title'), text = $('#walk-text'), dots = $('#walk-dots'), prog = $('#walk-prog i'), led = $('#walk-led'), ledt = $('#walk-ledt'), pp = $('#walk-play');
  scenes.forEach((_, i) => { const b = document.createElement('button'); b.addEventListener('click', () => { go(i); auto = false; upd(); }); dots.appendChild(b); });
  function txt() { const i = cur; title.textContent = (i + 1) + '. ' + t('walk.' + i + '.t'); text.textContent = t('walk.' + i + '.d'); ledt.textContent = t('walk.' + i + '.l'); $$('button', dots).forEach((b, k) => b.setAttribute('aria-label', t('walk.scene', k + 1))); }
  function go(i) { cur = (i + scenes.length) % scenes.length; const sn = scenes[cur]; dev.apply(sn.st); dev.ring(sn.ring); txt(); led.style.setProperty('--lc', sn.led[0]); led.classList.toggle('blink', !!sn.led[1]); $$('button', dots).forEach((b, k) => { b.classList.toggle('on', k === cur); b.classList.toggle('done', k < cur); }); lastSw = null; if (reduce) renderScr(0); }
  function upd() { pp.textContent = t(auto ? 'walk.pause' : 'walk.auto'); pp.setAttribute('aria-pressed', String(auto)); }
  $('#walk-next').addEventListener('click', () => { go(cur + 1); auto = false; upd(); });
  $('#walk-prev').addEventListener('click', () => { go(cur - 1); auto = false; upd(); });
  pp.addEventListener('click', () => { auto = !auto; lastSw = null; upd(); });
  function renderScr(tm) {
    const sn = scenes[cur], m = sn.scr.mode, el = tm - (lastSw ?? tm);
    const L = 2; const o = { head: sn.scr.head, right: sn.scr.right, lo: 0, hi: L, len: L, inB: 0, outB: L, seed: 3, markers: false };
    const pos = (tm / 2) % L;
    if (m === 'empty') o.len = 0;
    else if (m === 'count') { o.len = 0; o.over = s => { const beat = Math.floor((el * 2) % 4) + 1; s.text(String(beat), 64, 24, 'c'); s.text('COUNT-IN', 64, 36, 'c'); }; }
    else if (m === 'rec') { o.recTo = clamp(el / 2, 0, L); o.pos = o.recTo; }
    else if (m === 'play') o.pos = pos;
    else if (m === 'dub') { o.pos = pos; o.dub = true; }
    else { o.dub = true; o.pos = sn.scr.still ? 0.4 : pos; }
    drawWave(sc, o);
    host.querySelector('.dscreen') && dev.screen && drawWave(dev.screen, o);
  }
  reg(host, (tm, dt) => {
    if (lastSw === null) { lastSw = tm; t0 = tm; }
    if (auto && !reduce) { const p = (tm - t0) / DUR; prog.style.width = clamp(p * 100, 0, 100) + '%'; if (p >= 1) { go(cur + 1); lastSw = tm; t0 = tm; } } else prog.style.width = '0';
    renderScr(tm);
  });
  go(0); upd(); onLang(() => { txt(); upd(); });
})();
/* =====================================================================
   AUTO-REC meter
   ===================================================================== */
(() => {
  const cv = $('#meter'); if (!cv) return; const ctx = cv.getContext('2d');
  const pad = $('#recpad'), badge = $('#recbadge'); let thr = -42, t0 = null, state = 'idle', hist = [];
  const W = 900, H = 300; cv.width = W; cv.height = H;
  const DB0 = -72, DB1 = -6, yOf = db => H - 16 - (db - DB0) / (DB1 - DB0) * (H - 32);
  const lvl = t => { const nz = -66 + 2.2 * Math.sin(t * 9) + 1.5 * Math.sin(t * 23.7); if (t < 3) return nz; const k = 1 - Math.exp(-(t - 3) * 1.25); return Math.max(nz, -60 + 44 * k + 3 * Math.sin(t * 11)) - (t > 7.2 ? (t - 7.2) * 9 : 0); };
  $$('[data-thr]').forEach(b => b.addEventListener('click', () => { thr = +b.dataset.thr; $$('[data-thr]').forEach(x => x.setAttribute('aria-pressed', String(x === b))); t0 = null; hist = []; }));
  const LOOP = 9;
  function run(now) {
    if (t0 === null) { t0 = now; hist = []; state = 'idle'; } let t = (now - t0); if (t > LOOP) { t0 = now; hist = []; state = 'idle'; t = 0; }
    if (reduce) { t = 5; }
    const l = lvl(t); hist.push([t, l]); if (hist.length > 600) hist.shift();
    if (t > .8 && state === 'idle') state = 'armed';
    if (state === 'armed' && l > thr) state = 'rec';
    if (reduce) state = 'rec';
    ctx.clearRect(0, 0, W, H); ctx.fillStyle = '#161616'; ctx.fillRect(0, 0, W, H);
    ctx.font = '12px JetBrains Mono, "Noto Sans JP", monospace'; ctx.fillStyle = '#707070'; ctx.strokeStyle = '#2d2d2d'; ctx.lineWidth = 1;
    for (let db = -66; db <= -6; db += 12) { const y = yOf(db); ctx.beginPath(); ctx.moveTo(50, y); ctx.lineTo(W, y); ctx.stroke(); ctx.fillText(db + ' dB', 4, y + 4); }
    const x0 = 56, wX = W - x0 - 10;
    ctx.beginPath(); hist.forEach(([tt, ll], i) => { const x = x0 + (tt / LOOP) * wX, y = yOf(ll); i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); }); ctx.strokeStyle = '#19d3e6'; ctx.lineWidth = 2.5; ctx.stroke();
    ctx.strokeStyle = C.arm; ctx.setLineDash([8, 6]); ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(x0, yOf(thr)); ctx.lineTo(W, yOf(thr)); ctx.stroke(); ctx.setLineDash([]);
    ctx.fillStyle = C.arm; ctx.textAlign = 'right'; ctx.fillText(tr('meter.thr', thr), W - 8, yOf(thr) - 6); ctx.textAlign = 'left';
    if (state !== 'idle') { // armed marker
      const xa = x0 + (.8 / LOOP) * wX; ctx.fillStyle = '#ff9a1f66'; ctx.fillRect(xa - 1, 8, 2, H - 16); ctx.fillStyle = C.arm; ctx.fillText('ARM', xa + 4, 20);
    }
    const hit = hist.find(h => h[1] > thr && h[0] > .8);
    if (hit && state === 'rec') { const xr = x0 + (hit[0] / LOOP) * wX; ctx.fillStyle = C.rec; ctx.fillRect(xr - 1, 8, 2, H - 16); ctx.fillText('REC', xr + 4, 36); ctx.fillRect(xr, H - 14, W - xr, 4); }
    ctx.fillStyle = '#eee'; ctx.fillText(tr('meter.in', Math.round(l)), x0 + 6, H - 3);
    const L = { idle: ['#444', tr('meter.idle'), ''], armed: [C.arm, tr('meter.armed'), 'b'], rec: [C.rec, tr('meter.rec'), ''] }[state];
    pad.style.background = L[0]; pad.classList.toggle('blink', L[2] === 'b'); pad.textContent = state === 'idle' ? 'REC' : state === 'armed' ? 'ARM' : 'REC'; badge.textContent = L[1];
  }
  reg(cv, (t) => run(t));
  $$('[data-thr]')[2] && $$('[data-thr]')[2].setAttribute('aria-pressed', 'true');
})();
/* =====================================================================
   VISTA PISTA: knob page (+ zoom while IN/OUT are turned)
   ===================================================================== */
const FXN = ['OFF', 'LPF', 'HPF', 'CRUSH', 'DELAY', 'PHASER', 'RING'], FXS = ['OFF', 'LPF', 'HPF', 'CRS', 'DLY', 'PHS', 'RNG'];
const SPD = [['1/4', .25], ['1/2', .5], ['3/4', .75], ['1', 1], ['1.5', 1.5], ['2', 2], ['4', 4]];
(() => {
  const host = $('#pista-device'); if (!host) return;
  const dev = new Device(host); const sc = new Screen($('#pista-screen'));
  const BUF = 8;
  const st = { sel: 3, inB: 0, outB: BUF, vol: .78, pan: .35, fx: 5, amt: .62, spd: 3, rev: 0, last: 0, lastT: -9, zoomUntil: -1, zk: 'in', zlab: '' };
  const state = newState(); state.t.forEach((x, i) => x.st = i < 4 ? 'play' : 'empty');
  const names = ['IN', 'OUT', 'VOL', 'PAN', 'FX', 'AMT', 'SPD', 'REV'];
  const panTxt = () => { const p = Math.round((st.pan - .5) * 100); return p === 0 ? 'C' : (p < 0 ? 'L' : 'R') + Math.abs(p); };
  const val = i => [sixteenths(st.inB), lenLabel(st.outB), String(Math.round(st.vol * 127)), panTxt(), FXN[st.fx], String(Math.round(st.amt * 100)), 'x' + SPD[st.spd][0], st.rev ? 'ON' : 'OFF'][i];
  const normv = i => [st.inB / BUF, st.outB / BUF, st.vol, st.pan, 0, st.amt, 0, 0][i];
  const q16 = x => Math.round(x * 16) / 16;
  const now = () => performance.now() / 1000;
  function turn(i, d, mult = 1) {
    if (i === 0) st.inB = clamp(q16(st.inB + d * mult / 16), 0, st.outB - 1 / 16);
    if (i === 1) st.outB = clamp(q16(st.outB + d * mult / 16), st.inB + 1 / 16, BUF);
    if (i === 2) st.vol = clamp(st.vol + d * .06, 0, 1);
    if (i === 3) st.pan = clamp(st.pan + d * .06, 0, 1);
    if (i === 4) st.fx = (st.fx + d + 7) % 7;
    if (i === 5) st.amt = clamp(st.amt + d * .08, 0, 1);
    if (i === 6) st.spd = clamp(st.spd + d, 0, 6);
    if (i === 7) st.rev = st.rev ? 0 : 1;
    st.last = i; st.lastT = now();
    if (i < 2) { st.zoomUntil = now() + 2.5; st.zk = i ? 'out' : 'in'; st.zlab = names[i] + ' ' + val(i); }
    sync();
  }
  function sync() { for (let i = 0; i < 8; i++) dev.setKnob(i + 1, st.sel ? [st.inB / BUF, st.outB / BUF, st.vol, st.pan, st.fx / 6, st.amt, st.spd / 6, st.rev][i] : .5); state.sel = st.sel; dev.apply(state); }
  function drawPage() {
    const tn = now();
    if (!st.sel) { drawKnobPage(sc, { title: 'MIX', state: 'PLAY', focus: -1, e: ['V1', 'V2', 'V3', 'V4', 'V5', 'V6', 'IN', 'MST'].map(l => ({ lab: l, txt: '', v: .6 })) }); return; }
    if (tn < st.zoomUntil) {
      const c = st.zk === 'in' ? st.inB : st.outB, lo = clamp(c - 1, 0, BUF - 2);
      drawWave(sc, { head: `PISTA ${st.sel} ${st.zlab} ZOOM`, right: '', lo, hi: lo + 2, len: BUF, inB: st.inB, outB: st.outB, seed: 7, markers: true, pos: st.inB + ((tn / 2) % (st.outB - st.inB)) });
      return;
    }
    const e = names.map((nm, i) => ({ lab: nm, txt: val(i), v: normv(i), b: i === 4 || i === 6 || i === 7 }));
    e[4].txt = FXS[st.fx]; e[6].txt = SPD[st.spd][0]; e[7].txt = val(7);
    drawKnobPage(sc, { title: 'PISTA ' + st.sel, state: 'PLAY', focus: tn - st.lastT < 1.5 ? st.last : -1, e });
  }
  dev.bind((k, d, act) => { if (k === 'pad' && d.row === 4 && d.col >= 2 && d.col <= 7 && act === 'click') { const tr = d.col - 1; st.sel = st.sel === tr ? 0 : tr; sync(); showToast(); } else if (k === 'knob' && act === 'click') { turn(d.n - 1, 1); showToast(); } });
  const toast = $('#pista-toast'); function showToast() { toast.textContent = st.sel ? `T${st.sel} ${names[st.last]} ${val(st.last)}` : t('pista.mix'); }
  const pbtn = $('#pista-tour');
  let tour = !reduce; const lab = () => { pbtn.setAttribute('aria-pressed', String(tour)); pbtn.textContent = t(tour ? 'btn.pausedemo' : 'btn.autodemo'); };
  pbtn.addEventListener('click', () => { tour = !tour; lab(); });
  const seq = [[0, 1, 6, 3.4], [1, -1, 6, 3.4], [2, -1, 5, .5], [3, 1, 5, .5], [4, 1, 3, .5], [5, 1, 5, .5], [6, 1, 3, .5], [7, 1, 1, .5]];
  let k = 0, tick = 0, acc = 0, wait = 0;
  reg(host, (tm, dt) => {
    if (tour && !reduce) {
      if (wait > 0) wait -= dt; else { acc += dt; if (acc > .3) { acc = 0; if (st.sel === 0) { st.sel = 3; sync(); } const [kn, dir, n, w] = seq[k]; turn(kn, dir, 1); dev.ring([{ knob: kn + 1 }]); showToast(); if (++tick >= n) { tick = 0; wait = w; k = (k + 1) % 8; if (k === 0) { st.inB = 0; st.outB = BUF; sync(); } } } }
    } else if (!tour) dev.ring([]);
    drawPage();
  });
  lab(); sync(); showToast(); onLang(() => { lab(); showToast(); });
})();

/* =====================================================================
   IN / OUT y zoom
   ===================================================================== */
(() => {
  const cv = $('#inout-screen'); if (!cv) return; const sc = new Screen(cv);
  const m = { buf: 8, inB: 1, outB: 6, zk: null, zu: -1, view: 'pista', auto: !reduce, label: '' };
  const q = x => Math.round(x * 16) / 16;
  function turn(k, d, tm) { m[k] = k === 'inB' ? clamp(q(m.inB + d / 16), 0, m.outB - 1 / 16) : clamp(q(m.outB + d / 16), m.inB + 1 / 16, m.buf); m.zk = k; m.zu = tm + 2.5; m.label = k === 'inB' ? 'IN ' + sixteenths(m.inB) : 'OUT ' + lenLabel(m.outB); }
  let tnow = 0, last = -1, fired = {};
  $$('[data-io]').forEach(b => b.addEventListener('click', () => { m.auto = false; syncUI(); const [k, d] = b.dataset.io.split(':'); turn(k === 'in' ? 'inB' : 'outB', +d, tnow); }));
  $$('[data-iov]').forEach(b => b.addEventListener('click', () => { m.view = b.dataset.iov; syncUI(); }));
  const au = $('#io-auto'); au.addEventListener('click', () => { m.auto = !m.auto; syncUI(); });
  function syncUI() { $$('[data-iov]').forEach(b => b.setAttribute('aria-pressed', String(b.dataset.iov === m.view))); au.setAttribute('aria-pressed', String(m.auto)); au.textContent = t(m.auto ? 'btn.pausedemo' : 'btn.autodemo'); }
  syncUI(); onLang(syncUI);
  const LOOPT = 14;
  const script = []; for (let i = 0; i < 6; i++) script.push([1.0 + i * .3, 'inB', 1]); for (let i = 0; i < 10; i++) script.push([6.0 + i * .3, 'outB', -1]);
  reg(cv, (tm, dt) => {
    tnow = tm;
    if (m.auto && !reduce) { const c = tm % LOOPT; if (c < last) { fired = {}; m.inB = 1; m.outB = 6; } last = c; script.forEach(([tt, k, d], i) => { if (c >= tt && !fired[i]) { fired[i] = 1; turn(k, d * 3, tm); } }); }
    const zoom = tm < m.zu; const reg_len = m.outB - m.inB; const pos = m.inB + ((tm / 2) % reg_len);
    if (m.view === 'pista') {
      if (zoom) { const c = m.zk === 'inB' ? m.inB : m.outB, lo = clamp(c - 1, 0, m.buf - 2); drawWave(sc, { head: `PISTA 3 ${m.label} ZOOM`, right: '', lo, hi: lo + 2, len: m.buf, inB: m.inB, outB: m.outB, seed: 7, markers: true, pos }); }
      else drawKnobPage(sc, { title: 'PISTA 3', state: 'PLAY', focus: -1, e: [{ lab: 'IN', txt: sixteenths(m.inB), v: m.inB / m.buf }, { lab: 'OUT', txt: lenLabel(m.outB), v: m.outB / m.buf }, { lab: 'VOL', v: .78 }, { lab: 'PAN', v: .5 }, { lab: 'FX', txt: 'OFF', b: 1 }, { lab: 'AMT', v: .5 }, { lab: 'SPD', txt: '1', b: 1 }, { lab: 'REV', txt: 'OFF', b: 1 }] });
    } else {
      if (zoom) { const c = m.zk === 'inB' ? m.inB : m.outB, lo = clamp(c - 1, 0, m.buf - 2); drawWave(sc, { head: `START 3 ${m.label} ZOOM`, right: '', lo, hi: lo + 2, len: m.buf, inB: m.inB, outB: m.outB, seed: 7, markers: true, pos }); }
      else drawWave(sc, { head: 'PLAY 3 - ' + (Math.abs(reg_len - Math.round(reg_len)) < .04 ? Math.round(reg_len) + ' BARS' : reg_len.toFixed(1) + ' B'), right: '', lo: m.inB, hi: m.outB, len: m.buf, inB: m.inB, outB: m.outB, seed: 7, markers: false, pos });
    }
    $('#io-info').textContent = t('io.info', m.inB.toFixed(2), m.outB.toFixed(2), reg_len.toFixed(2), m.view.toUpperCase());
  });
})();
/* =====================================================================
   Velocidad y reverse
   ===================================================================== */
(() => {
  const cv = $('#speed-screen'); if (!cv) return; const sc = new Screen(cv);
  let si = 3, rev = false, ph = 0; const note = $('#speed-note'), cont = $('#speed-chips');
  SPD.forEach((s, i) => { const b = document.createElement('button'); b.textContent = 'x' + s[0]; b.addEventListener('click', () => { si = i; upd(); }); cont.appendChild(b); });
  $('#rev-btn').addEventListener('click', () => { rev = !rev; upd(); });
  function upd() { $$('button', cont).forEach((b, i) => b.setAttribute('aria-pressed', String(i === si))); $('#rev-btn').setAttribute('aria-pressed', String(rev)); const r = SPD[si][1]; const st = 12 * Math.log2(r); note.textContent = t('speed.note', SPD[si][0], st === 0 ? t('speed.orig') : t('speed.semi', (st > 0 ? '+' : '') + st.toFixed(1)), t(rev ? 'speed.rev' : 'speed.fwd'), t((r !== 1 || rev) ? 'speed.od.off' : 'speed.od.on')); }
  upd(); onLang(upd);
  reg(cv, (t, dt) => { const r = SPD[si][1]; if (!reduce) ph = (ph + dt * r * .5 * (rev ? -1 : 1) + 2) % 2; else ph = .6;
    drawWave(sc, { head: 'PISTA 3', right: 'x' + SPD[si][0] + (rev ? ' REV' : ''), lo: 0, hi: 2, len: 2, inB: 0, outB: 2, seed: 4, pos: ph, markers: false }); });
})();
/* =====================================================================
   Columna 1 (MASTER)
   ===================================================================== */
(() => {
  const host = $('#col1-device'); if (!host) return;
  const dev = new Device(host); const s = newState();
  s.countIn = false;
  const cards = $('#col1-cards'); const link = cardLink(dev, cards, 1);
  const L = (sel, states) => { const el = $(sel, cards); if (el) ledDemo(el, states); };
  L('[data-led=shift]', [{ c: '#6a6a6a', k: 'led.shift.0' }, { c: '#ffffff', k: 'led.shift.1' }]);
  L('[data-led=arm]', [{ c: '#4a4a4a', k: 'led.arm.0' }, { c: C.arm, b: 1, k: 'led.arm.1' }]);
  L('[data-led=stopall]', [{ c: '#4a4a4a', k: 'led.stopall.0' }, { c: '#ffe94d', k: 'led.stopall.1' }]);
  L('[data-led=all]', [{ c: '#6a6a6a', k: 'led.all.0' }, { c: '#ffffff', k: 'led.all.1' }]);
  // STOP ALL vs ALL START/STOP simulator
  const init = () => ['play', 'play', 'play', 'play', 'stop', 'empty'];
  let ts = init(), logKey = ['sim.hint'];
  const strip = $('#allsim-strip'); const spans = [0, 1, 2, 3, 4, 5].map(i => { const e = document.createElement('span'); e.textContent = i + 1; strip.appendChild(e); return e; });
  const running = () => ts.some(x => x === 'play');
  function render() {
    spans.forEach((e, i) => { const x = ts[i]; e.style.background = x === 'play' ? C.play : x === 'stop' ? dim(i) : '#333'; e.classList.toggle('on', x === 'play'); e.title = x; });
    s.t.forEach((x, i) => { x.st = ts[i]; }); dev.apply(s);
    $('#allsim-log').textContent = t(...logKey);
  }
  const stopAll = () => { const was = running(); ts = ts.map(x => x === 'empty' ? x : 'stop'); logKey = [was ? 'sim.stopall.did' : 'sim.stopall.none']; render(); };
  const allBtn = () => {
    if (running()) { ts = ts.map(x => x === 'empty' ? x : 'stop'); logKey = ['sim.all.stop']; }
    else if (ts.some(x => x === 'stop')) { ts = ts.map(x => x === 'stop' ? 'play' : x); logKey = ['sim.all.start']; }
    else logKey = ['sim.all.none'];
    render();
  };
  $('#sim-stop').addEventListener('click', () => { stopAll(); link.pick(2); });
  $('#sim-all').addEventListener('click', () => { allBtn(); link.pick(1); });
  $('#sim-reset').addEventListener('click', () => { ts = init(); logKey = ['sim.hint']; render(); });
  dev.bind((k, d, act) => {
    if (k !== 'pad' || d.col !== 1 || act !== 'click') return;
    link.pick(d.row);
    if (d.row === 4) { s.shift = !s.shift; render(); } else if (d.row === 3) { s.arm = !s.arm; render(); } else if (d.row === 2) stopAll(); else allBtn();
  });
  dev.focus([[4, 1], [3, 1], [2, 1], [1, 1]]);
  render(); onLang(render);
})();

/* =====================================================================
   Columna 8 (REC SETTINGS)
   ===================================================================== */
(() => {
  const host = $('#col8-device'); if (!host) return;
  const dev = new Device(host); const s = newState(); s.countIn = true; s.recAct = 0; s.quant = true;
  const cards = $('#col8-cards'); const link = cardLink(dev, cards, 8);
  const L = (sel, states) => { const el = $(sel, cards); if (el) ledDemo(el, states); };
  L('[data-led=countin]', [{ c: C.yel, k: 'led.countin.1' }, { c: C.yelD, k: 'led.countin.0' }]);
  L('[data-led=recact]', [{ c: C.roy, k: 'led.recact.0' }, { c: C.dub, k: 'led.recact.1' }]);
  L('[data-led=fixlen]', [0, 1, 2, 4, 8].map(b => ({ c: fixlevel(b), lit: b ? b + ' ' + (b === 1 ? 'BAR' : 'BARS') : 'FREE', k: b ? 'led.fixlen.n' : 'led.fixlen.0' })));
  L('[data-led=quant]', [{ c: C.cy, k: 'led.quant.1' }, { c: C.cyD, k: 'led.quant.0' }]);
  const LF = [0, 1, 2, 4, 8]; let fi = 0;
  function render() { dev.apply(s); }
  // fixed length tape
  const tape = $('#demo-fix'); for (let i = 0; i < 8; i++) tape.appendChild(document.createElement('i'));
  const fv = $('#fix-val'); const fixTap = () => { fi = (fi + 1) % 5; s.fixlen = LF[fi]; fixText(); render(); };
  const fixText = () => { fv.textContent = s.fixlen ? t('fix.val', s.fixlen) : t('fix.free'); };
  $('#fix-tap').addEventListener('click', () => { fixTap(); link.pick(2); });
  reg(tape, tm => {
    const N = s.fixlen || 8, per = N * .55 + 1.6, ph = (tm % per) / (N * .55); const cells = $$('i', tape);
    cells.forEach((c, i) => { const lim = s.fixlen && i >= s.fixlen; const filled = i < Math.min(N, ph * N + .001); c.className = lim ? 'out' : filled ? (s.fixlen && ph >= 1 ? 'done' : 'rec') : (!s.fixlen && ph >= 1 && i < 5 ? 'free' : ''); });
    if (!s.fixlen && ph >= 1) cells.forEach((c, i) => { c.className = i < 5 ? 'rec' : ''; });
  });
  dev.bind((k, d, act) => {
    if (k !== 'pad' || d.col !== 8 || act !== 'click') return; link.pick(d.row);
    if (d.row === 4) { s.countIn = !s.countIn; } else if (d.row === 3) s.recAct = s.recAct ? 0 : 1; else if (d.row === 2) { fixTap(); return; } else s.quant = !s.quant;
    render();
  });
  dev.focus([[4, 8], [3, 8], [2, 8], [1, 8]]);
  // count-in demo
  let ci = 1; const sc1 = new Screen($('#demo-count'));
  $$('[data-ci]').forEach(b => b.addEventListener('click', () => { ci = +b.dataset.ci; $$('[data-ci]').forEach(x => x.setAttribute('aria-pressed', String(x === b))); }));
  reg($('#demo-count'), tm => {
    const beats = 4 * ci, cnt = beats * .5, per = cnt + 3.2, c = tm % per;
    if (c < cnt) { const b = Math.floor(c * 2); drawWave(sc1, { head: 'REC 1', right: 'COUNT', lo: 0, hi: 2, len: 0, inB: 0, outB: 2, seed: 3, over: x => { x.text(String(b % 4 + 1), 64, 20, 'c'); x.text('COUNT-IN', 64, 32, 'c'); x.text(ci > 1 ? (b < 4 ? '1/2' : '2/2') : '1/1', 64, 44, 'c'); for (let i = 0; i < beats; i++) x.rect(64 - beats * 3 + i * 6, 54, 4, 3, i <= b ? x.fg : x.dm); } }); }
    else { const el = c - cnt; drawWave(sc1, { head: 'PISTA 1', right: 'REC', lo: 0, hi: 2, len: 2, recTo: clamp(el / 1.5, 0, 2), inB: 0, outB: 2, seed: 3, pos: clamp(el / 1.5, 0, 2) }); }
  });
  // rec action demo
  const ra = (id, dub) => { const sc = new Screen($(id)); reg($(id), tm => {
    const c = tm % 9, R = 3, el = c - 0;
    if (c < R) drawWave(sc, { head: 'PISTA 1', right: 'REC', lo: 0, hi: 2, len: 2, recTo: clamp(c / 1.5, 0, 2), inB: 0, outB: 2, seed: 5, pos: clamp(c / 1.5, 0, 2), over: x => x.rect(121, 55, 5, 5, C.rec) });
    else { const e = c - R, flash = e < .5; drawWave(sc, { head: 'PISTA 1', right: dub ? 'DUB' : 'PLAY', lo: 0, hi: 2, len: 2, inB: 0, outB: 2, seed: 5, dub: dub && e > 1.2, pos: (e / 1.5) % 2, over: x => { x.rect(121, 55, 5, 5, dub ? C.dub : C.play); if (flash) x.text('REC>' + (dub ? 'DUB' : 'PLAY'), 64, 52, 'c', null, true); } }); }
  }); };
  ra('#demo-ra0', false); ra('#demo-ra1', true);
  // quantize demo
  const scq = new Screen($('#demo-quant'));
  reg($('#demo-quant'), tm => {
    const c = tm % 9, T = clamp(c / 6.5 * 4.6, 0, 4.6), X = b => Math.round(8 + b * 25), P1 = 1.4, P2 = 3.6;
    scq.clear(); for (let b = 0; b <= 4; b++) for (let y = 2; y < 62; y += 3) scq.px(X(b), y, scq.dm);
    const lane = (y, on) => {
      scq.text(on ? 'QUANT ON' : 'QUANT OFF', 0, y - 14);
      const st = on ? 2.0 : P1, en = on ? 4.0 : P2;
      if (T >= P1) scq.rect(X(P1), y - 6, 1, 3, scq.fg);
      if (on && T >= P1 && T < 2.0 && Math.floor(tm * 4) % 2 === 0) scq.rect(X(P1), y, X(2.0) - X(P1), 4, C.rec);
      if (T >= st) { const to = Math.min(T, en); scq.rect(X(st), y, Math.max(1, X(to) - X(st)), 8, C.rec); }
      if (T >= P2) scq.rect(X(P2), y - 6, 1, 3, scq.fg);
      if (T >= en && c > 6.5) scq.text(on ? '2 BARS' : '2.2 BARS', on ? 70 : 62, y + 10, 'l');
    };
    lane(17, false); lane(46, true);
  });
  const raN = () => { $('#ra-note').textContent = t('ra.note'); };
  render(); fixText(); raN(); onLang(() => { fixText(); raN(); });
})();
/* =====================================================================
   Botones de paso
   ===================================================================== */
(() => {
  const host = $('#steps-device'); if (!host) return;
  const dev = new Device(host); const s = newState(); s.t.forEach((x, i) => x.st = i < 3 ? 'play' : 'empty'); s.sel = 1;
  const REC = 4; const bars = $('#bars16'), led16 = $('#led16');
  for (let i = 0; i < 16; i++) { bars.appendChild(document.createElement('i')); led16.appendChild(document.createElement('i')); }
  let mode = 'clip', clipLen = 4, freeLen = 0, key = ['steps.hint'], toast = '';
  const cells = $$('i', bars), leds = $$('i', led16);
  function render(anim) {
    const n = mode === 'clip' ? clipLen : freeLen;
    cells.forEach((e, i) => { e.style.transitionDelay = anim ? (i * 14) + 'ms' : '0ms'; e.className = mode === 'clip' ? (i < Math.min(n, REC) ? 'rec' : i < n ? 'rep' : 'cut') : (i < n ? 'nx' : 'cut'); });
    leds.forEach((e, i) => { e.className = n > 0 && i + 1 === n ? 'w' : n > 0 && i + 1 < n ? 'g' : ''; });
    dev.setLen(n); dev.stepHit[1] && 0;
    $('#steps-toast').textContent = toast;
    $('#len-note').textContent = t(...key);
    $$('[data-sm]').forEach(b => b.setAttribute('aria-pressed', String(b.dataset.sm === mode)));
  }
  function press(n) {
    if (mode === 'clip') {
      clipLen = n; toast = `LEN 1: ${n} ${n === 1 ? 'BAR' : 'BARS'}`;
      key = n === REC ? ['steps.same', n] : n < REC ? ['steps.short', n] : ['steps.long', n];
    } else {
      freeLen = freeLen === n ? 0 : n; toast = freeLen ? `LEN ${freeLen} ${freeLen === 1 ? 'BAR' : 'BARS'}` : 'LEN FREE';
      key = freeLen ? ['steps.next', freeLen] : ['steps.free'];
    }
    render(true); dev.ring([{ step: n }]);
  }
  $$('[data-sm]').forEach(b => b.addEventListener('click', () => { mode = b.dataset.sm; toast = ''; key = [mode === 'clip' ? 'steps.hint' : 'steps.hint.empty']; render(true); }));
  dev.bind((k, d, act) => { if (act === 'click' && k === 'step') press(d.n); });
  dev.focus([]); // dim the pads, the step buttons are the point
  dev.apply(s); render();
  let tour = 0, lastP = -1;
  reg(host, tm => {
    const cnt = $$('i', bars); const n = mode === 'clip' ? clipLen : freeLen; const per = .35;
    if (n > 0) { const ph = Math.floor(tm / per) % n; if (ph !== lastP) { lastP = ph; cells.forEach((e, i) => e.classList.toggle('ph', i === ph && (mode === 'clip' ? true : false))); } } else if (lastP !== -2) { lastP = -2; cells.forEach(e => e.classList.remove('ph')); }
    if (reduce) return; const p = Math.floor(tm / 3) % 4; if (p !== tour) { tour = p; if (!host.matches(':hover')) dev.ring([{ step: [2, 8, 4, 1][p] }]); }
  });
  onLang(() => render());
})();

/* =====================================================================
   SHIFT
   ===================================================================== */
(() => {
  const host = $('#shift-device'); if (!host) return; const dev = new Device(host);
  const s = newState(); s.t.forEach((x, i) => x.st = 'play'); s.t[1].fx = true; s.t[4].fx = true;
  const msg = $('#shift-msg'); let mk = ['shift.off'];
  const refresh = () => { dev.apply(s); const b = $('#shift-btn'); b.setAttribute('aria-pressed', String(s.shift)); dev.ring([{ pad: [4, 1] }]); msg.textContent = t(...mk); };
  const toggle = () => { s.shift = !s.shift; mk = [s.shift ? 'shift.on' : 'shift.off']; refresh(); };
  $('#shift-btn').addEventListener('click', toggle);
  dev.bind((k, d, act) => { if (act !== 'click' || k !== 'pad') return; if (d.row === 4 && d.col === 1) return toggle(); if (d.col < 2 || d.col > 7) return; const i = d.col - 2;
    if (s.shift) { if (d.row === 4) { s.t[i].fx = !s.t[i].fx; mk = [s.t[i].fx ? 'shift.fxon' : 'shift.fxoff', i + 1]; } else if (d.row === 3) { const sid = i + 1; if (s.solo === sid) { s.solo = 0; s.t.forEach((x, j) => x.mute = (s.pm || [])[j] || false); mk = ['shift.unsolo']; } else { s.pm = s.t.map(x => x.mute); s.solo = sid; s.t.forEach((x, j) => x.mute = j !== i); mk = ['shift.solo', sid]; } } else if (d.row === 1) mk = ['shift.rev', i + 1]; }
    else if (d.row === 3) { s.t[i].mute = !s.t[i].mute; mk = [s.t[i].mute ? 'shift.mute' : 'shift.unmute', i + 1]; }
    refresh(); });
  refresh(); onLang(refresh);
})();

/* =====================================================================
   Undo / CLEAR / RESET
   ===================================================================== */
(() => {
  const host = $('#undo-device'); if (!host) return; const dev = new Device(host); const s = newState();
  const DEF = { FX: 'OFF', VOL: '100', PAN: 'C', SPD: 'x1', REV: 'OFF', SHOT: 'OFF', MUTE: 'OFF' };
  const SET = { FX: 'DELAY 60', VOL: '70', PAN: 'L30', SPD: 'x2', REV: 'ON', SHOT: 'ON', MUTE: 'OFF' };
  let layers = 2, set = { ...SET }, saved = null, redo = null, key = ['undo.hint'], toast = '', lastCol = 2; s.sel = 1;
  const L = $('#layers'), log = $('#undo-log'), chips = $('#settings'), tt = $('#undo-toast');
  const snap = () => ({ layers, set: { ...set } });
  const draw = () => {
    L.innerHTML = ''; for (let i = 0; i < layers; i++) { const e = document.createElement('i'); e.style.setProperty('--c', i ? C.dub : TC[0]); L.appendChild(e); }
    if (!layers) { const e = document.createElement('em'); e.textContent = t('undo.emptyaudio'); L.appendChild(e); }
    chips.innerHTML = ''; for (const k in DEF) { const e = document.createElement('span'); e.className = 'chip2' + (set[k] !== DEF[k] ? ' mod' : ''); e.textContent = `${k} ${set[k]}`; chips.appendChild(e); }
    s.t[0].st = layers ? 'play' : 'empty'; s.t[0].mute = set.MUTE === 'ON'; dev.apply(s);
    tt.textContent = toast; log.textContent = t(...key);
  };
  const act = {
    dub() { if (!layers) { key = ['undo.needaudio']; return draw(); } saved = snap(); redo = null; layers = Math.min(4, layers + 1); toast = 'DUB 1'; key = ['undo.dub']; draw(); dev.ring([{ pad: [1, 2] }]); },
    clear() { if (!layers) { key = ['undo.already']; toast = 'CLEAR 1'; return draw(); } saved = { ...snap(), kind: 'clear' }; redo = null; layers = 0; toast = 'CLEAR 1'; key = ['undo.clear']; draw(); dev.ring([{ btn: 'x' }, { pad: [2, 2] }]); },
    reset() {
      const was = layers > 0; redo = null; toast = 'RESET 1';
      if (was) { saved = { ...snap(), kind: 'reset' }; key = ['undo.reset']; } else { saved = null; key = ['undo.reset.empty']; }
      layers = 0; set = { ...DEF }; draw(); dev.ring([{ btn: 'x' }, { pad: [4, 2] }]);
    },
    undo() { if (!saved) { toast = 'NOTHING TO UNDO'; key = ['undo.nothing']; return draw(); } redo = snap(); const k = saved.kind; layers = saved.layers; set = { ...saved.set }; saved = null; toast = 'UNDO'; key = [k === 'reset' ? 'undo.undo.reset' : k === 'clear' ? 'undo.undo.clear' : 'undo.undo.dub']; draw(); dev.ring([{ btn: 'undo' }]); },
    redo() { if (!redo) { toast = 'NOTHING TO REDO'; key = ['undo.noredo']; return draw(); } saved = snap(); layers = redo.layers; set = { ...redo.set }; redo = null; toast = 'REDO'; key = ['undo.redo']; draw(); dev.ring([{ btn: 'undo' }]); },
    setup() { layers = 2; set = { ...SET }; saved = redo = null; toast = ''; key = ['undo.hint']; draw(); },
    empty() { layers = 0; set = { ...SET }; saved = redo = null; toast = ''; key = ['undo.hint.empty']; draw(); },
  };
  $$('[data-u]').forEach(b => b.addEventListener('click', () => act[b.dataset.u]()));
  dev.bind((k, d, a) => { if (a !== 'click') return; if (k === 'undo') act.undo(); else if (k === 'x') act.clear(); else if (k === 'pad' && d.col === 2) { if (d.row === 4) act.reset(); else act.clear(); } });
  dev.focus([[4, 2], [3, 2], [2, 2], [1, 2]]);
  draw(); dev.ring([{ btn: 'x' }, { pad: [4, 2] }]); onLang(draw);
})();
/* =====================================================================
   Pedal
   ===================================================================== */
(() => {
  const svg = $('#pedal'); if (!svg) return;
  const st = newState(); let cur = 1; let arm = false;
  const strip = $('#pedal-strip'), toast = $('#pedal-toast i');
  const cols = { empty: '#333', rec: C.rec, play: C.play, dub: C.dub, stop: null };
  for (let i = 0; i < 6; i++) { const e = document.createElement('span'); e.textContent = i + 1; strip.appendChild(e); }
  const showT = m => { toast.textContent = m; };
  function render() { $$('span', strip).forEach((e, i) => { const t = st.t[i]; e.style.background = t.st === 'stop' ? dim(i) : cols[t.st]; e.classList.toggle('sel', i + 1 === cur); }); $('#pedal-arm').textContent = arm ? 'ARM: ON' : 'ARM: off'; $('#pedal-arm').style.color = arm ? C.arm : ''; }
  const T = () => st.t[cur - 1];
  const A = {
    1() { const t = T(); t.st = t.st === 'empty' ? 'rec' : t.st === 'rec' ? 'play' : t.st === 'play' ? 'dub' : t.st === 'dub' ? 'play' : 'play'; showT(`TRACK ${cur} · REC/DUB/PLAY → ${t.st.toUpperCase()}`); },
    2() { const t = T(); t.st = t.st === 'play' || t.st === 'dub' ? 'stop' : t.st === 'rec' ? 'stop' : t.st === 'stop' ? 'play' : t.st; showT(`TRACK ${cur} · PLAY/STOP → ${t.st.toUpperCase()}`); },
    '2h'() { T().st = 'empty'; showT(t('pedal.clear', cur)); },
    3() { cur = (cur + 4) % 6 + 1; showT(`TRACK ${cur}`); },
    '3h'() { arm = !arm; showT(`ARM auto-rec ${arm ? 'ON' : 'OFF'}`); },
    4() { cur = cur % 6 + 1; showT(`TRACK ${cur}`); },
    5() { showT('UNDO'); },
    6() { const any = st.t.some(t => t.st === 'play' || t.st === 'dub'); st.t.forEach(t => { if (t.st !== 'empty') t.st = any ? 'stop' : 'play'; }); showT(any ? 'STOP ALL' : 'ALL START'); },
  };
  st.t[0].st = 'play'; st.t[1].st = 'play'; st.t[2].st = 'stop';
  const down = {}, timers = {}; const sws = {}; let manualUntil = 0;
  $$('.sw', svg).forEach(g => { sws[g.dataset.n] = g; });
  function press(n) { sws[n].classList.add('down'); down[n] = performance.now(); if ([1, 4, 5, 6].includes(+n)) A[n](); else { timers[n] = setTimeout(() => { down[n] = -1; A[n + 'h'](); render(); }, 1000); anim(n); } render(); }
  function anim(n) { const arc = sws[n].querySelector('.holdarc'); if (!arc) return; arc.style.transition = 'none'; arc.style.strokeDashoffset = 100; arc.style.strokeDasharray = 100; arc.getBoundingClientRect(); arc.style.transition = reduce ? 'none' : 'stroke-dashoffset 1s linear'; arc.style.strokeDashoffset = 0; arc.style.opacity = 1; }
  function release(n) { const g = sws[n]; g.classList.remove('down'); const arc = g.querySelector('.holdarc'); if (arc) { arc.style.transition = 'none'; arc.style.opacity = 0; } if (timers[n]) { clearTimeout(timers[n]); timers[n] = null; if (down[n] !== -1) A[n](); } down[n] = 0; render(); }
  Object.keys(sws).forEach(n => { const g = sws[n];
    g.addEventListener('pointerdown', e => { e.preventDefault(); manualUntil = performance.now() + 9000; auto = false; if (!down[n]) press(n); });
    ['pointerup', 'pointerleave', 'pointercancel'].forEach(ev => g.addEventListener(ev, () => { if (down[n]) release(n); }));
    g.addEventListener('keydown', e => { if ((e.key === 'Enter' || e.key === ' ') && !e.repeat) { e.preventDefault(); manualUntil = performance.now() + 9000; auto = false; press(n); } });
    g.addEventListener('keyup', e => { if (e.key === 'Enter' || e.key === ' ') release(n); }); });
  // automatic demo
  const seq = [[1, 0], [1, 0], [3, 0], [1, 0], [2, 0], [4, 0], [2, 1], [3, 1], [5, 0], [6, 0], [6, 0]];
  let auto = !reduce, si = 0, nxt = 1.2;
  reg(svg, (t) => { if (reduce) { render(); return; } if (!auto) { if (performance.now() > manualUntil && manualUntil) { auto = true; manualUntil = 0; nxt = t + 1; } return; } if (t < nxt) return;
    const [n, hold] = seq[si % seq.length]; si++; const ns = String(n);
    if (hold) { press(ns); nxt = t + 1.9; setTimeout(() => { if (down[ns]) release(ns); }, 1100); } else { press(ns); setTimeout(() => release(ns), 180); nxt = t + 1.5; } });
  render();
})();
/* =====================================================================
   Persistencia flow
   ===================================================================== */
(() => {
  const items = $$('#flow div'); if (!items.length) return; let i = 0;
  const set = k => items.forEach((e, j) => e.classList.toggle('on', j === k));
  items.forEach((e, j) => e.addEventListener('click', () => { i = j; set(j); }));
  set(0); if (reduce) return;
  reg($('#flow'), (t) => { const k = Math.floor(t / 1.7) % items.length; if (k !== i) { i = k; set(k); } });
})();
}
langPref;
LANG = langPref();
loadLang(LANG).then(() => { applyDOM(); document.documentElement.classList.remove('i18n-load'); main(); });
})();
