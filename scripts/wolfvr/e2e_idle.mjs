// [VR IMMERSIVE 2026-11-04] Paul's PC ran at 0 fps after log-in: an open headset tab reconnected by itself and the
// viewer paused its desktop for a headset that was not in VR. Against the REAL viewer (VR switched on in this session,
// WolfVRStreamMode "world"), headless Chromium with WolfStorm's fake WebXR device:
//   1. a connected tab NOT in VR gets no pictures (nothing is drawn for it, the desktop is not paused)
//   2. Enter VR: pictures come; Exit VR: they stop at once
//   3. the tab reloads (reconnects by itself) and stays out of VR: still no pictures
//   4. in VR, the tab goes away without a word: the viewer is told by the closed connection (its log), and
//   5. in VR, the page hidden (headset asleep / tab in the background): pictures stop
// The viewer log (WolfVRDebugStats) is checked by the caller for "desktop paused" only inside the VR phases.
//   node e2e_idle.mjs <lan_url> <out_dir> <fake_xr.js>
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [LAN, OUT, FAKE] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }
const stamp = (what) => console.log(`   ${new Date().toISOString().slice(11, 19)}Z ${what}`);

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--remote-debugging-port=9387', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
// never leave the browser behind (a stray tab would keep reconnecting to the next test viewer)
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
// connected = the viewer said welcome (the page then says whether it is in VR); [VR IMMERSIVE] the picture's
// "config" only comes once the headset is in VR
const connected = () => waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000);

await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') });
await send('Page.navigate', { url: LAN });
check(await connected(), 'the tab connects to the real viewer');
stamp('phase 1: connected, not in VR');
await sleep(8000);
check((await pics()) === 0, 'a connected tab that is NOT in VR gets no pictures (' + (await pics()) + ')');

stamp('phase 2: Enter VR');
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(async () => (await pics()) > 10, 20000), 'in VR the pictures come (' + (await pics()) + ')');
await sleep(4000);
stamp('phase 2: Exit VR');
await evalJs('window.__wolfvr.toggleVR()');
await sleep(1500);
const p1 = await pics();
await sleep(5000);
const p2 = await pics();
check(p2 - p1 <= 2, `out of VR they stop at once (${p2 - p1} more in 5 s)`);

stamp('phase 3: the tab reloads by itself, stays out of VR');
await send('Page.reload', { ignoreCache: false });
check(await connected(), 'the reloaded tab reconnects');
await sleep(8000);
check((await pics()) === 0, 'the reconnected tab, not in VR, gets no pictures (' + (await pics()) + ')');

stamp('phase 5: in VR, then the page hidden (headset asleep)');
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(async () => (await pics()) > 10, 20000), 'in VR again: pictures');
await send('Emulation.setFocusEmulationEnabled', { enabled: false });
await evalJs("Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => 'hidden' }); document.dispatchEvent(new Event('visibilitychange'))");
await sleep(1500);
const h1 = await pics();
await sleep(5000);
const h2 = await pics();
check(h2 - h1 <= 2, `page hidden: pictures stop (${h2 - h1} more in 5 s); the page said ${JSON.stringify(await evalJs('window.__wolfvr.xr()'))}`);
await evalJs("Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => 'visible' }); document.dispatchEvent(new Event('visibilitychange'))");
check(await waitFor(async () => (await pics()) > h2 + 10, 15000), 'shown again: pictures again');

stamp('phase 4: in VR, the tab goes away without a word');
await send('Page.navigate', { url: 'about:blank' });
await sleep(6000);
stamp('end');
ws.close(); chrome.kill();
console.log(failures ? `E2E IDLE FAILED (${failures})` : 'E2E IDLE PASSED');
process.exit(failures ? 1 : 0);
