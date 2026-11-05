// [VR KEYBOARD 2026-11-05] Paul: "the user can still use the keyboard while in vr, i find using cursors way easier than
// using the controllers to move the avatar" - the PC's own keyboard did nothing in VR 3D Mode or VR Big Screen Mode.
// This drives the REAL viewer with REAL key presses and reads where the avatar went (LEAP LLAgent getPosition). The keys
// are never LEAP's key injection (that calls the viewer's key handlers directly, skipping the window, its focus and SDL):
//   KEYS=wayland (the default; residents' WolfViewer on a Wayland desktop, WolfNativeWayland on): wtype's virtual
//     keyboard on a bare Hyprland nested in the desktop's (session_vrkeys.sh), where the viewer is the only window - the
//     key arrives as wl_keyboard events through SDL's Wayland backend, as a hardware key does
//   KEYS=x11 (XWayland / X11 desktops): xkeys.c, XTEST on the viewer's own X server - X KeyPress / KeyRelease through
//     SDL's X11 backend
// in each of:
//   desktop  - the stream on, the headset page connected but not in VR (the control)
//   3d       - VR 3D Mode, the fake headset in VR (WolfStorm's fake WebXR device + Touch Plus controllers)
//   big      - VR Big Screen Mode (the left stick click, as the resident switches)
//   after    - VR left again
// In each: Escape (the world has the keyboard), the cursor keys walk (Up / Down) and turn (Left / Right), W and S walk
// (with "letter keys move the avatar" on: the Firestorm session settings, settings_firestorm.xml, have letters start a
// chat instead, as on the desktop), Page Up flies up and Page Down comes down, and the keyboard types into the chat bar
// (Return gives it the keyboard; letters typed there must not walk the avatar), then Escape and the cursor keys walk again.
// The window is given the keyboard ONCE, at the start, as a resident's window has it at their desk; each phase prints
// which window has it, so a focus change caused by VR would show.
// [VR KEYBOARD 2026-11-05] Paul: "i had not selected the viewer and so when i'm using the keyboard its working with another
// application". KEYS=wayland also checks, with a second window (foot) in the nested Hyprland taking the keyboard:
//   - entering VR while the other window has the keyboard: the viewer asks the desktop for it once (the nested Hyprland
//     has misc.focus_on_activate on, as Paul's Omarchy desktop); either it gets it or the headset says where the keys go
//   - in VR 3D Mode and in Big Screen: the keyboard moved to the other window shows the headset's notice "Your PC keyboard
//     is going to another window..." (and the arrow keys do not walk the avatar), the keyboard back on the viewer hides it
//   KEYS=x11 DISPLAY=:N XKEYS=<xkeys binary> | KEYS=wayland WAYLAND_DISPLAY=<nested> NESTED_HYPR=<its instance signature>
//   VIEWER_TREE=<install tree> node e2e_keyboard.mjs <lan_url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
import { spawn, execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync, appendFileSync, readdirSync, readlinkSync } from 'node:fs';
import { resolve } from 'node:path';
import { homedir } from 'node:os';

