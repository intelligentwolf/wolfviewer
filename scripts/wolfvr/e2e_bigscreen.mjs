// [VR BIG SCREEN 2026-11-03] End-to-end test of big-screen mode with no headset: the test server standing in for the
// viewer window (WOLFVR_MODE=screen: a 1280x720 "desktop", every pointer/key event printed as "IN ..."), headless
// Chromium with WolfStorm's fake WebXR device plus fake_xr_controllers.js.
//   node scripts/wolfvr/e2e_bigscreen.mjs <selftest_bin> <web_root> <fake_xr.js> <fake_xr_controllers.js> <out> [port=4355]
// Checks: the page builds the curved screen; mouse on the 2D preview = click / right click / wheel at the right place;
// typing box = text, Enter, Backspace; in VR the right controller aimed at the centre clicks there, grip = right
// click, stick = wheel; left stick changes the size; the left menu button recentres. Ports 4350+ only.
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { resolve } from 'node:path';

const [BIN, WEBROOT, FAKE, CTL, OUT, PORT = '4355'] = process.argv.slice(2);
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
const port = 9361;
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
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.config() && window.__wolfvr.config().mode === "screen")'), 15000), 'page gets config mode "screen" after Allow');
check(await waitFor(() => evalJs('window.__wolfvr.big().visible')), 'the big screen shows the viewer picture');
check(!(await evalJs("document.getElementById('wvr-big-controls').hidden")), 'size buttons and typing box are shown');
check((await evalJs('window.__wolfvr.big().size')) === 3.2, 'a first visit gets the default size, 3.2 m wide');
await sleep(500);
await shot('1_page.jpg');

// ---- 2D preview: mouse at the middle of the preview = the middle of the picture (the camera looks at its centre)
const rect = await evalJs("(() => { const r = document.querySelector('#wvr-canvas-wrap canvas').getBoundingClientRect(); return {x: r.left + r.width / 2, y: r.top + r.height / 2}; })()");
await send('Page.bringToFront');
next();
const mouse = (type, button = 'left', extra = {}) => send('Input.dispatchMouseEvent', { type, x: rect.x, y: rect.y, button, buttons: type === 'mousePressed' ? (button === 'right' ? 2 : 1) : 0, clickCount: 1, ...extra });
await mouse('mouseMoved', 'none');
await mouse('mousePressed'); await mouse('mouseReleased');
await sleep(400);
let ev = since();
const down = ev.find(e => e.type === 'down'), up = ev.find(e => e.type === 'up');
check(down && up && Math.abs(+down.u - 0.5) < 0.02 && Math.abs(+down.v - 0.5) < 0.02 && down.b === '0', `2D click arrives at the centre as a left click (${down ? down.u + ',' + down.v : 'none'})`);
next();
await mouse('mousePressed', 'right'); await mouse('mouseReleased', 'right');
await sleep(400);
check(since().some(e => e.type === 'down' && e.b === '2'), '2D right click arrives as a right click');
next();
await send('Input.dispatchMouseEvent', { type: 'mouseWheel', x: rect.x, y: rect.y, deltaX: 0, deltaY: 120 });
await sleep(400);
check(since().some(e => e.type === 'wheel' && e.wheel === '1'), '2D wheel down arrives as one click down');

// ---- typing box
next();
await evalJs("document.getElementById('wvr-type').focus()");
await send('Input.insertText', { text: 'hello' });
await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 });
await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 });
await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Backspace', code: 'Backspace', windowsVirtualKeyCode: 8 });
await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Backspace', code: 'Backspace', windowsVirtualKeyCode: 8 });
await sleep(400);
ev = since();
check(ev.filter(e => e.type === 'text').map(e => e.text).join('') === 'hello', 'typed text arrives as text: ' + ev.filter(e => e.type === 'text').map(e => e.text).join(''));
check(ev.some(e => e.type === 'keydown' && e.key === 'Enter') && ev.some(e => e.type === 'keyup' && e.key === 'Enter'), 'Enter arrives as a key down and up');
check(ev.some(e => e.type === 'keydown' && e.key === 'Backspace'), 'Backspace arrives as a key');
await evalJs("document.getElementById('wvr-type').blur()");

// ---- size buttons
const w0 = await evalJs('window.__wolfvr.big().size');
await evalJs("document.getElementById('wvr-bigger').click()");
check((await evalJs('window.__wolfvr.big().size')) > w0, 'Bigger makes the screen wider');
await evalJs("document.getElementById('wvr-smaller').click()");
check((await evalJs('window.__wolfvr.big().size')) === w0, 'Smaller makes it narrower again');

