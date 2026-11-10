// [VR BIG SCREEN VULKAN 2026-11-09] Big Screen against the REAL viewer (WolfVRStreamMode "screen", stream on <lan_url>,
// LEAP relay FIFO at ~/.cache/skyband/leap.fifo), in headless Chromium with WolfStorm's fake WebXR device and the
// Touch Plus controllers (fake_xr_controllers.js), as e2e_vr3d.mjs does. In Vulkan mode the headset used to get the
// interface without the world (docs/VULKAN_BACKEND.md 8.2i); wolfvrscreen.cpp now rebuilds the window's composite for
// it (VkBigScreen). Checks, in either renderer (the chain runs it in both and compares the pictures afterwards with
// vrbigcheck.py):
//   1. the curved screen gets pictures, in Big Screen mode (config().mode === "screen")
//   2. the world is in them: the share of pixels that are exactly black (luminance <= 3 of 255) - what the cleared
//      layer under the interface is in Vulkan mode - stays under 30% (a lit world has almost none)
//   3. the same after the desktop window is resized narrower and back (the campaign resizes it on request:
//      resize_req_<n> -> resize_done_<n> files in <out_dir>)
// Pictures: big_<step>.jpg in <out_dir>, as the headset received them (window.__wolfvr.grab()).
//   node e2e_vrbig.mjs <lan_url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync, existsSync } from 'node:fs';
import { resolve } from 'node:path';

const [LAN, OUT, FAKE, CTL] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }

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
let id = 0; const pend = new Map(); const events = [];
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } else if (d.method) events.push(d); };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => { const r = await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true }); return r.result?.result?.value; };

// The picture the headset shows now, and the share of exactly-black pixels in the window's own rectangle. The
// letterbox bars are black too and the viewer sends no rectangle, so rows and columns that are black end to end are
// taken as bars and left out.
const MEASURE_INNER = `(async () => {
    const g = window.__wolfvr.grab(); if (!g) return null;
    const img = new Image(); img.src = g; await img.decode();
    const c = document.createElement('canvas'); c.width = img.width; c.height = img.height;
    const x = c.getContext('2d'); x.drawImage(img, 0, 0);
    const W = img.width, H = img.height, d = x.getImageData(0, 0, W, H).data;
    const lum = (i) => d[i] * 0.3 + d[i + 1] * 0.59 + d[i + 2] * 0.11;
    const rowBlack = (y) => { for (let px = 0; px < W; px += 4) if (lum((y * W + px) * 4) > 3) return false; return true; };
    const colBlack = (px) => { for (let y = 0; y < H; y += 4) if (lum((y * W + px) * 4) > 3) return false; return true; };
    let y0 = 0, y1 = H - 1, x0 = 0, x1 = W - 1;
    while (y0 < y1 && rowBlack(y0)) y0++;
    while (y1 > y0 && rowBlack(y1)) y1--;
    while (x0 < x1 && colBlack(x0)) x0++;
    while (x1 > x0 && colBlack(x1)) x1--;
    let black = 0, n = 0;
    for (let y = y0; y <= y1; y += 2) for (let px = x0; px <= x1; px += 2) { if (lum((y * W + px) * 4) <= 3) black++; n++; }
    return { data: g, w: W, h: H, black: n ? black / n : 1, rect: [x0, y0, x1, y1] };
})()`;
async function shotInner(tag) {
    const m = await evalJs(MEASURE_INNER);
    if (!m) { check(false, `${tag}: a picture to grab`); return null; }
    writeFileSync(resolve(OUT, `big_${tag}.jpg`), Buffer.from(m.data.split(',')[1], 'base64'));
    console.log(`   ${tag}: ${m.w}x${m.h}, window area ${JSON.stringify(m.rect)}, exactly black ${(m.black * 100).toFixed(1)}%`);
    return m;
}

await send('Page.enable'); await send('Runtime.enable'); await send('Log.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: LAN });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'connected to the real viewer');
await evalJs("try { localStorage.setItem('wolfvr.headset', 'auto'); } catch (e) {}");
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!window.__fakeXR.session')), 'Enter VR');
check(await waitFor(() => evalJs('!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "screen")'), 30000), 'in VR: Big Screen from the real viewer');
check(!!(await waitFor(() => evalJs('!!window.__wolfvr.grab()'), 20000)), 'Big Screen pictures arrive');
await sleep(8000);   // a few seconds of pictures: the stream's first ones can predate the world

for (let k = 0; k < 3; k++) {
    const m = await shotInner(`start_${k}`);
    if (m) check(m.black < 0.30, `start ${k}: the world is in the picture (exactly black ${(m.black * 100).toFixed(1)}% < 30%)`);
    await sleep(4000);
}
// window resizes, done by the campaign (it owns the window)
for (const n of [1, 2]) {
    writeFileSync(resolve(OUT, `resize_req_${n}`), '');
    const done = await waitFor(() => existsSync(resolve(OUT, `resize_done_${n}`)), 60000, 500);
    check(!!done, `resize ${n} done by the campaign`);
    await sleep(6000);
    const m = await shotInner(`resize${n}`);
    if (m) check(m.black < 0.30, `after resize ${n}: the world is in the picture (exactly black ${(m.black * 100).toFixed(1)}% < 30%)`);
}

const errs = events.filter(e => e.method === 'Runtime.exceptionThrown' || (e.method === 'Log.entryAdded' && e.params.entry.level === 'error' && e.params.entry.source !== 'network')).slice(0, 8);
for (const e of errs) console.log('ERR', JSON.stringify(e.params).slice(0, 400));
check(errs.length === 0, 'no page errors');
await evalJs('window.__wolfvr.toggleVR()');
ws.close(); chrome.kill();
console.log(failures ? `E2E VRBIG FAILED (${failures})` : 'E2E VRBIG PASSED');
process.exit(failures ? 1 : 0);
