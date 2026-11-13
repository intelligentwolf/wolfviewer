// [VR CAMERA ON ENTRY 2026-11-13] Paul (11-12): he had moved the camera somewhere odd, went into VR 3D Mode and was stuck
// getting it back. Every entry into 3D Mode now gives the desktop camera the camera steps of the viewer's Reset View,
// and only those (wolfvrscreen.cpp resetCameraForVR). Against the REAL viewer (logged in as Shadow Testwolf,
// WolfVR3DMode on, WolfVRStreamMode "world"), in headless Chromium (muted) with WolfStorm's fake WebXR device and Touch
// Plus controllers, the LEAP relay FIFO in $RIG (default ~/.cache/skyband):
//   1. free camera on the SMALL camera window only (the main one never opened: the stock resetCameraMode early return)
//      -> enter VR: the reset line says free camera 1 -> 0; the head is placed behind and above the avatar's eyes
//   2. desktop mouselook -> enter VR: mode 1 -> 0; then the Camera Controls window's pan stick, held in the headset, still
//      moves the headset's view (reset once, not locked); exit VR: the desktop camera is back in mode 0, focus on avatar
//   3. out and straight back in: the logged gap decides - under 2 s the grace line and no reset, else a reset
//   4. Hide my avatar (WolfVRHideAvatar) -> enter VR: the head at the avatar's eyes
//   5. build mode (Build.Toggle: the Build window and the create tool, LLToolMgr::enterBuildMode, which with
//      EditCameraMovement also takes the focus off the avatar) -> enter VR: focus back, zoom reset, build tools still on
//      (Paul: "camera only"), the Build window still open
//   6. the flycam (JoystickFlycamEnabled on, View.JoystickFlycam) -> enter VR: flycam 1 -> 0
//   7. appearance editing (EditShape: the customise camera, LLVOAvatarSelf::onCustomizeStart) -> enter VR: the desktop
//      camera is left to it (no reset); then Escape (View.ResetView) ends the editing as a resident would
//   node e2e_vrcamentry.mjs <url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, appendFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { homedir } from 'node:os';

