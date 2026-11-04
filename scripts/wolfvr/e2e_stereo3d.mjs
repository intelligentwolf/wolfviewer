// [VR 3D CHECK 2026-11-04] Paul: "it still doesn't really look 3D ... its like flat". Against the REAL viewer, in headless
// Chromium with the fake headset: first with fake_xr.js's symmetric eyes, then with a Quest 3's own asymmetric, mirrored
// eye windows (window.__asymEyes, fake_xr_controllers.js; the windows from Paul's own device line). For each, the page's
// device report (the disparity in the stereo picture it decoded, the eye windows' own offset taken out: 0 at infinity,
// positive for near things) and the headset view of each eye (renderEye: left; renderEye with the right view: right).
//   node e2e_stereo3d.mjs <url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [URL_, OUT, FAKE, CTL] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }
const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--remote-debugging-port=9394', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
process.on('exit', () => { try { chrome.kill('SIGKILL'); } catch (e) { /* gone */ } });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9394/json')).json(); } catch (e) { await sleep(250); } }
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
await evalJs('window.__wolfvr.setQuality(1)');   // Paul's 1728 px eyes
await evalJs('window.__fakeXR.setPitch(-0.35)');  // look a little down: near ground in the lower band, the distance above
const parse = (line) => {
    const m = /near (-?\d+) px, middle (-?\d+) px, far (-?\d+) px/.exec(line || '');
    return m ? { near: +m[1], middle: +m[2], far: +m[3] } : null;
};
for (const [tag, asym] of [['symmetric', null], ['quest3', [[-1.376, 0.839, -1.43, 0.96], [-0.839, 1.376, -1.43, 0.96]]]]) {
    await evalJs(`window.__asymEyes = ${JSON.stringify(asym)}`);
    await sleep(9000);
    const line = await evalJs('window.__wolfvr.deviceLine()');
    const d = parse(line);
    console.log(`   ${tag}: ` + (/disparity[^;]*;/.exec(line) || [''])[0]);
    console.log(`   ${tag}: ` + (/eye windows .*$/.exec(line) || [''])[0]);
    check(!!d && Math.abs(d.far) <= 12, `${tag}: no disparity at infinity once the windows' own offset is out (far ${d && d.far} px)`);
    check(!!d && d.near >= 15, `${tag}: near things have real disparity (near ${d && d.near} px)`);
    const l = await evalJs('window.__wolfvr.renderEye(0)');
    if (l) writeFileSync(resolve(OUT, `eye_${tag}_left.png`), Buffer.from(l.split(',')[1], 'base64'));
}
console.log(failures ? `${failures} FAILED` : 'ALL PASSED');
chrome.kill('SIGKILL');
process.exit(failures ? 1 : 0);
