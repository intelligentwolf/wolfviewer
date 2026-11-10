// [VR 3D POOL 2026-11-09] Soak: the fake headset (WolfStorm's fake WebXR device + Touch Plus controllers) stays in VR
// against the REAL viewer for <minutes>, whatever the viewer's WolfVRStreamMode is (world = VR 3D Mode, screen = Big
// Screen). Every 30 s the pictures received so far are read; they must keep coming (the VR 3D Mode crash of 2026-11-09
// stopped the viewer within ~3 s: a Vulkan frame slot never closed while the desktop was paused).
//   node e2e_vrsoak.mjs <lan_url> <out_dir> <fake_xr.js> <fake_xr_controllers.js> <minutes>
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [LAN, OUT, FAKE, CTL, MIN] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const minutes = Number(MIN || 10);
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }
const stamp = (what) => console.log(`   ${new Date().toISOString().slice(11, 19)}Z ${what}`);

// the same headless browser e2e_idle.mjs starts (its own profile in the shots folder)
const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--remote-debugging-port=9387', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
process.on('exit', () => { try { chrome.kill(); } catch (e) { /* gone */ } });
process.on('uncaughtException', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
process.on('unhandledRejection', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9387/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => (await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
const pics = async () => (await evalJs('window.__wolfvr && window.__wolfvr.xr ? window.__wolfvr.xr().pictures : -1'));
const connected = () => waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000);

await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') });
if (CTL) await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: LAN });
check(await connected(), 'the tab connects to the real viewer');
stamp('Enter VR');
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(async () => (await pics()) > 10, 30000), 'in VR the pictures come (' + (await pics()) + ')');
const t0 = Date.now();
let last = await pics(), stalls = 0;
while (Date.now() - t0 < minutes * 60000) {
    await sleep(30000);
    const now = await pics();
    stamp(`pictures ${now} (+${now - last} in 30 s)`);
    if (!(now > last)) stalls++;
    last = now;
}
check(stalls === 0, `the pictures kept coming for ${minutes} min (${stalls} 30-s spells without one; ${last} in all)`);
await evalJs('window.__wolfvr.toggleVR()');
stamp('Exit VR');
await sleep(2000);
ws.close(); chrome.kill();
console.log(failures ? `E2E VRSOAK FAILED (${failures})` : 'E2E VRSOAK PASSED');
process.exit(failures ? 1 : 0);
