// [VR STRETCH / THEATRE 2026-11-05] Paul: "we didnt build the theatre or stretching for vr screen mode". Big Screen mode
// against the test server (WOLFVR_MODE=screen), headless Chromium with the fake WebXR device and controllers:
//   1. in VR the theatre room is round the screen
//   2. pointing at the screen shows the four corner handles
//   3. a corner handle pulled outwards with the trigger makes the screen wider, smoothly, and no click reaches the viewer
//   4. the new width is remembered (localStorage)
//   5. the Theatre button under the screen turns the room off and on again
// [Wolf 2026-11-06] Paul could not drag the handles in the headset (they hid as the pointer reached them; this test
// jumped straight onto them, so it never saw it). Now, for the curved AND the flat screen:
//   6. the aim SWEEPS in 1 cm steps along the screen from its centre out to each corner handle and down to each button
//      under it: the controls stay shown every step, the one under the ray lights up, and the viewer's mouse never
//      leaves the picture (no move outside 0..1)
//   7. each corner handle reached that way can be pulled, without a jump when it is taken
//   8. the "Screen: Curved / Flat" button makes the screen flat and back, remembered; the theatre room holds it
//   9. "Nearer" / "Further" move the screen, within limits, remembered; the theatre room holds it near and far
//  10. the headset the browser names comes before WolfViewer's saved choice; a choice on the headset page still wins
//  11. both controllers are drawn (our own simple shapes); the clicks and drags above all ran with them shown
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
// [Wolf 2026-11-06] 11. both controllers are drawn, where the controllers are
{
    const c = await evalJs('window.__wolfvr.controllers()');
    const near = (p, q) => Math.hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]) < 0.02;
    check(c && c.left.visible && c.right.visible && near(c.right.at, RIGHT), `11. both controllers are drawn in the headset (right at ${c && c.right.at.map((v) => v.toFixed(2)).join(',')})`);
}
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
// ---------------------------------------------------------------- [Wolf 2026-11-06] 6-9
const frames = () => evalJs('new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(() => r(1))))');
const big = () => evalJs('window.__wolfvr.big()');
// sweep the aim along the screen's surface from (0, 0) to (x, y) metres, 1 cm a step; true if every step the controls
// showed and the viewer's mouse stayed on the picture
async function sweep(x, y, what) {
    const n = Math.max(1, Math.ceil(Math.hypot(x, y) / 0.01));
    let hidden = 0;
    next();
    for (let k = 0; k <= n; k++) {
        await aimAt(await evalJs(`window.__wolfvr.screenPoint(${x * k / n}, ${y * k / n})`));
        await frames();
        const b = await big();
        if (!b.knobs.every(Boolean) || !b.buttons.every(Boolean)) hidden++;
    }
    const outside = since().filter((e) => e.type === 'move' && (+e.u < 0 || +e.u > 1 || +e.v < 0 || +e.v > 1)).length;
    check(hidden === 0, `6. ${what}: swept in ${n + 1} steps of 1 cm, the controls showed at every step (hidden ${hidden})`);
    check(outside === 0, `6. ${what}: the viewer's mouse never went outside the picture (${outside} moves outside)`);
}
async function sweepAndDragAll(shape) {
    await aimAt(await evalJs('window.__wolfvr.screenPoint(0, 0)')); await sleep(400);
    for (let i = 0; i < 4; i++) {
        const b0 = await big();
        const sx = i < 2 ? -1 : 1, sy = i % 2 ? 1 : -1;     // knobs: [-1,-1], [-1,1], [1,-1], [1,1]
        const kx = sx * (b0.size / 2 + 0.07), ky = sy * (b0.height / 2 + 0.07);
        await sweep(kx, ky, `${shape} screen, centre to corner handle ${i}`);
        check((await evalJs(`window.__wolfvr.big().hot`)) === i, `6. ${shape}: on corner handle ${i} it lights up`);
        next();
        await evalJs("window.__fakeCtl.press('right', 0, true)"); await frames(); await frames();
        const bg = await big();
        check(bg.drag && Math.abs(bg.size - b0.size) < 0.02, `7. ${shape}: corner handle ${i} taken, no jump (${b0.size.toFixed(3)} -> ${bg.size.toFixed(3)} m)`);
        for (let k = 1; k <= 10; k++) { await aimAt(await evalJs(`window.__wolfvr.screenPoint(${kx + sx * k * 0.01}, ${ky})`)); await frames(); }
        await evalJs("window.__fakeCtl.press('right', 0, false)"); await frames(); await frames();
        const b1 = await big();
        check(!b1.drag && b1.size > b0.size + 0.1, `7. ${shape}: corner handle ${i} pulled out 10 cm, the screen wider (${b0.size.toFixed(2)} -> ${b1.size.toFixed(2)} m)`);
        check(!since().some((e) => e.type === 'down' || e.type === 'up'), `7. ${shape}: ... and no click reached the viewer`);
        await aimAt(await evalJs('window.__wolfvr.screenPoint(0, 0)')); await sleep(300);
    }
    // down to each button under the screen ([Wolf 2026-11-06] as many as there are: Keyboard made it five)
    const nBtn = (await big()).buttons.length;
    for (let i = 0; i < nBtn; i++) {
        const b0 = await big();
        const bp = await evalJs(`window.__wolfvr.ctlBtnPoint(${i})`);
        // the button's place along the screen: x from its world point in the sweep's own terms (row centred, 0.66 m apart)
        const bx = (i - (nBtn - 1) / 2) * 0.66, by = -b0.height / 2 - 0.17;
        await sweep(bx, by, `${shape} screen, centre to button ${i}`);
        check((await evalJs('window.__wolfvr.big().hotBtn')) === i, `6. ${shape}: on button ${i} it lights up (button at ${bp.map((v) => v.toFixed(2)).join(',')})`);
        await aimAt(await evalJs('window.__wolfvr.screenPoint(0, 0)')); await sleep(300);
    }
}
// press one of the buttons under the screen (0 Nearer, 1 Further, 2 Screen shape, 3 Theatre, 4 Keyboard)
async function pressBtn(i) {
    await aimAt(await evalJs('window.__wolfvr.screenPoint(0, 0)')); await sleep(300);
    await aimAt(await evalJs(`window.__wolfvr.ctlBtnPoint(${i})`)); await frames(); await frames();
    await evalJs("window.__fakeCtl.press('right', 0, true)"); await frames(); await frames();
    await evalJs("window.__fakeCtl.press('right', 0, false)"); await frames(); await frames();
}
// the screen's four corners and its centre stand inside the theatre room (in the screen group's frame)
async function roomHolds(what) {
    const b = await big();
    const r = b.room;
    const half = b.size / 2, pts = [];
    for (const sx of [-1, 1]) for (const sy of [-1, 1]) {
        const x = sx * half, y = b.lift + sy * b.height / 2;   // [Wolf 2026-11-06] the lifted picture
        if (b.flat) pts.push([x, y, -b.dist]);
        else { const a = x / b.dist; pts.push([b.dist * Math.sin(a), y, -b.dist * Math.cos(a)]); }
    }
    const ok = r && pts.every(([x, y, z]) => z > r.front && z < r.back && Math.abs(x) < r.half && y > r.floor && y < r.top);
    check(!!ok, `8/9. ${what}: the theatre room holds the screen (front wall ${r ? r.front.toFixed(2) : '?'} m, screen ${b.flat ? 'flat' : 'curved'} ${b.size.toFixed(2)} m at ${b.dist} m)`);
}
await sleep(500);
check(!(await big()).flat, '8. the screen starts curved');
await roomHolds('curved at 2.5 m');
await sweepAndDragAll('curved');
await shotEye('eye_curved.png');
await pressBtn(2);
check((await big()).flat && (await evalJs("localStorage.getItem('wolfvr.screenFlat')")) === '1', '8. "Screen: Curved" pressed: the screen is flat, and remembered');
await shotEye('eye_flat.png');
await roomHolds('flat at 2.5 m');
await sweepAndDragAll('flat');
const d0 = (await big()).dist;
await pressBtn(0);
const d1 = (await big()).dist;
check(d1 === d0 - 0.5 && (await evalJs("localStorage.getItem('wolfvr.screenDist')")) === d1.toFixed(1), `9. "Nearer": ${d0} -> ${d1} m, remembered`);
await roomHolds('flat, nearer');
for (let k = 0; k < 4; k++) await pressBtn(0);
check((await big()).dist === 1.5, `9. "Nearer" stops at 1.5 m (${(await big()).dist})`);
await shotEye('eye_flat_near.png');
await pressBtn(2);
check(!(await big()).flat, '8. pressed again: curved again');
await roomHolds('curved at 1.5 m');
const bNear = await big();
check(bNear.size / bNear.dist <= 6.5 / 2.5 + 1e-6, `9. a curved screen near never wraps round further (${(bNear.size / bNear.dist).toFixed(2)} rad)`);
for (let k = 0; k < 8; k++) await pressBtn(1);
check((await big()).dist === 5, `9. "Further" stops at 5 m (${(await big()).dist})`);
await roomHolds('curved at 5 m');
await shotEye('eye_curved_far.png');
await sweepAndDragAll('curved far');
// ---------------------------------------------------------------- [Wolf 2026-11-06] 12. a big screen clears the floor
// Paul: stretched wide, "the bottom of the screen got cut off by i think the floor of the theatre". The widest screen
// at 2.5 m, curved and flat: its bottom and the button row under it stay above the floor, and the room still holds it.
for (let k = 0; k < 6; k++) await pressBtn(0);                       // back to 1.5 m, then out to 2.5 m
for (let k = 0; k < 2; k++) await pressBtn(1);
for (const shape of ['curved', 'flat']) {
    if ((await big()).flat !== (shape === 'flat')) await pressBtn(2);
    for (let k = 0; k < 8; k++) { await evalJs("document.getElementById('wvr-bigger').click()"); await frames(); }
    const b = await big();
    const bottom = b.lift - b.height / 2, btnBottom = bottom - 0.17 - 0.075;
    check(b.size > 6.4 && bottom > b.floor + 0.05 && btnBottom > b.floor,
          `12. ${shape}, widest (${b.size.toFixed(2)} m): bottom ${bottom.toFixed(2)} m, buttons ${btnBottom.toFixed(2)} m, floor ${b.floor.toFixed(2)} m, lifted ${b.lift.toFixed(2)} m`);
    await roomHolds(`${shape}, widest`);
    const btn = await evalJs('window.__wolfvr.ctlBtnPoint(4)');
    check(Array.isArray(btn), '12. the Keyboard button is there under the screen');
    await shotEye(`eye_${shape}_widest.png`);
}
// ---------------------------------------------------------------- [Wolf 2026-11-06] 10. headset detection
// Paul's Quest 2 ran the Quest 3's settings because WolfViewer's saved choice was quest3. The model the browser names
// now comes first (a choice made on the headset page still wins), and the VR Menu says which headset it is.
for (const [ua, want] of [['Mozilla/5.0 (X11; Linux x86_64; Quest 2) AppleWebKit/537.36 (KHTML, like Gecko) OculusBrowser/39.0 Chrome/136.0 VR Safari/537.36', 'quest2'],
                          ['Mozilla/5.0 (X11; Linux x86_64; Quest 3) AppleWebKit/537.36 (KHTML, like Gecko) OculusBrowser/39.0 Chrome/136.0 VR Safari/537.36', 'quest3'],
                          ['Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/136.0 Safari/537.36', 'quest3']]) {
    await send('Emulation.setUserAgentOverride', { userAgent: ua });
    await send('Page.reload', { ignoreCache: true });
    await sleep(500);
    await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.config())'), 15000);
    await evalJs("localStorage.removeItem('wolfvr.headset')");
    const got = await evalJs("window.__wolfvr.setViewerHeadset('quest3')");
    const txt = await evalJs('window.__wolfvr.headsetText()');
    const named = /Quest [23]/.test(ua);
    check(got === want && txt.includes(named ? 'found by itself' : "WolfViewer's Headset Settings"),
          `10. ${named ? ua.match(/Quest [23]/)[0] : 'an unnamed browser'} with WolfViewer set to quest3: ${got}, the VR Menu says "${txt}"`);
    if (named && want === 'quest2') {
        await evalJs("localStorage.setItem('wolfvr.headset', 'quest3')");
        check((await evalJs("window.__wolfvr.setViewerHeadset('auto')")) === 'quest3', '10. a choice made on the headset page still wins (manual choice kept)');
        await evalJs("localStorage.removeItem('wolfvr.headset')");
    }
}
console.log(failures ? `${failures} FAILED` : 'ALL PASSED');
chrome.kill('SIGKILL'); srv.kill('SIGKILL');
process.exit(failures ? 1 : 0);