const [LAN, OUT, FAKE, CTL] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const H = resolve(homedir(), '.cache/skyband');
const MODE = process.env.KEYS || 'wayland', TREE = process.env.VIEWER_TREE;
if (!TREE || (MODE === 'x11' ? !(process.env.XKEYS && process.env.DISPLAY) : !(process.env.WAYLAND_DISPLAY && process.env.NESTED_HYPR))) {
    console.log('ERROR needs VIEWER_TREE and, for KEYS=x11, DISPLAY + XKEYS, for KEYS=wayland, WAYLAND_DISPLAY + NESTED_HYPR'); process.exit(2);
}
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }
const leap = (pump, notation) => appendFileSync(resolve(H, 'leap.fifo'), `${pump}\t${notation}\n`);
const run = (cmd, a, env) => execFileSync(cmd, a, { encoding: 'utf8', timeout: 20000, env: { ...process.env, ...env } }).trim();
// real key presses (keysym names: Up, Prior, Next, Return, BackSpace, Escape, w ...)
const keys = MODE === 'x11' ? {
    focus: (pid) => run(process.env.XKEYS, ['focus', pid]),
    focused: () => run(process.env.XKEYS, ['focused']),
    hold: (k, ms) => run(process.env.XKEYS, ['hold', k, String(ms)]),
    tap: (k) => run(process.env.XKEYS, ['tap', k]),
    type: (t) => run(process.env.XKEYS, ['type', t]),
} : {
    // the nested compositor gives its only window the keyboard by itself; "focused" asks it which window that is
    focus: () => keys.focused(),
    focused: () => { try { const w = JSON.parse(run('hyprctl', ['-j', 'activewindow'], { HYPRLAND_INSTANCE_SIGNATURE: process.env.NESTED_HYPR })); return `${w.class || '?'} pid ${w.pid}`; } catch (e) { return 'none'; } },
    hold: (k, ms) => run('wtype', ['-P', k, '-s', String(ms), '-p', k]),
    tap: (k) => run('wtype', ['-k', k]),
    type: (t) => run('wtype', ['-d', '60', t]),
};
// [VR KEYBOARD] KEYS=wayland: another window in the nested Hyprland (foot), and moving the keyboard between it and the viewer
const nested = (args) => run('hyprctl', args, { HYPRLAND_INSTANCE_SIGNATURE: process.env.NESTED_HYPR });
let other = null;
function openOther() {
    other = spawn('foot', ['--app-id=wolfkeys-other', 'sleep', '900'], { stdio: 'ignore', env: { ...process.env } });
    process.on('exit', () => { try { other.kill(); } catch (e) { /* gone */ } });
}
const focusPid = (p) => nested(['dispatch', `hl.dsp.focus({ window = "pid:${p}" })`]);
// the notice as the page has it ({away, shown, text})
const notice = () => evalJs('window.__wolfvr.pcKeys()');
// the keyboard to the other window: the headset says so, and the cursor keys go there (the avatar stays)
async function focusAway(name) {
    focusPid(other.pid);
    await sleep(1200);
    console.log(`   ${name}: the keyboard is on ${keys.focused()}`);
    const n = await waitFor(async () => { const x = await notice(); return x && x.shown ? x : null; }, 3000, 200);
    check(!!n, `${name}: the keyboard on another window shows the headset notice ("${n && n.text}")`);
    const a = await agent(); keys.hold('Up', 1200); await sleep(700); const b = await agent();
    check(!!a && !!b && flat(a, b) < 0.3, `${name}: the cursor keys then go to the other window, not the avatar (${a && b ? flat(a, b).toFixed(2) : '?'} m)`);
    focusPid(pid);
    const t0 = Date.now();
    const gone = await waitFor(async () => { const x = await notice(); return x && !x.shown && !x.away ? x : null; }, 3000, 100);
    check(!!gone, `${name}: the keyboard back on the viewer hides the notice (${Date.now() - t0} ms)`);
}
// the newest LEAP reply line in the relay's log that matches `has` (a RegExp)
function lastReply(has) {
    const l = readFileSync(resolve(H, 'leap.log'), 'utf8').split('\n').filter((x) => x.startsWith('IN ') && has.test(x)).pop();
    if (!l) return null;
    try { const j = JSON.parse(l.replace(/^IN \S+ /, '')); return j.data || j; } catch (e) { return null; }
}
let seq = 0;
// where the avatar is: region position and heading (LEAP LLAgent getPosition), each reply told apart by its reqid
// (LLReqID copies the request's reqid into the reply, llevents.h)
async function agent() {
    const tag = new RegExp(`"reqid": ${++seq}[,}]`);
    leap('LLAgent', `{'op':'getPosition','reply':'@@REPLY@@','reqid':i${seq}}`);
    const r = await waitFor(() => { const j = lastReply(tag); return j && j.region ? j : null; }, 5000, 150);
    return r ? { p: r.region, yaw: r.euler.yaw } : null;
}
// a UI control's value (LEAP UI getValue)
async function uiValue(path) {
    const tag = new RegExp(`"reqid": ${++seq}[,}]`);
    leap('UI', `{'op':'getValue','path':'${path}','reply':'@@REPLY@@','reqid':i${seq}}`);
    const r = await waitFor(() => { const j = lastReply(tag); return j ? j : null; }, 5000, 150);
    return r ? r.value : undefined;
}
const flat = (a, b) => Math.hypot(b.p[0] - a.p[0], b.p[1] - a.p[1]);
const turned = (a, b) => { let d = b.yaw - a.yaw; while (d > Math.PI) d -= 2 * Math.PI; while (d < -Math.PI) d += 2 * Math.PI; return d; };

