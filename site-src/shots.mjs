import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import { fileURLToPath, pathToFileURL } from 'node:url';
const PAGE = pathToFileURL(fileURLToPath(new URL('../site/index.html', import.meta.url))).href;
const OUT = process.argv[2];
const langs = (process.argv[3] || 'es').split(',');
const widths = (process.argv[4] || '1400,390').split(',').map(Number);
const targets = (process.argv[5] || 'top,nav,col1,col8,pasos,undo').split(',');
fs.mkdirSync(OUT, { recursive: true });
const chrome = spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', ['--headless=new', '--remote-debugging-port=9333', '--no-first-run', '--hide-scrollbars', '--user-data-dir=' + os.tmpdir() + '/chrome-prof-' + Date.now(), 'about:blank'], { stdio: 'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));
let ws, id = 0; const pend = {}; const errors = [];
async function connect() {
  for (let i = 0; i < 50; i++) { try { const r = await fetch('http://127.0.0.1:9333/json'); const l = await r.json(); const p = l.find(x => x.type === 'page'); if (p) { ws = new WebSocket(p.webSocketDebuggerUrl); await new Promise(res => ws.onopen = res); return; } } catch (e) { } await sleep(300); }
  throw new Error('no chrome');
}
const send = (method, params = {}) => new Promise(res => { const i = ++id; pend[i] = res; ws.send(JSON.stringify({ id: i, method, params })); });
await connect();
ws.onmessage = m => { const d = JSON.parse(m.data); if (d.id && pend[d.id]) { pend[d.id](d.result || d.error); delete pend[d.id]; } else if (d.method === 'Runtime.exceptionThrown') errors.push('EXC ' + JSON.stringify(d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text)); else if (d.method === 'Runtime.consoleAPICalled' && d.params.type === 'error') errors.push('CONSOLE ' + d.params.args.map(a => a.value || a.description).join(' ')); else if (d.method === 'Log.entryAdded' && d.params.entry.level === 'error') errors.push('LOG ' + d.params.entry.text + ' ' + (d.params.entry.url || '')); };
await send('Runtime.enable'); await send('Log.enable'); await send('Page.enable');
const ev = async expr => (await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true })).result?.value;
for (const w of widths) for (const lang of langs) {
  const h = w > 600 ? 900 : 844;
  await send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: w < 600 });
  errors.length = 0;
  await send('Page.navigate', { url: PAGE + '?lang=' + lang });
  await sleep(2200);
  const info = await ev('JSON.stringify({sw:document.documentElement.scrollWidth,cw:document.documentElement.clientWidth,lang:document.documentElement.lang,title:document.title})');
  console.log(lang, w, info, 'errors:', errors.length ? errors.slice(0, 5) : 'none');
  for (const tg of targets) {
    if (tg === 'nav') {
      if (w < 1100) { await ev("window.scrollTo(0,0);document.getElementById('menu-btn').click()"); await sleep(300); }
      else await ev("window.scrollTo(0,document.getElementById('col1').offsetTop)");
      await sleep(500);
    } else if (tg === 'top') { await ev('window.scrollTo(0,0)'); await sleep(400); }
    else { const [id, off] = tg.split('@'); await ev(`(()=>{const e=document.getElementById('${id}');window.scrollTo(0,e.offsetTop-40+${+off || 0});return 1})()`); await sleep(1300); }
    const r = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${OUT}/${lang}-${w}-${tg.replace('@','_')}.png`, Buffer.from(r.data, 'base64'));
    if (tg === 'nav' && w < 1100) await ev("document.getElementById('menu-btn').click()");
  }
}
chrome.kill(); process.exit(0);