const [URL_, OUT, FAKE, CTL] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const H = process.env.RIG || resolve(homedir(), '.cache/skyband');
const LOG = resolve(H, 'prof/logs/WolfViewer.log');
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }
const leap = (pump, notation) => appendFileSync(resolve(H, 'leap.fifo'), `${pump}\t${notation}\n`);
async function leapInfo(path) {
    leap('LLWindow', `{'op':'getInfo','path':'${path}','reply':'@@REPLY@@'}`);
    await sleep(700);
    const lines = readFileSync(resolve(H, 'leap.log'), 'utf8').split('\n').filter(l => l.startsWith('IN ') && l.includes('"rect"') && l.includes(path.replace('//', '')));
    if (!lines.length) return null;
    try { const j = JSON.parse(lines[lines.length - 1].replace(/^IN \S+ /, '')); return j.data || j; } catch (e) { return null; }
}
const logLines = () => readFileSync(LOG, 'utf8').split('\n');
const after = (since, s) => logLines().slice(since).filter((x) => x.includes(s));
// the "VR camera controls (...)" line (wolfvrscreen.cpp logView), parsed
function viewLine(since, why) {
    const l = after(since, 'VR camera controls (').filter((x) => !why || x.includes(`(${why})`)).pop();
    if (!l) return null;
    const m = l.match(/VR camera controls \((.*?)\): view moved forward (-?[\d.]+) m, left (-?[\d.]+) m, up (-?[\d.]+) m, zoom x(-?[\d.]+), looking (-?\d+) degrees left of the (.*?); head (-?[\d.]+) m above the avatar's eyes, (-?[\d.]+) m from them/);
    return m ? { why: m[1], up: +m[4], zoom: +m[5], headUp: +m[8], headDist: +m[9], line: l.replace(/^.*VR camera/, 'VR camera') } : null;
}
// LLFloaterReg instanceVisible (llfloaterreglistener.cpp:110), its reply found by its reqid in the relay's log
async function floaterVisible(name, tag) {
    leap('LLFloaterReg', `{'op':'instanceVisible','name':'${name}','reply':'@@REPLY@@','reqid':'${tag}'}`);
    return waitFor(() => {
        const l = readFileSync(resolve(H, 'leap.log'), 'utf8').split('\n').filter((x) => x.startsWith('IN ') && x.includes(`"reqid": "${tag}"`)).pop();
        if (!l) return null;
        try { const j = JSON.parse(l.replace(/^IN \S+ /, '')); return { visible: !!(j.data || j).visible }; } catch (e) { return null; }
    }, 5000);
}
// LLViewerControl get (llviewercontrollistener.cpp:72), its reply found by its reqid in the relay's log
async function controlValue(key, tag) {
    leap('LLViewerControl', `{'op':'get','group':'Global','key':'${key}','reply':'@@REPLY@@','reqid':'${tag}'}`);
    const r = await waitFor(() => {
        const l = readFileSync(resolve(H, 'leap.log'), 'utf8').split('\n').filter((x) => x.startsWith('IN ') && x.includes(`"reqid": "${tag}"`)).pop();
        if (!l) return null;
        try { const j = JSON.parse(l.replace(/^IN \S+ /, '')); return { v: (j.data || j).value }; } catch (e) { return null; }
    }, 5000);
    return r ? r.v : null;
}
const setControl = (key, v) => leap('LLViewerControl', `{'op':'set','group':'Global','key':'${key}','value':${v === true}}`);
// the settings a run changes, read first and given back at the end (as round 6 does for the joystick)
const SAVED = {};
// a Boolean setting may come back as 0/1 (as seen in the 2026-11-13 runs: "JoystickEnabled 0") or as true/false
const asBool = (v) => (v === true || v === 1) ? true : (v === false || v === 0) ? false : null;
async function saveControls(keys) { for (const k of keys) SAVED[k] = asBool(await controlValue(k, `save_${k}`)); console.log('   settings before:', JSON.stringify(SAVED)); }
function restoreControls() { for (const [k, v] of Object.entries(SAVED)) if (typeof v === 'boolean') setControl(k, v); else console.log(`   ${k} was not read, left as the run set it`); }
function resetLine(since) {
    const l = after(since, 'VR 3D: camera reset on entering 3D Mode').pop();
    if (!l) return null;
    const m = l.match(/desktop camera mode (\d+) -> (\d+), focus on the avatar (\d) -> (\d), flycam (\d) -> (\d), free camera (\d) -> (\d), camera ([\d.e+-]+) m from the avatar before, zoom ([\d.e+-]+) -> ([\d.e+-]+), build tools (\d) -> (\d)/);
    const g = l.match(/; ([\d.]+) s since the last exit/);
    return m ? { mode0: +m[1], mode1: +m[2], focus0: +m[3], focus1: +m[4], fly0: +m[5], fly1: +m[6], free0: +m[7], free1: +m[8], dist: +m[9],
                 zoom0: +m[10], zoom1: +m[11], build0: +m[12], build1: +m[13], gap: g ? +g[1] : -1, line: l.replace(/^.*VR 3D:/, 'VR 3D:') } : null;
}

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader', '--mute-audio',
    '--ignore-certificate-errors', '--remote-debugging-port=9389', '--window-size=1280,1280',
    `--user-data-dir=${resolve(H, 'chrome')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
process.on('exit', () => { try { chrome.kill(); } catch (e) { /* gone */ } });
process.on('uncaughtException', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
process.on('unhandledRejection', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9389/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => { const r = await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true }); return r.result?.result?.value; };
const RIGHT = [0.2, 1.4, -0.2];
async function aimAt(p) {
    const dx = p[0] - RIGHT[0], dy = p[1] - RIGHT[1], dz = p[2] - RIGHT[2];
    const n = Math.hypot(dx, dy, dz);
    await evalJs(`window.__fakeCtl.aim('right', ${Math.atan2(-dx / n, -dz / n)}, ${Math.asin(dy / n)})`);
}
const press = async (hand, i, down) => evalJs(`window.__fakeCtl.press('${hand}', ${i}, ${down})`);
async function click(hand, i, holdMs = 250) { await press(hand, i, true); await sleep(holdMs); await press(hand, i, false); await sleep(600); }
async function aimAtView(path, fu = 0.5, fv = 0.5) {
    const info = await leapInfo(path), root = await leapInfo('//main_view');
    if (!info || !info.visible_chain || !info.rect || !root) return null;
    const x = info.rect.left + fu * (info.rect.right - info.rect.left), y = info.rect.top - fv * (info.rect.top - info.rect.bottom);
    await aimAt(await evalJs(`window.__wolfvr.windowPoint(${x / root.rect.right}, ${1 - y / root.rect.top})`)); await sleep(600);
    return await evalJs('window.__wolfvr.aim()');
}
const inWorld = () => evalJs('!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "world")');
async function enterVR(tag) {
    const since = logLines().length;
    await evalJs('window.__wolfvr.toggleVR()');
    check(await waitFor(() => evalJs('!!window.__fakeXR.session')), `${tag}: Enter VR`);
    check(await waitFor(inWorld, 30000), `${tag}: in VR 3D Mode`);
    return since;
}
async function exitVR(tag) {
    const since = logLines().length;
    await evalJs('window.__wolfvr.toggleVR()');
    const l = await waitFor(() => after(since, 'VR 3D: desktop camera back in mode').pop(), 15000);
    console.log('   exit ->', l && l.replace(/^.*VR 3D:/, 'VR 3D:'));
    return l;
}
// the default head place, wolfvrscreen.cpp buildMap: shown, 1.75 m behind and 0.45 m above the avatar's eyes
// (kBehind, kAbove: 1.807 m from them); hidden (WolfVRHideAvatar), at the eyes. The fake headset adds no lean at entry.
const BEHIND = 1.75, ABOVE = 0.45, DIST = Math.hypot(BEHIND, ABOVE);

await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
// the headset service starts when VR is switched on in this session (Wolf World > VR: WolfVRStreamEnabled, as the
// rig's campaign_vr.py does); WolfVRStreamMode only says where it starts
await saveControls(['WolfVRStreamEnabled', 'WolfVRHideAvatar']);
setControl('WolfVRStreamEnabled', true);
await sleep(4000);
await send('Page.navigate', { url: URL_ });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'connected to the real viewer');
await evalJs("try { localStorage.setItem('wolfvr.headset', 'auto'); } catch (e) {}");
leap('LLViewerControl', "{'op':'set','group':'Global','key':'WolfVRHideAvatar','value':false}");
// a known start whatever an earlier run left: Escape (Reset View), the camera and Build windows closed
leap('UI', "{'op':'call','function':'View.ResetView'}");
for (const f of ['camera', 'fs_camera_small', 'build']) leap('LLFloaterReg', `{'op':'hideInstance','name':'${f}'}`);
await sleep(1500);
for (let k = 0; k < 2; k++) { leap('LLWindow', "{'op':'keyDown','keycode':i129}"); leap('LLWindow', "{'op':'keyUp','keycode':i129}"); await sleep(600); }

// ---- 1. free camera on the small camera window only ----
leap('LLFloaterReg', "{'op':'showInstance','name':'fs_camera_small'}");
await sleep(2500);
const ov = await waitFor(async () => { const i = await leapInfo('//object_view'); return i && i.visible_chain ? i : null; }, 8000);
check(!!(ov && ov.visible_chain), '1: the small camera window is open, its Object View (free camera) button shown');
leap('LLWindow', "{'op':'mouseDown','button':'LEFT','path':'//object_view'}"); await sleep(200);
leap('LLWindow', "{'op':'mouseUp','button':'LEFT','path':'//object_view'}"); await sleep(1500);
let since = await enterVR('1');
let r = await waitFor(() => resetLine(since), 10000);
console.log('  ', r && r.line);
check(!!r && r.free0 === 1 && r.free1 === 0, `1: entering 3D Mode left the free camera (free camera ${r && r.free0} -> ${r && r.free1})`);
let v = await waitFor(() => viewLine(since, 'entering VR 3D Mode'), 10000);
console.log('  ', v && v.line);
check(!!v && Math.abs(v.headUp - ABOVE) < 0.15 && Math.abs(v.headDist - DIST) < 0.3,
      `1: the head is behind and above the avatar's eyes (${v && v.headUp} m above, ${v && v.headDist} m from them)`);
await exitVR('1');
leap('LLFloaterReg', "{'op':'hideInstance','name':'fs_camera_small'}");
await sleep(3500);   // past the 2 s grace

// ---- 2. desktop mouselook, then the camera controls in the headset ----
leap('UI', "{'op':'call','function':'View.Mouselook'}");
await sleep(2500);
since = await enterVR('2');
r = await waitFor(() => resetLine(since), 10000);
console.log('  ', r && r.line);
check(!!r && r.mode0 === 1 && r.mode1 === 0 && r.focus1 === 1, `2: entering 3D Mode left mouselook (mode ${r && r.mode0} -> ${r && r.mode1}, focus on the avatar ${r && r.focus1})`);
v = await waitFor(() => viewLine(since, 'entering VR 3D Mode'), 10000);
console.log('  ', v && v.line);
check(!!v && Math.abs(v.headUp - ABOVE) < 0.15 && Math.abs(v.headDist - DIST) < 0.3,
      `2: the head is behind and above the avatar's eyes (${v && v.headUp} m above, ${v && v.headDist} m from them)`);
leap('LLFloaterReg', "{'op':'showInstance','name':'camera'}");
await sleep(2500);
const tab = await leapInfo('//joysticks_tab');
if (tab && tab.visible_chain) { await aimAtView('//joysticks_tab'); await click('right', 0, 200); await sleep(800); }
check(!!(await waitFor(async () => { const i = await leapInfo('//cam_track_stick'); return i && i.visible_chain; }, 8000)), '2: the Camera Controls window is open in the headset');
await aimAtView('//cam_track_stick', 0.5, 0.85);
since = logLines().length;
await press('right', 0, true); await sleep(2000); await press('right', 0, false);
v = await waitFor(() => viewLine(since, 'move ended'), 6000);
console.log('   pan down ->', v && v.line);
check(!!v && v.up < -0.2 && v.headUp < ABOVE - 0.2, `2: after the reset the camera controls still move the headset view (up ${v && v.up} m, head ${v && v.headUp} m above the eyes)`);
leap('LLFloaterReg', "{'op':'hideInstance','name':'camera'}");
await sleep(1000);
const since2x = logLines().length;
const back = await exitVR('2');
check(!!back && /back in mode 0, focus on the avatar 1, flycam 0/.test(back), '2: out of VR the desktop camera is the normal one (third person, on the avatar)');
check(after(since2x, 'its focus is back on the avatar').length === 1, '2: the focus the camera window had unlocked was put back on the avatar on the way out');

// ---- 3. out and straight back in: the logged gap decides ----
await sleep(3500);
since = await enterVR('3a');
check(!!(await waitFor(() => resetLine(since), 10000)), '3: a new entry (more than 2 s out) resets');
await sleep(3000);
await evalJs('window.__wolfvr.toggleVR()');
await waitFor(() => after(since, 'VR 3D: stopped drawing the eyes').length >= 1, 10000);
const since3 = logLines().length;
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(inWorld, 30000), '3: back in VR 3D Mode at once');
const drawing = await waitFor(() => after(since3, 'VR 3D: drawing both eyes').length >= 1, 10000);
await sleep(2000);
const grace = after(since3, 'VR 3D: back in 3D Mode after').pop();
const gm = grace && grace.match(/after ([\d.]+) s - inside the 2 s grace/);
const r3 = resetLine(since3);
console.log(`   re-entry: drawing again ${!!drawing}; grace line: ${grace ? grace.replace(/^.*VR 3D:/, 'VR 3D:') : 'none'}; reset: ${r3 ? r3.line : 'none'}`);
check(!!drawing && ((!!gm && +gm[1] < 2 && !r3) || (!gm && !!r3 && r3.gap >= 2)),
      `3: the re-entry follows its logged gap (${gm ? gm[1] + ' s: grace, no reset' : r3 ? r3.gap + ' s: a new entry, reset' : 'nothing logged'})`);