// the viewer's own process (its window carries _NET_WM_PID)
function viewerPid() {
    for (const d of readdirSync('/proc')) {
        if (!/^\d+$/.test(d)) continue;
        try { const e = readlinkSync(`/proc/${d}/exe`); if (e.startsWith(TREE + '/bin/') && e.includes('do-not-directly-run')) return d; } catch (e) { /* not ours */ }
    }
    return null;
}

async function phase(name) {
    console.log(`---- ${name} ${new Date().toISOString().slice(11, 19)}Z: the keyboard is on ${keys.focused()} (the viewer's window ${WIN})`);
    keys.tap('Escape'); await sleep(500);   // as a resident would: whatever text field had the keyboard lets go of it
    // walk forward with the cursor key, then back again (so the avatar stays where the next phase expects it)
    let a = await agent(); keys.hold('Up', 1600); await sleep(900); let b = await agent();
    console.log(`   Up held 1.6 s: ${JSON.stringify(a && a.p)} -> ${JSON.stringify(b && b.p)}`);
    check(!!a && !!b && flat(a, b) > 1.0, `${name}: the Up cursor key walks the avatar (${a && b ? flat(a, b).toFixed(2) : '?'} m)`);
    a = b; keys.hold('Down', 1600); await sleep(900); b = await agent();
    check(!!a && !!b && flat(a, b) > 0.5, `${name}: the Down cursor key walks it back (${a && b ? flat(a, b).toFixed(2) : '?'} m)`);
    a = b; keys.hold('Left', 1000); await sleep(600); b = await agent();
    check(!!a && !!b && turned(a, b) > 0.3, `${name}: the Left cursor key turns the avatar left (${a && b ? turned(a, b).toFixed(2) : '?'} rad)`);
    a = b; keys.hold('Right', 1000); await sleep(600); b = await agent();
    check(!!a && !!b && turned(a, b) < -0.3, `${name}: the Right cursor key turns it back (${a && b ? turned(a, b).toFixed(2) : '?'} rad)`);
    a = b; keys.hold('w', 1600); await sleep(900); b = await agent();
    check(!!a && !!b && flat(a, b) > 1.0, `${name}: W walks the avatar (${a && b ? flat(a, b).toFixed(2) : '?'} m)`);
    a = b; keys.hold('s', 1600); await sleep(900); b = await agent();
    check(!!a && !!b && flat(a, b) > 0.5, `${name}: S walks it back (${a && b ? flat(a, b).toFixed(2) : '?'} m)`);
    // fly: Page Up held starts flying and climbs; Page Down held comes down (and lands)
    a = await agent(); keys.hold('Prior', 2500); await sleep(900); b = await agent();
    check(!!a && !!b && b.p[2] - a.p[2] > 1.0, `${name}: Page Up flies up (${a && b ? (b.p[2] - a.p[2]).toFixed(2) : '?'} m)`);
    a = b; keys.hold('Next', 4000); await sleep(1500); b = await agent();
    check(!!a && !!b && a.p[2] - b.p[2] > 0.8, `${name}: Page Down comes down (${a && b ? (a.p[2] - b.p[2]).toFixed(2) : '?'} m)`);
    keys.hold('Next', 2500); await sleep(1000);   // on the ground (Page Down held on landing stops flying)
    if (process.env.E2E_NOTYPE) return;
    // typing: Return gives the chat bar the keyboard; the letters go into it and do not walk the avatar
    a = await agent();
    keys.tap('Return'); await sleep(700);
    keys.type('wolf keys'); await sleep(900);
    const typed = await uiValue('//default_chat_bar');
    b = await agent();
    console.log(`   chat bar after typing: ${JSON.stringify(typed)}`);
    check(typeof typed === 'string' && typed.includes('wolf keys'), `${name}: typed text goes into the chat bar ("${typed}")`);
    check(!!a && !!b && flat(a, b) < 0.3, `${name}: typing W into the chat bar does not walk the avatar (${a && b ? flat(a, b).toFixed(2) : '?'} m)`);
    // empty it again and give the keyboard back to the world (nothing is said in chat)
    for (let i = 0; i < 12; i++) keys.tap('BackSpace');
    keys.tap('Escape'); await sleep(600);
    a = await agent(); keys.hold('Up', 1600); await sleep(900); b = await agent();
    check(!!a && !!b && flat(a, b) > 1.0, `${name}: after Escape the cursor keys walk again (${a && b ? flat(a, b).toFixed(2) : '?'} m)`);
    keys.hold('Down', 1600); await sleep(900);
}

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
const press = async (hand, i, down) => evalJs(`window.__fakeCtl.press('${hand}', ${i}, ${down})`);

