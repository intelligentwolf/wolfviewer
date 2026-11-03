// [VR PIPE 2026-11-03] Drives the WolfViewer headset page in headless Chromium with WolfStorm's fake WebXR device
// (firestorm repo wolfstorm/tools/fake_xr.js - the same emulator WolfStorm VR is tested with), so the page's REAL
// entry path, three r185's WebXRManager, the decoder and the pose loop all run without a headset.
//   node scripts/wolfvr/headset_emulator.mjs <page_url> <fake_xr.js> <out_dir> [seconds=30] [extra chromium flags...]
// Prints one JSON line of the page's numbers every 2 s and saves 2d.jpg / vr_*.jpg screenshots.
// The head turns 30 degrees/s left and right while presenting, so the reprojection and the pose loop are exercised.
// [VR PAIRING 2026-11-03] No code any more: the server asks Allow / Deny for this browser profile first (answer it in
// the server's terminal within the 10 s the page is given). e2e_pairing.mjs runs the whole pairing flow by itself.
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [URL_, FAKE, OUT, SECS = '30', ...extra] = process.argv.slice(2);
if (!URL_ || !FAKE || !OUT) { console.error('usage: headset_emulator.mjs <url> <fake_xr.js> <out_dir> [secs] [flags]'); process.exit(2); }
mkdirSync(OUT, { recursive: true });
const PORT = 9341;
const fake = readFileSync(FAKE, 'utf8');
const profile = resolve(OUT, 'chromium-profile');
const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    `--remote-debugging-port=${PORT}`, '--window-size=1280,1280', `--user-data-dir=${profile}`, ...extra, 'about:blank'],
    { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let targets = null;
for (let i = 0; i < 40 && !targets; i++) { try { targets = await (await fetch(`http://127.0.0.1:${PORT}/json`)).json(); } catch (e) { await sleep(250); } }
if (!targets) { console.error('no CDP'); chrome.kill(); process.exit(1); }
const ws = new WebSocket(targets.find(t => t.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map(); const events = [];
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); } else if (d.method) events.push(d); };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
const shot = async (name) => { const r = await send('Page.captureScreenshot', { format: 'jpeg', quality: 75 }); writeFileSync(resolve(OUT, name), Buffer.from(r.result.data, 'base64')); };
await send('Page.enable'); await send('Runtime.enable'); await send('Log.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: fake });
await send('Page.navigate', { url: URL_ });
let cfg = null;
for (let i = 0; i < 40 && !cfg; i++) { await sleep(250); cfg = await evalJs('window.__wolfvr && window.__wolfvr.config()'); }
console.log('secureContext:', await evalJs('window.isSecureContext'), ' xr:', await evalJs("typeof navigator.xr"), ' webcodecs:', await evalJs('typeof VideoDecoder'));
console.log('config:', JSON.stringify(cfg));
await sleep(3000);
await shot('2d.jpg');
console.log('2D numbers:', JSON.stringify(await evalJs('window.__wolfvr.numbers().rows')));
await evalJs('window.__wolfvr.toggleVR()');
console.log('presenting:', await evalJs('!!(window.__fakeXR && window.__fakeXR.session)'));
const t0 = Date.now();
const secs = Number(SECS);
let k = 0;
while (Date.now() - t0 < secs * 1000) {
    const t = (Date.now() - t0) / 1000;
    // sweep the head +-60 degrees
    const yaw = (Math.PI / 3) * Math.sin(t * 0.5);   // peak 30 deg/s
    await evalJs(`window.__fakeXR.setYaw(${yaw})`);
    await sleep(100);
    if (++k % 20 === 0) console.log(`+${t.toFixed(0)}s`, JSON.stringify(await evalJs('window.__wolfvr.numbers()')));
    if (k === 50) await shot('vr_5s.jpg');
}
await shot('vr_end.jpg');
const errs = events.filter(e => e.method === 'Runtime.exceptionThrown' || (e.method === 'Log.entryAdded' && e.params.entry.level === 'error')).slice(0, 12);
for (const e of errs) console.log('ERR', JSON.stringify(e.params).slice(0, 400));
await evalJs('window.__wolfvr.toggleVR()');
ws.close(); chrome.kill(); process.exit(0);