if (!gm) console.log('   NOTE: the re-entry took 2 s or more, so the grace itself was not exercised this run');
await exitVR('3');
await sleep(3500);

// ---- 4. Hide my avatar: the head at the eyes ----
leap('LLViewerControl', "{'op':'set','group':'Global','key':'WolfVRHideAvatar','value':true}");
await sleep(500);
since = await enterVR('4');
v = await waitFor(() => viewLine(since, 'entering VR 3D Mode'), 10000);
console.log('  ', v && v.line);
check(!!v && Math.abs(v.headUp) < 0.15 && v.headDist < 0.2, `4: hidden avatar - the head is at the avatar's eyes (${v && v.headUp} m above, ${v && v.headDist} m from them)`);
await exitVR('4');
leap('LLViewerControl', "{'op':'set','group':'Global','key':'WolfVRHideAvatar','value':false}");
await sleep(3500);

// ---- 5. build mode: entering VR resets the camera only ----
// Build.Toggle (LLToolMgr::toggleBuildMode) does nothing until the Build window exists (gFloaterTools), and with the
// parameter "build" it stops after opening the window where the parcel does not allow building (enterBuildMode
// verify_canedit: seen at Sandbox 2's centre in the 09:59Z run - the window opened, no build tools). So the window is
// made first (shown, hidden) and then toggled with no parameter: the window and the create tool, whatever the parcel.
// Whether build mode is really on is read from the reset line (build tools before -> after).
leap('UI', "{'op':'call','function':'Floater.Show','parameter':'build'}");
await sleep(2500);
leap('UI', "{'op':'call','function':'Floater.Hide','parameter':'build'}");
await sleep(2000);
leap('UI', "{'op':'call','function':'Build.Toggle','parameter':''}");
await sleep(3000);
const b0 = await floaterVisible('build', 'b5a');
console.log(`   the Build window before VR: ${b0 && b0.visible ? 'open' : 'not reported open'}`);
since = await enterVR('5');
r = await waitFor(() => resetLine(since), 10000);
console.log('  ', r && r.line);
check(!!r && r.build0 === 1, `5: build mode was on as 3D Mode began (build tools ${r && r.build0})`);
check(!!r && r.build0 === 1 && r.build1 === 1, `5: the build tools are still on after the reset (${r && r.build0} -> ${r && r.build1})`);
check(!!r && r.focus1 === 1 && Math.abs(r.zoom1 - 1) < 1e-3 && r.mode1 === r.mode0,
      `5: the camera is back on the avatar, zoom reset, camera mode kept (focus ${r && r.focus0} -> ${r && r.focus1}, zoom ${r && r.zoom0} -> ${r && r.zoom1}, mode ${r && r.mode0} -> ${r && r.mode1})`);
