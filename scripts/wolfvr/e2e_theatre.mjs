// [VR STRETCH / THEATRE 2026-11-05] Paul: "we didnt build the theatre or stretching for vr screen mode". Big Screen mode
// against the test server (WOLFVR_MODE=screen), headless Chromium with the fake WebXR device and controllers:
//   1. in VR the theatre room is round the screen
//   2. pointing at the screen shows the four corner handles
//   3. a corner handle pulled outwards with the trigger makes the screen wider, smoothly, and no click reaches the viewer
//   4. the new width is remembered (localStorage)
//   5. the Theatre button under the screen turns the room off and on again
//   node scripts/wolfvr/e2e_theatre.mjs <selftest_bin> <web_root> <fake_xr.js> <fake_xr_controllers.js> <out> [port=4356]
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { resolve } from 'node:path';

const [BIN, WEBROOT, FAKE, CTL, OUT, PORT = '4356'] = process.argv.slice(2);
if (!BIN || !WEBROOT || !FAKE || !CTL || !OUT) { console.error('usage: e2e_bigscreen.mjs <bin> <web_root> <fake_xr.js> <ctl.js> <out> [port]'); process.exit(2); }
if (Number(PORT) >= 4333 && Number(PORT) <= 4340) { console.error('ports 4333-4340 are for Paul'); process.exit(2); }
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 10000, step = 150) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); }
    return null;
}

const srv = spawn(BIN, ['serve', WEBROOT, '300', 'auto'], {
    env: { ...process.env, WOLFVR_PORT: PORT, WOLFVR_STATS: '1', WOLFVR_MODE: 'screen', WOLFVR_STATE_DIR: resolve(OUT, 'state') },
    stdio: ['pipe', 'pipe', 'pipe'] });
const lines = [];
let lan = '';
let buf = '';
srv.stdout.on('data', (d) => {
    buf += d.toString();
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
        const l = buf.slice(0, i); buf = buf.slice(i + 1);
        lines.push(l);
        if (l.startsWith('LAN_URL ')) lan = l.slice(8).trim();
        if (l.startsWith('ASK ')) srv.stdin.write('a\n');   // this test is about the screen, not the question
    }
});
srv.stderr.on('data', () => {});
const ins = () => lines.filter(l => l.startsWith('IN ')).map(l => {
    const o = { type: l.split(' ')[1] };
    for (const m of l.matchAll(/(\w+)=(\S*)/g)) o[m[1]] = m[2];
    return o;
});
let mark = 0;
const since = () => { const a = ins().slice(mark); return a; };
const next = () => { mark = ins().length; };

await waitFor(() => lan);
const port = 9362;
const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', `--remote-debugging-port=${port}`, '--window-size=1280,1600',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
let targets = null;
for (let i = 0; i < 40 && !targets; i++) { try { targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(targets.find(t => t.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map(); const events = [];
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); } else if (d.method) events.push(d); };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => (await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
const shot = async (f) => { const r = await send('Page.captureScreenshot', { format: 'jpeg', quality: 70 }); writeFileSync(resolve(OUT, f), Buffer.from(r.result.data, 'base64')); };
await send('Page.enable'); await send('Runtime.enable'); await send('Log.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: lan });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.config() && window.__wolfvr.config().mode === "screen")'), 15000), 'page in Big Screen mode');
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('window.__wolfvr.big().visible'), 15000), 'in VR: the big screen shows the picture');
await sleep(1500);
check(await waitFor(() => evalJs('window.__wolfvr.big().theatre'), 5000), '1. the theatre room is round the screen');
const RIGHT = [0.2, 1.4, -0.2];   // fake_xr_controllers.js: where the right controller is
async function aimAt(p) {
    const dx = p[0] - RIGHT[0], dy = p[1] - RIGHT[1], dz = p[2] - RIGHT[2];
    const n = Math.hypot(dx, dy, dz);
    await evalJs(`window.__fakeCtl.aim('right', ${Math.atan2(-dx / n, -dz / n)}, ${Math.asin(dy / n)})`);
}
const shotEye = async (f) => { const d = await evalJs('window.__wolfvr.renderEye(0)'); if (d) writeFileSync(resolve(OUT, f), Buffer.from(d.split(',')[1], 'base64')); };
const k0 = await evalJs('window.__wolfvr.knobPoint(0)'), k3 = await evalJs('window.__wolfvr.knobPoint(3)');
const centre = [(k0[0] + k3[0]) / 2, (k0[1] + k3[1]) / 2, (k0[2] + k3[2]) / 2];
await aimAt(centre); await sleep(500);
check(await waitFor(async () => (await evalJs('window.__wolfvr.big().knobs')).every(Boolean), 3000), '2. pointing at the screen shows the four corner handles');
await shotEye('eye_theatre_on.png');
// the room itself: looking left, and round behind (the rows stepping up)
for (const [yaw, f] of [[1.2, 'eye_theatre_left.png'], [2.6, 'eye_theatre_behind.png']]) {
    await evalJs(`window.__fakeXR.setYaw(${yaw})`); await sleep(800); await shotEye(f);
}
await evalJs('window.__fakeXR.setYaw(0)'); await sleep(800);
const w0 = await evalJs('window.__wolfvr.big().size');
next();
await aimAt(await evalJs('window.__wolfvr.knobPoint(3)')); await sleep(400);
await evalJs("window.__fakeCtl.press('right', 0, true)"); await sleep(300);
check(await evalJs('window.__wolfvr.big().drag'), '3. the trigger on the top-right handle takes hold of it');
const kp = await evalJs('window.__wolfvr.knobPoint(3)');
for (let k = 1; k <= 5; k++) { await aimAt([kp[0] + k * 0.12, kp[1], kp[2]]); await sleep(150); }
const wMid = await evalJs('window.__wolfvr.big().size');
await evalJs("window.__fakeCtl.press('right', 0, false)"); await sleep(400);
const w1 = await evalJs('window.__wolfvr.big().size');
check(w1 > w0 + 0.3 && Math.abs(w1 - wMid) < 0.05, `3. pulled outwards the screen gets wider (${w0.toFixed(2)} -> ${w1.toFixed(2)} m)`);
check(!since().some((e) => e.type === 'down' || e.type === 'up'), '3. ... and no click reached the viewer while the handle was held');
check(!(await evalJs('window.__wolfvr.big().drag')), '3. let go: the handle is free again');
check(Math.abs(Number(await evalJs("localStorage.getItem('wolfvr.screenWidth')")) - w1) < 0.01, '4. the new width is remembered');
await aimAt(centre); await sleep(500);
await aimAt(await evalJs('window.__wolfvr.theatreBtnPoint()')); await sleep(400);
await evalJs("window.__fakeCtl.press('right', 0, true)"); await sleep(200); await evalJs("window.__fakeCtl.press('right', 0, false)"); await sleep(500);
check(await waitFor(async () => !(await evalJs('window.__wolfvr.big().theatre')), 3000), '5. the Theatre button turns the room off');
await shotEye('eye_theatre_off.png');
await evalJs("window.__fakeCtl.press('right', 0, true)"); await sleep(200); await evalJs("window.__fakeCtl.press('right', 0, false)"); await sleep(500);
check(await waitFor(() => evalJs('window.__wolfvr.big().theatre'), 3000), '5. ... and on again');
console.log(failures ? `${failures} FAILED` : 'ALL PASSED');
chrome.kill('SIGKILL'); srv.kill('SIGKILL');
process.exit(failures ? 1 : 0);
