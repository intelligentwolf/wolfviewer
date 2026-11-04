// [VR STAGES 2026-11-04] Where the time goes, against the REAL viewer (logged in, WolfVRStreamMode "world", the stream on
// <url>, LEAP relay FIFO at ~/.cache/skyband/leap.fifo): headless Chromium with WolfStorm's fake WebXR device enters VR,
// the head sweeps left and right, and for each phase (viewer settings set through LEAP first) the page's own stage
// breakdown (wolfvr.js noteStages: pose up, wait for a frame, left eye, right eye, read-back, queue, encode, pack,
// down, decode, shown) is printed as one JSON line, with pictures in / decoded per second.
//   node e2e_stages.mjs <url> <out_dir> <fake_xr.js> <phase> [<phase> ...]
//   phase = label:Key=value,Key=value:seconds   (values as LEAP notation: true, false, 1.5, 'text')
// Not shipped to residents.
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, appendFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { homedir } from 'node:os';

const [URL_, OUT, FAKE, ...PHASES] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const H = resolve(homedir(), '.cache/skyband');
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const setting = (k, v) => appendFileSync(resolve(H, 'leap.fifo'), `LLViewerControl\t{'op':'set','group':'Global','key':'${k}','value':${v}}\n`);

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--autoplay-policy=no-user-gesture-required', '--remote-debugging-port=9391', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
process.on('exit', () => { try { chrome.kill('SIGKILL'); } catch (e) { /* gone */ } });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9391/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => (await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') });
await send('Page.navigate', { url: URL_ });
let cfg = null;
for (let i = 0; i < 80 && !cfg; i++) { await sleep(250); cfg = await evalJs('window.__wolfvr && window.__wolfvr.config()'); }
console.log('config', JSON.stringify(cfg));
await evalJs('window.__wolfvr.toggleVR()');
await sleep(4000);
const t0 = Date.now();
const results = [];
for (const ph of PHASES) {
    const [label, kv, secs] = ph.split(':');
    for (const pair of (kv || '').split(',').filter(Boolean)) {
        const [k, v] = pair.split('=');
        if (k === 'q') await evalJs(`window.__wolfvr.setQuality(${Number(v)})`);   // the page's quality step (eye size)
        else setting(k, v);
    }
    await sleep(6000);   // settle: the new picture size, the encoder restarted, old pictures out of the window
    await evalJs('window.__wolfvr.resetStages && window.__wolfvr.resetStages()');
    const end = Date.now() + Number(secs || 20) * 1000;
    while (Date.now() < end) {
        const s = (Date.now() - t0) / 1000;
        await evalJs(`window.__fakeXR.setYaw(${(Math.PI / 4) * Math.sin(s * 0.6)})`);
        await sleep(100);
    }
    const st = await evalJs('window.__wolfvr.stages()');
    const nums = await evalJs('window.__wolfvr.rates()');
    const r = { label, settings: kv, rates: nums, stages: st };
    results.push(r);
    console.log(JSON.stringify(r));
}
writeFileSync(resolve(OUT, 'stages.json'), JSON.stringify(results, null, 1));
await evalJs('window.__wolfvr.toggleVR()');
await sleep(1000);
chrome.kill('SIGKILL');
process.exit(0);
