// [VR CAMERA CONTROLS 2026-11-07] Paul: "the camera controls dont work in vr" (and, seated in a train coach, "i'm in the
// roof"). Against the REAL viewer in VR 3D Mode (logged in as Shadow Testwolf, WolfVRStreamMode "world", LEAP relay FIFO at
// ~/.cache/skyband/leap.fifo), in headless Chromium with WolfStorm's fake WebXR device and Touch Plus controllers - the
// Camera Controls window used as a resident uses it in the headset: its window is a panel, the controller's ray on it is
// the mouse, the trigger held is the mouse button held:
//   1. pan stick held at its bottom: the view sinks (the viewer's "VR camera controls" log line: up < 0, head lower)
//   2. zoom + held: the view comes closer (zoom < 1); the picture changes
//   3. orbit stick held at its left: the view turns about the avatar (looking left of its facing)
//   4. Rear View preset: back to the default place (all 0, zoom 1)
//   5. (E2E_SEAT=1) seated on something with a sit-on-click (a train coach at the platform), the same pan-down works there
//      and its log line says how far the head is above the avatar's eyes
//   node e2e_vrcam.mjs <lan_url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync, appendFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { homedir } from 'node:os';

const [LAN, OUT, FAKE, CTL] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const H = resolve(homedir(), '.cache/skyband');
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
// the newest "VR camera controls" line of the viewer's log after `since` lines, parsed
const logLines = () => readFileSync(LOG, 'utf8').split('\n');
function viewLine(since) {
    const l = logLines().slice(since).filter((x) => x.includes('VR camera controls (')).pop();
    if (!l) return null;
    const m = l.match(/VR camera controls \((.*?)\): view moved forward (-?[\d.]+) m, left (-?[\d.]+) m, up (-?[\d.]+) m, zoom x(-?[\d.]+), looking (-?\d+) degrees left of the (.*?); head (-?[\d.]+) m above the avatar's eyes, (-?[\d.]+) m from them/);
    return m ? { why: m[1], fwd: +m[2], left: +m[3], up: +m[4], zoom: +m[5], look: +m[6], of: m[7], headUp: +m[8], headDist: +m[9], line: l.replace(/^.*VR camera/, 'VR camera') } : null;
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
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => { const r = await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true }); return r.result?.result?.value; };
const grab = async (file) => { const d = await evalJs('window.__wolfvr.grab()'); if (d) writeFileSync(resolve(OUT, file), Buffer.from(d.split(',')[1], 'base64')); return d; };
const RIGHT = [0.2, 1.4, -0.2];
async function aimAt(p) {
    const dx = p[0] - RIGHT[0], dy = p[1] - RIGHT[1], dz = p[2] - RIGHT[2];
    const n = Math.hypot(dx, dy, dz);
    await evalJs(`window.__fakeCtl.aim('right', ${Math.atan2(-dx / n, -dz / n)}, ${Math.asin(dy / n)})`);
}
const press = async (hand, i, down) => evalJs(`window.__fakeCtl.press('${hand}', ${i}, ${down})`);
async function click(hand, i, holdMs = 250) { await press(hand, i, true); await sleep(holdMs); await press(hand, i, false); await sleep(600); }
// aim the ray at a point of a viewer control: (fu, fv) = 0..1 across its rectangle from its top-left
async function aimAtView(path, fu = 0.5, fv = 0.5) {
    const info = await leapInfo(path), root = await leapInfo('//main_view');
    if (!info || !info.visible_chain || !info.rect || !root) return null;
    const x = info.rect.left + fu * (info.rect.right - info.rect.left), y = info.rect.top - fv * (info.rect.top - info.rect.bottom);
    await aimAt(await evalJs(`window.__wolfvr.windowPoint(${x / root.rect.right}, ${1 - y / root.rect.top})`)); await sleep(600);
    return await evalJs('window.__wolfvr.aim()');
}
// hold the trigger on a control for ms, and the view line that follows
async function holdOn(path, fu, fv, ms) {
    const a = await aimAtView(path, fu, fv);
    const since = logLines().length;
    await press('right', 0, true); await sleep(ms); await press('right', 0, false);
    const v = await waitFor(() => viewLine(since), 6000);
    return { aim: a, view: v };
}
// mean absolute difference of two received pictures (0..255), as a "the headset's view moved" measure
async function pictureDiff(a, b) {
    return evalJs(`(async () => { const L = async (s) => { const i = new Image(); i.src = s; await i.decode(); const c = document.createElement('canvas');
        c.width = 256; c.height = 128; const x = c.getContext('2d'); x.drawImage(i, 0, 0, 256, 128); return x.getImageData(0, 0, 256, 128).data; };
        const p = await L(${JSON.stringify(a)}), q = await L(${JSON.stringify(b)}); let s = 0; for (let k = 0; k < p.length; k += 4) s += Math.abs(p[k] - q[k]) + Math.abs(p[k + 1] - q[k + 1]) + Math.abs(p[k + 2] - q[k + 2]);
        return +(s / (p.length / 4) / 3).toFixed(2); })()`);
}

