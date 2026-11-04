// [VR SIZE SWITCH 2026-11-04] Ticket 526: Paul's viewer crashed (SIGSEGV in memcpy under gst_buffer_fill) the moment
// the headset switched from VR 3D Mode (3456 x 1728) to Big Screen (1920 x 1080): a picture of the new size reached
// the encoder still open at the old size. Against the REAL viewer, in headless Chromium with the fake headset: switch
// between the two modes many times (the left stick click, as Paul does), at varying moments, and check that the
// viewer keeps streaming after every switch - pictures keep arriving in the mode switched to. A viewer that crashes
// stops answering, and the page's picture count stops growing.
//   node e2e_modeswitch.mjs <url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [URL_, OUT, FAKE, CTL] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }
const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--remote-debugging-port=9396', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
process.on('exit', () => { try { chrome.kill('SIGKILL'); } catch (e) { /* gone */ } });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9396/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => { const r = await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true }); return r.result?.result?.value; };
await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: URL_ });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'connected');
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "world")'), 30000), 'VR 3D Mode');
await evalJs('window.__wolfvr.setQuality(1)');   // Paul's 1728 px eyes: 3456 x 1728 against Big Screen's 1920 x 1080
await sleep(6000);
const press = async (hand, i, down) => evalJs(`window.__fakeCtl.press('${hand}', ${i}, ${down})`);
const pictures = () => evalJs('window.__wolfvr.xr().pictures');
const mode = () => evalJs('window.__wolfvr.config() && window.__wolfvr.config().mode');
const N = 24;
let ok = 0;
for (let k = 0; k < N; k++) {
    const want = (await mode()) === 'world' ? 'screen' : 'world';
    await press('left', 3, true); await sleep(120); await press('left', 3, false);
    // a varying wait, so the switch lands at different moments of the viewer's frame and the encoder's wait
    const switched = await waitFor(async () => (await mode()) === want, 12000, 100);
    const p0 = await pictures();
    const flowing = await waitFor(async () => (await pictures()) >= p0 + 5, 12000, 100);
    if (switched && flowing) ok++;
    else console.log(`   switch ${k + 1} to ${want}: ${switched ? 'switched' : 'did NOT switch'}, ${flowing ? 'pictures flowing' : 'pictures STOPPED'} (at ${p0})`);
    await sleep(150 + (k * 137) % 900);
}
check(ok === N, `${ok} of ${N} switches between VR 3D Mode and Big Screen, pictures flowing after each`);
console.log(failures ? `${failures} FAILED` : 'ALL PASSED');
chrome.kill('SIGKILL');
process.exit(failures ? 1 : 0);