await sleep(2000);
const b1 = await floaterVisible('build', 'b5b');
check(!!b1 && b1.visible === !!(b0 && b0.visible), `5: the Build window is as it was before VR (${b1 && b1.visible})`);
await exitVR('5');
leap('UI', "{'op':'call','function':'Build.Toggle','parameter':''}");   // out of build mode (LLToolMgr::leaveBuildMode)
await sleep(3500);

// ---- 6. the flycam on entry ----
// LLViewerJoystick::toggleFlycam needs JoystickEnabled and JoystickFlycamEnabled (llviewerjoystick.cpp:1390); both are
// read first and given back after
const joy0 = asBool(await controlValue('JoystickEnabled', 'j6a')), fly0 = asBool(await controlValue('JoystickFlycamEnabled', 'j6b'));
console.log(`   JoystickEnabled ${joy0}, JoystickFlycamEnabled ${fly0} before`);
leap('LLViewerControl', "{'op':'set','group':'Global','key':'JoystickEnabled','value':true}");
leap('LLViewerControl', "{'op':'set','group':'Global','key':'JoystickFlycamEnabled','value':true}");
await sleep(500);
leap('UI', "{'op':'call','function':'View.JoystickFlycam'}");
await sleep(2000);
since = await enterVR('6');
r = await waitFor(() => resetLine(since), 10000);
console.log('  ', r && r.line);
check(!!r && r.fly0 === 1 && r.fly1 === 0 && r.focus1 === 1, `6: entering 3D Mode turned the flycam off (flycam ${r && r.fly0} -> ${r && r.fly1})`);
await exitVR('6');
leap('LLViewerControl', `{'op':'set','group':'Global','key':'JoystickEnabled','value':${joy0 === true}}`);
leap('LLViewerControl', `{'op':'set','group':'Global','key':'JoystickFlycamEnabled','value':${fly0 === true}}`);
await sleep(3500);

// ---- 7. appearance editing: left to it ----
leap('UI', "{'op':'call','function':'EditShape'}");
await sleep(5000);
since = await enterVR('7');
const cust = await waitFor(() => after(since, 'while editing appearance - the desktop camera is left to it').pop(), 10000);
await sleep(1500);
console.log('  ', cust ? cust.replace(/^.*VR 3D:/, 'VR 3D:') : 'no appearance line', '; reset:', resetLine(since) ? 'yes' : 'none');
check(!!cust && !resetLine(since), '7: entering 3D Mode while editing appearance leaves the desktop camera (and the editing) alone');
await exitVR('7');
leap('UI', "{'op':'call','function':'View.ResetView'}");   // Escape: ends the editing, as a resident would
await sleep(2000);
console.log(`RESULT failures ${failures}`);
restoreControls();
await sleep(500);
process.exit(failures ? 1 : 0);