await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: LAN });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'connected to the real viewer');
await evalJs("try { localStorage.setItem('wolfvr.headset', 'auto'); } catch (e) {}");
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!window.__fakeXR.session')), 'Enter VR');
check(await waitFor(() => evalJs('!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "world")'), 30000), 'in VR 3D Mode');
// a modal notice the region may have shown at log-in holds the mouse until answered
for (let k = 0; k < 2; k++) { leap('LLWindow', "{'op':'keyDown','keycode':i129}"); leap('LLWindow', "{'op':'keyUp','keycode':i129}"); await sleep(600); }

async function cameraWindow() {
    leap('LLFloaterReg', "{'op':'showInstance','name':'camera'}");
    await sleep(2500);
    // the joysticks page (the window may open on its buttons page)
    const tab = await leapInfo('//joysticks_tab');
    if (tab && tab.visible_chain) { await aimAtView('//joysticks_tab'); await click('right', 0, 200); await sleep(800); }
    return !!(await waitFor(async () => { const i = await leapInfo('//cam_track_stick'); return i && i.visible_chain; }, 8000));
}

async function controlsRound(tag) {
    check(await cameraWindow(), `${tag}: the Camera Controls window is open in the headset, its pan stick shown`);
    await sleep(1500);
    const before = await grab(`${tag}_0_before.jpg`);
    // 1. pan down: the pan stick (cam_track_stick) held near its bottom edge
    const d = await holdOn('//cam_track_stick', 0.5, 0.85, 2000);
    console.log('   pan down: aim', JSON.stringify(d.aim && { k: d.aim.k }), '->', d.view && d.view.line);
    check(!!d.aim && d.aim.k === 'ui', `${tag}: the ray on the pan stick is on the viewer's UI`);
    check(!!d.view && d.view.why === 'move ended' && d.view.up < -0.2, `${tag}: pan stick held at its bottom - the headset view sank (up ${d.view && d.view.up} m)`);
    await sleep(1500);
    const sunk = await grab(`${tag}_1_pan_down.jpg`);
    const diff1 = before && sunk ? await pictureDiff(before, sunk) : -1;
    check(diff1 > 2, `${tag}: and the headset's picture changed with it (mean difference ${diff1})`);
    // 2. zoom in: the + button held
    const z = await holdOn('//zoom_plus_btn', 0.5, 0.5, 1500);
    console.log('   zoom in ->', z.view && z.view.line);
    check(!!z.view && z.view.zoom < 0.9, `${tag}: zoom + held - the view came closer (zoom x${z.view && z.view.zoom})`);
    // 3. orbit: the rotate stick held near its left edge
    const o = await holdOn('//cam_rotate_stick', 0.15, 0.5, 1500);
    console.log('   orbit ->', o.view && o.view.line);
    check(!!o.view && Math.abs(o.view.look) >= 10, `${tag}: rotate stick held at its side - the view turned about the avatar (${o.view && o.view.look} degrees)`);
    await sleep(1000);
    await grab(`${tag}_2_moved.jpg`);
    // 4. Rear View preset (the presets list: rear_view)
    const since = logLines().length;
    const pr = await aimAtView('//rear_view');
    await click('right', 0, 200);
    const p = await waitFor(() => viewLine(since), 6000);
    console.log('   rear view ->', JSON.stringify(pr && { k: pr.k }), p && p.line);
    check(!!p && p.why === 'preset' && p.up === 0 && p.fwd === 0 && p.left === 0 && Math.abs(p.zoom - 1) < 0.01 && Math.abs(p.look) <= 1,
          `${tag}: Rear View puts the headset view back where it started`);
    await sleep(1500);
    await grab(`${tag}_3_rear.jpg`);
    return p;
}

