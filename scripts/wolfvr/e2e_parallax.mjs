// [VR DEPTH 2026-11-04] Paul: "it just doesn't look 3d ... like its rendering the big screen and thinking its 3d".
// Proof that the eyes are now world-locked: against the REAL viewer (VR 3D Mode, in VR in headless Chromium with the
// fake WebXR device), the picture is HELD (no new one drawn) and the head moves 10 cm to the right. With the depth
// meshes a near thing shifts against a far one by f * 0.10 / z (real motion parallax); with the old flat cards nothing
// shifts against anything (the whole picture moves with the head). Saves the headset views (both eyes side by side,
// as the fake device draws them into the page) for each case: p_depth_0.png / p_depth_10cm.png / p_cards_0.png /
// p_cards_10cm.png, and prints the depth the page has for the near and far bands (analysed by parallax_check.py).
//   node e2e_parallax.mjs <lan_url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [LAN, OUT, FAKE, CTL] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--remote-debugging-port=9388', '--window-size=1280,720',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
process.on('exit', () => { try { chrome.kill(); } catch (e) { /* gone */ } });
process.on('uncaughtException', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
process.on('unhandledRejection', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9388/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => { const r = await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true }); return r.result?.exceptionDetails ? 'EXCEPTION ' + (r.result.exceptionDetails.exception?.description || r.result.exceptionDetails.text) : r.result?.result?.value; };
// the canvas only (the fake device draws both eyes into it), wherever it is on the page
const shot = async (name) => {
    // in view (an element below the fold is not drawn again, so two shots of it are the same picture)
    const rc = await evalJs("(() => { const c = document.querySelector('canvas'); c.scrollIntoView({ block: 'center' }); const r = c.getBoundingClientRect(); return [r.left + window.scrollX, r.top + window.scrollY, r.width, r.height]; })()");
    await sleep(300);
    const r = await send('Page.captureScreenshot', { format: 'png', clip: { x: rc[0], y: rc[1], width: rc[2], height: rc[3], scale: 1 } });
    writeFileSync(resolve(OUT, name), Buffer.from(r.result.data, 'base64'));
};

await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: LAN });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'connected');
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('window.__wolfvr.depth().frames > 5'), 30000), 'pictures with depth arrive and are drawn as world-locked meshes');
console.log('   last picture:', JSON.stringify(await evalJs('(() => { const e = window.__wolfvr.eyes(); return e && e.hdr; })()')), JSON.stringify(await evalJs('window.__wolfvr.depth()')));
await evalJs('window.__fakeXR.setPitch(-0.3)');
await sleep(5000);
const dep = await evalJs('window.__wolfvr.depth()');
console.log('   depth:', JSON.stringify(dep));
console.log('   eyes:', JSON.stringify(await evalJs('(() => { const e = window.__wolfvr.eyes(); return { tan: e.tan, views: e.views && e.views.map((v) => v.proj.slice(0, 1)) }; })()')));

// the left eye's view drawn off-screen by the page itself (its own scene and the left XRView's projection and pose):
// the same picture held, the head at 0 and 10 cm to the right (a screenshot of the canvas does not refresh in VR)
const eyeShot = async (name, dx) => { const d = await evalJs(`window.__wolfvr.renderEye(${dx})`); if (d) writeFileSync(resolve(OUT, name), Buffer.from(d.split(',')[1], 'base64')); return !!d; };
async function pair(tag) {
    await evalJs('window.__wolfvr.freeze(true)'); await sleep(600);
    check(await eyeShot(`p_${tag}_0.png`, 0), `${tag}: the left eye's view, head at 0`);
    await eyeShot(`p_${tag}_10cm.png`, 0.10);
    await evalJs('window.__wolfvr.freeze(false)'); await sleep(1500);
}
const rect = await evalJs("(() => { const r = document.querySelector('canvas').getBoundingClientRect(); return [r.left, r.top, r.width, r.height, devicePixelRatio]; })()");
console.log('   canvas:', JSON.stringify(rect));

writeFileSync(resolve(OUT, 'canvas.json'), JSON.stringify({ rect: [0, 0, 960, 432, 1], depth: dep }));   // the shots are one eye, 480 x 432
await pair('depth');
await evalJs('window.__wolfvr.cards(true)'); await sleep(2500);
check(!(await evalJs('window.__wolfvr.depth().active')), 'the old flat cards for comparison');
await pair('cards');
await evalJs('window.__wolfvr.cards(false)');
await evalJs('window.__wolfvr.toggleVR()');
ws.close();
console.log(failures ? `E2E PARALLAX FAILED (${failures})` : 'E2E PARALLAX CAPTURED');
process.exit(failures ? 1 : 0);
