// [VR NAME TAGS 2026-11-13] Paul: "in 3d vr mode there are no character names above avatars or voice dots". The
// name tags, hover text and voice dots are now drawn into each eye's picture (wolfvrscreen.cpp drawHudObjectsInEye),
// and one's own tag and dot are left out in first person (Hide my avatar), as in mouselook. Against the REAL viewer
// (Shadow Testwolf, WolfVR3DMode on, WolfVRStreamMode "world", default name tag settings: RenderNameShowSelf and
// AvatarNameTagMode 1, so the own tag is shown), headless Chromium (muted) with the fake headset, LEAP FIFO in $RIG:
//   1. shown (WolfVRHideAvatar off): the one-per-session log line says the eyes draw the HUD objects, own tag shown
//      (1); the headset's picture is saved (eyes_shown.jpg) to look at the tag above the avatar's head in both halves;
//      and in pixels: switching the own tag off (RenderNameShowSelf) takes its white text out of the pictures (the
//      near-white pixel count drops by far more than it differs between two pictures with the tag on)
//   2. first person (WolfVRHideAvatar on): the same line says own 0, "first person"
// What it cannot check: the tag's look and its stereo depth (look at the saved pictures: the tag must be above the
// head in both halves, shifted between them by the head's own disparity), and voice dots (no voice in the rig).
//   node e2e_vrnametags.mjs <url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, appendFileSync, writeFileSync } from 'node:fs';
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
const logLines = () => readFileSync(LOG, 'utf8').split('\n');
const after = (since, s) => logLines().slice(since).filter((x) => x.includes(s));

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader', '--mute-audio',
    '--ignore-certificate-errors', '--remote-debugging-port=9390', '--window-size=1280,1280',
    `--user-data-dir=${resolve(H, 'chrome')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
process.on('exit', () => { try { chrome.kill(); } catch (e) { /* gone */ } });
process.on('uncaughtException', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
process.on('unhandledRejection', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9390/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => { const r = await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true }); return r.result?.result?.value; };
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

const grab = async (file) => { const d = await evalJs('window.__wolfvr.grab()'); if (d) writeFileSync(resolve(OUT, file), Buffer.from(d.split(',')[1], 'base64')); return !!d; };
// near-white pixels (all channels > 215) of a received picture within a window around (u, v) of one eye's half (u, v
// 0..1 of that eye, from the viewer's own log: where the own tag's anchor landed). The tag's text is white (the default
// name tag colour); looking only around its anchor keeps the avatar's own moving pale hands and the white buildings out
// (the 11:30-11:40Z runs: block differences, whole-picture counts and changed pixels all drowned in them)
async function whiteNear(pic, eye, u, v) {
    return evalJs(`(async () => { const i = new Image(); i.src = ${JSON.stringify(pic)}; await i.decode();
        const c = document.createElement('canvas'); c.width = i.width; c.height = i.height; const x = c.getContext('2d');
        x.drawImage(i, 0, 0); const half = i.width / 2, cx = ${eye} * half + ${u} * half, cy = ${v} * i.height;
        const x0 = Math.max(${eye} * half, Math.round(cx - 110)), x1 = Math.min((${eye} + 1) * half, Math.round(cx + 110));
        const y0 = Math.max(0, Math.round(cy - 60)), y1 = Math.min(i.height, Math.round(cy + 60));
        if (x1 <= x0 || y1 <= y0) return -1;
        const d = x.getImageData(x0, y0, x1 - x0, y1 - y0).data; let n = 0;
        for (let k = 0; k < d.length; k += 4) if (d[k] > 215 && d[k + 1] > 215 && d[k + 2] > 215) n++; return n; })()`);
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
function hudLine(since) {
    const l = after(since, 'VR 3D: name tags, hover text and voice dots drawn in the').pop();
    if (!l) return null;
    const m = l.match(/drawn in the (eyes|view): (\d+) name tags shown \(own (\d+)\)(, first person)?/);
    const p = l.match(/own tag at (-?[\d.]+),(-?[\d.]+) of the (left|right) eye/);
    return m ? { where: m[1], tags: +m[2], own: +m[3], first: !!m[4], u: p ? +p[1] : null, v: p ? +p[2] : null,
                 eye: p ? (p[3] === 'right' ? 1 : 0) : null, line: l.replace(/^.*VR 3D:/, 'VR 3D:') } : null;
}
await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
// the headset service starts when VR is switched on in this session (Wolf World > VR: WolfVRStreamEnabled, as the
// rig's campaign_vr.py does); WolfVRStreamMode only says where it starts
await saveControls(['WolfVRStreamEnabled', 'WolfVRHideAvatar', 'RenderNameShowSelf']);
setControl('WolfVRStreamEnabled', true);
await sleep(4000);
await send('Page.navigate', { url: URL_ });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'connected to the real viewer');
await evalJs("try { localStorage.setItem('wolfvr.headset', 'auto'); } catch (e) {}");
for (let k = 0; k < 2; k++) { leap('LLWindow', "{'op':'keyDown','keycode':i129}"); leap('LLWindow', "{'op':'keyUp','keycode':i129}"); await sleep(600); }

leap('LLViewerControl', "{'op':'set','group':'Global','key':'WolfVRHideAvatar','value':false}");
await sleep(1000);
let since = await enterVR('1');
let h = await waitFor(() => hudLine(since), 15000);
console.log('  ', h && h.line);
check(!!h && h.where === 'eyes' && h.own === 1 && !h.first, `1: the eyes draw the HUD objects, own name tag shown (${h && h.tags} tags, own ${h && h.own})`);
await sleep(1500);
check(await grab('eyes_shown.jpg'), '1: the headset picture saved (eyes_shown.jpg: look for the tag above the head in both halves)');
const a1 = await evalJs('window.__wolfvr.grab()');
await sleep(600);
const a2 = await evalJs('window.__wolfvr.grab()');
leap('LLViewerControl', "{'op':'set','group':'Global','key':'RenderNameShowSelf','value':false}");
await sleep(900);
const b = await evalJs('window.__wolfvr.grab()');
await grab('eyes_no_own_tag.jpg');
setControl('RenderNameShowSelf', typeof SAVED.RenderNameShowSelf === 'boolean' ? SAVED.RenderNameShowSelf : true);
const near = (pic) => h && h.u !== null && pic ? whiteNear(pic, h.eye, h.u, h.v) : -1;
const n1 = await near(a1), n2 = await near(a2), nb = await near(b);
console.log(`   near-white pixels around the own tag's anchor (${h && h.u},${h && h.v} of the ${h && h.eye ? 'right' : 'left'} eye): tag on ${n1}, ${n2}; tag off ${nb}`);
check(n1 >= 20 && n2 >= 20 && nb >= 0 && nb * 3 <= Math.min(n1, n2),
      `1: the tag's white text is there with the tag on and gone with it off (${n1}, ${n2} -> ${nb})`);
await sleep(2500);
await exitVR('1');
await sleep(3500);

leap('LLViewerControl', "{'op':'set','group':'Global','key':'WolfVRHideAvatar','value':true}");
await sleep(1500);
since = await enterVR('2');
h = await waitFor(() => hudLine(since), 15000);
console.log('  ', h && h.line);
check(!!h && h.own === 0 && h.first, `2: first person - own name tag left out (own ${h && h.own})`);
await grab('eyes_first_person.jpg');
await exitVR('2');
leap('LLViewerControl', "{'op':'set','group':'Global','key':'WolfVRHideAvatar','value':false}");
console.log(`RESULT failures ${failures}`);
restoreControls();
await sleep(500);
process.exit(failures ? 1 : 0);