const foot = await controlsRound('foot');

if (process.env.E2E_SEAT === '1') {
    // ---- seated: find a sit-on-click object (a train coach at the platform), sit with the trigger, same controls ----
    leap('LLFloaterReg', "{'op':'hideInstance','name':'camera'}");
    const deadline = Date.now() + (+(process.env.E2E_SEAT_MIN || 25)) * 60000;
    let sat = false, found = null;
    while (!sat && Date.now() < deadline) {
        for (let turn = 0; turn < 8 && !sat; turn++) {
            for (const pitch of [0.05, -0.05, -0.15] ) {
                for (let yaw = -0.9; yaw <= 0.91 && !sat; yaw += 0.15) {
                    await evalJs(`window.__fakeCtl.aim('right', ${yaw}, ${pitch})`); await sleep(110);
                    const a = await evalJs('window.__wolfvr.aim()');
                    if (a && a.k === 'world' && a.c === 'sit' && a.d < 25) {
                        found = { yaw, pitch, ...a };
                        await click('right', 0, 200);
                        sat = !!(await waitFor(() => evalJs('window.__wolfvr.bar().sit === 2'), 6000));
                        console.log(`   sit-on-click object ${a.o} at ${a.d} m: ${sat ? 'seated' : 'not seated'}`);
                    }
                }
                if (sat) break;
            }
            if (!sat) { await evalJs("window.__fakeCtl.axis('right', 2, 1)"); await sleep(300); await evalJs("window.__fakeCtl.axis('right', 2, 0)"); await sleep(1200); }
        }
        if (!sat) { console.log('   nothing to sit on in view yet; waiting for a train', new Date().toISOString()); await sleep(45000); }
    }
    // no train came: sit on the nearest object instead (LEAP LLAgent requestSit by position - llagentlistener.cpp
    // findObjectClosestTo), so the seated path (the rig on the seat: onVehicle) is still exercised
    if (!sat) {
        leap('LLAgent', "{'op':'getPosition','reply':'@@REPLY@@'}"); await sleep(800);
        const l = readFileSync(resolve(H, 'leap.log'), 'utf8').split('\n').filter((x) => x.startsWith('IN ') && x.includes('"region"')).pop();
        let pos = null; try { const j = JSON.parse(l.replace(/^IN \S+ /, '')); pos = (j.data || j).region; } catch (e) { /* none */ }
        if (pos) {
            leap('LLAgent', `{'op':'requestSit','position':[r${pos[0]},r${pos[1]},r${pos[2]}]}`);
            sat = !!(await waitFor(() => evalJs('window.__wolfvr.bar().sit === 2'), 10000));
            console.log(`   no train: LEAP requestSit on the object closest to ${pos.map((v) => v.toFixed(1))}: ${sat ? 'seated' : 'not seated'}`);
        }
    }
    check(sat, 'seated on an object (a carriage when one is at the platform)');
    if (sat) {
        await evalJs("window.__fakeCtl.aim('right', 0, 0)");
        await sleep(4000);
        await grab('seat_0_default.jpg');
        // the default seated place, logged by a preset press (no movement): how far above the avatar's eyes the head is
        const s = await controlsRound('seat');
        console.log('   seated default (after Rear View):', s && s.line);
        // and a long press down, as Paul would to get out of a roof
        const d2 = await holdOn('//cam_track_stick', 0.5, 0.85, 3000);
        console.log('   seated, pan down 3 s ->', d2.view && d2.view.line);
        check(!!d2.view && d2.view.up < -0.5 && d2.view.headUp < (s ? s.headUp - 0.5 : 0), 'seated: a longer press takes the head down by more than half a metre');
        await sleep(1500);
        await grab('seat_4_down_more.jpg');
        leap('LLAgent', "{'op':'requestStand'}");
        await sleep(3000);
    }
}
leap('LLFloaterReg', "{'op':'hideInstance','name':'camera'}");
console.log(`RESULT failures ${failures}`);
process.exit(failures ? 1 : 0);
