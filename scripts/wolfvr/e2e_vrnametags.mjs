// [VR NAME TAGS 2026-11-13] Paul: "in 3d vr mode there are no character names above avatars or voice dots". The
// name tags, hover text and voice dots are now drawn into each eye's picture (wolfvrscreen.cpp drawHudObjectsInEye),
// and one's own tag and dot are left out in first person (Hide my avatar), as in mouselook. Against the REAL viewer
// (Shadow Testwolf, WolfVR3DMode on, WolfVRStreamMode "world", default name tag settings: RenderNameShowSelf and
// AvatarNameTagMode 1, so the own tag is shown), headless Chromium (muted) with the fake headset, LEAP FIFO in $RIG:
//   1. shown (WolfVRHideAvatar off): the one-per-session log line says the eyes draw the HUD objects, own tag shown
//      (1); the headset's picture is saved (eyes_shown.jpg) to look at the tag above the avatar's head in both halves
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
function hudLine(since) {
    const l = after(since, 'VR 3D: name tags, hover text and voice dots drawn in the').pop();
    if (!l) return null;
    const m = l.match(/drawn in the (eyes|view): (\d+) name tags shown \(own (\d+)\)(, first person)?/);
    return m ? { where: m[1], tags: +m[2], own: +m[3], first: !!m[4], line: l.replace(/^.*VR 3D:/, 'VR 3D:') } : null;
}
await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
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
process.exit(failures ? 1 : 0);