// ---- VR with controllers
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!window.__fakeXR.session')), 'Enter VR');
await sleep(800);
next();
await evalJs("window.__fakeCtl.aim('right', 0.08, 0.08)");   // from 0.2 m right, 0.2 m low: about at the centre
await sleep(300);
await evalJs("window.__fakeCtl.press('right', 0, true)"); await sleep(200);
await evalJs("window.__fakeCtl.press('right', 0, false)"); await sleep(300);
ev = since();
const vd = ev.find(e => e.type === 'down');
await shot('2a_vr_aim.jpg');
check(vd && vd.b === '0' && Math.abs(+vd.u - 0.5) < 0.1 && Math.abs(+vd.v - 0.5) < 0.1, `trigger = left click near the centre (${vd ? vd.u + ',' + vd.v : 'none'})`);
check(ev.some(e => e.type === 'up' && e.b === '0'), 'releasing the trigger releases the button');
next();
await evalJs("window.__fakeCtl.press('right', 1, true)"); await sleep(200);
await evalJs("window.__fakeCtl.press('right', 1, false)"); await sleep(300);
check(since().some(e => e.type === 'down' && e.b === '2'), 'grip = right click');
next();
await evalJs("window.__fakeCtl.axis('right', 3, 0.9)"); await sleep(500);
await evalJs("window.__fakeCtl.axis('right', 3, 0)"); await sleep(200);
const wheels = since().filter(e => e.type === 'wheel');
check(wheels.length >= 2 && wheels.every(e => e.wheel === '1'), `right stick down = scroll down (${wheels.length} clicks in 0.5 s)`);
next();
await evalJs("window.__fakeCtl.aim('right', 1.6, 0)");          // far off to the left: off the screen
await sleep(300);
await evalJs("window.__fakeCtl.press('right', 0, true)"); await sleep(200);
await evalJs("window.__fakeCtl.press('right', 0, false)"); await sleep(300);
check(!since().some(e => e.type === 'down'), 'no click when pointing off the screen');
// still pointing away from the screen: right stick up/down = screen size
const s0 = await evalJs('window.__wolfvr.big().size');
await evalJs("window.__fakeCtl.axis('right', 3, -0.9)"); await sleep(300);
await evalJs("window.__fakeCtl.axis('right', 3, 0)"); await sleep(300);
check((await evalJs('window.__wolfvr.big().size')) > s0, 'right stick up, pointing away from the screen, makes it bigger');
await evalJs("window.__fakeCtl.axis('right', 3, 0.9)"); await sleep(300);
await evalJs("window.__fakeCtl.axis('right', 3, 0)"); await sleep(300);
// [VR MOVE] left stick walks (analogue, heartbeat while held, zero on release); right stick X = snap turn; X = up
next();
await evalJs("window.__fakeCtl.axis('left', 3, -0.95)"); await sleep(700);
await evalJs("window.__fakeCtl.axis('left', 3, 0)"); await sleep(300);
let st2 = since().filter(e => e.type === 'stick');
check(st2.length >= 3 && +st2[0].y < -0.9 && +st2[st2.length - 1].y === 0, `left stick forward = walk (${st2.length} stick messages, last one lets go)`);
next();
await evalJs("window.__fakeCtl.axis('right', 2, 0.9)"); await sleep(250);
await evalJs("window.__fakeCtl.axis('right', 2, 0)"); await sleep(250);
let tu = since().filter(e => e.type === 'turn');
check(tu.length === 1 && tu[0].wheel === '-1', 'right stick right = one snap turn to the right');
next();
await evalJs("window.__fakeCtl.press('left', 4, true)"); await sleep(400);
await evalJs("window.__fakeCtl.press('left', 4, false)"); await sleep(300);
let li = since().filter(e => e.type === 'lift');
check(li.length >= 2 && li[0].wheel === '1' && li[li.length - 1].wheel === '0', 'X held = jump / fly up, then let go');
// recentre: turn the head 90 degrees left, press the left menu button, aim straight ahead of the head again
await evalJs('window.__fakeXR.setYaw(Math.PI / 2)');
await sleep(200);
await evalJs("window.__fakeCtl.press('left', 7, true)"); await sleep(200);
await evalJs("window.__fakeCtl.press('left', 7, false)"); await sleep(300);
next();
await evalJs("window.__fakeCtl.aim('right', Math.PI / 2 + 0.08, 0.08)");
await sleep(300);
await evalJs("window.__fakeCtl.press('right', 0, true)"); await sleep(200);
await evalJs("window.__fakeCtl.press('right', 0, false)"); await sleep(300);
check(since().some(e => e.type === 'down'), 'menu button recentres the screen where the head looks');
await shot('2_vr.jpg');
// [VR 3D] left stick click switches to VR 3D Mode without reconnecting, and back
await evalJs("window.__fakeCtl.press('left', 3, true)"); await sleep(150);
await evalJs("window.__fakeCtl.press('left', 3, false)");
check(await waitFor(() => evalJs('window.__wolfvr.config() && window.__wolfvr.config().mode === "world" && window.__wolfvr.config().w === 1440')), 'left stick click switches to VR 3D Mode (new config 1440x1440, same connection)');
check(lines.some(l => l === 'MODE world'), 'the PC was asked for VR 3D Mode');
await sleep(1500);
check(!(await evalJs('window.__wolfvr.big().visible')), 'the big screen is hidden in VR 3D Mode');
next();
await evalJs("window.__fakeCtl.axis('left', 2, 0.7)"); await sleep(400);
await evalJs("window.__fakeCtl.axis('left', 2, 0)"); await sleep(300);
const walk = since().filter(e => e.type === 'stick');
check(walk.length >= 2 && +walk[0].x > 0.6 && +walk[walk.length - 1].x === 0, 'VR 3D Mode: left stick right = strafe right');
await shot('3_world.jpg');
await evalJs("window.__fakeCtl.press('left', 3, true)"); await sleep(150);
await evalJs("window.__fakeCtl.press('left', 3, false)");
check(await waitFor(() => evalJs('window.__wolfvr.config() && window.__wolfvr.config().mode === "screen"')), 'and back to VR Big Screen Mode');
const st = lines.filter(l => l.startsWith('STATS ')).pop();
console.log('   numbers:', st);
const errs = events.filter(e => e.method === 'Runtime.exceptionThrown').slice(0, 5);
for (const e of errs) console.log('ERR', JSON.stringify(e.params).slice(0, 300));
check(errs.length === 0, 'no script errors on the page');
await evalJs('window.__wolfvr.toggleVR()');
ws.close(); chrome.kill(); srv.kill('SIGTERM');
console.log(failures ? `E2E BIGSCREEN FAILED (${failures})` : 'E2E BIGSCREEN PASSED');
process.exit(failures ? 1 : 0);