const pid = viewerPid();
leap('LLViewerControl', "{'op':'set','group':'Global','key':'LetterKeysAffectsMovementNotFocusChatBar','value':true}");
check(!!pid, `the test viewer's process (${pid})`);
const WIN = pid ? keys.focus(pid) : '?';
check(MODE === 'x11' ? keys.focused() === WIN : WIN.includes(`pid ${pid}`), `the viewer's window has the keyboard (${WIN})`);
await sleep(1000);

await send('Page.enable'); await send('Runtime.enable'); await send('Log.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: LAN });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'the headset page is connected to the real viewer (not in VR yet)');
await evalJs("try { localStorage.setItem('wolfvr.headset', 'auto'); } catch (e) {}");

// as a resident starts VR: Wolf World > VR opens the VR window (its address and QR code) and they click in it; the
// viewer hides it once the headset connects (wolfvrstream.cpp reconcileDesktop)
leap('LLFloaterReg', "{'op':'showInstance','name':'wolf_vr_stream','focus':true}");
await sleep(1500);
const only = (process.env.E2E_PHASES || 'desktop,3d,big,after').split(',');
if (only.includes('desktop')) await phase('desktop');

const WL = MODE === 'wayland';
if (WL) {
    // the resident's keyboard is on another window when they go into VR
    openOther();
    check(!!(await waitFor(() => keys.focused().includes(`pid ${other.pid}`), 8000)), `another window has the keyboard before VR (${keys.focused()})`);
}
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!window.__fakeXR.session')), 'Enter VR');
check(await waitFor(() => evalJs('!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "world")'), 30000), 'in VR: VR 3D Mode');
if (WL) {
    const got = await waitFor(() => keys.focused().includes(`pid ${pid}`), 5000);
    const n = await notice();
    console.log(`   entering VR with the keyboard elsewhere: the desktop ${got ? 'gave the viewer the keyboard' : 'kept it on the other window'}; notice ${JSON.stringify(n)}`);
    check(!!got || !!(n && n.shown), 'entering VR: the viewer has the keyboard, or the headset says where the keys are going');
    if (!got) focusPid(pid);
    await sleep(1500);
}
await sleep(3000);
if (only.includes('3d')) await phase('3d');
if (WL) await focusAway('3d');

await press('left', 3, true); await sleep(200); await press('left', 3, false);
check(!!(await waitFor(async () => { const s = await evalJs('window.__wolfvr.surfaces()'); return s && s.big ? s : null; }, 15000)), 'VR Big Screen Mode (left stick click)');
await sleep(3000);
if (only.includes('big')) await phase('big');
if (WL) await focusAway('big');

await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(async () => !(await evalJs('!!window.__fakeXR.session')), 10000), 'left VR');
await sleep(3000);
if (only.includes('after')) await phase('after');

const errs = events.filter(e => e.method === 'Runtime.exceptionThrown' || (e.method === 'Log.entryAdded' && e.params.entry.level === 'error' && e.params.entry.source !== 'network')).slice(0, 8);
for (const e of errs) console.log('ERR', JSON.stringify(e.params).slice(0, 400));
check(errs.length === 0, 'no page errors');
ws.close(); chrome.kill(); if (other) other.kill();
console.log(failures ? `E2E KEYBOARD FAILED (${failures})` : 'E2E KEYBOARD PASSED');
process.exit(failures ? 1 : 0);
