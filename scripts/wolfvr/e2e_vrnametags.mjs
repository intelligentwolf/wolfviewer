// [VR NAME TAGS 2026-11-13] Paul: "in 3d vr mode there are no character names above avatars or voice dots". The
// name tags, hover text and voice dots are now drawn into each eye's picture (wolfvrscreen.cpp drawHudObjectsInEye),
// and one's own tag and dot are left out in first person (Hide my avatar), as in mouselook. Against the REAL viewer
// (Shadow Testwolf, WolfVR3DMode on, WolfVRStreamMode "world", default name tag settings: RenderNameShowSelf and
// AvatarNameTagMode 1, so the own tag is shown), headless Chromium (muted) with the fake headset, LEAP FIFO in $RIG:
//   1. shown (WolfVRHideAvatar off): the one-per-session log line says the eyes draw the HUD objects, own tag shown
//      (1); the headset's picture is saved (eyes_shown.jpg) to look at the tag above the avatar's head in both halves;
//      and in pixels: the picture changes when the own tag is switched off (RenderNameShowSelf) by clearly more than
//      between two pictures with it on (the sea and sky move a little by themselves)
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
// per 32x16 block of the pictures (scaled to 512x256) the mean absolute difference (0..255) of a/b, less that of the
// same block between n1/n2 (two pictures with nothing changed: the sea, the sky and the avatar's idle move by
// themselves); the largest such excess. A name tag is small, so a whole-picture mean would hide it.
async function tagBlockChange(n1, n2, a, b) {
    return evalJs(`(async () => { const L = async (s) => { const i = new Image(); i.src = s; await i.decode(); const c = document.createElement('canvas');
        c.width = 512; c.height = 256; const x = c.getContext('2d'); x.drawImage(i, 0, 0, 512, 256); return x.getImageData(0, 0, 512, 256).data; };
        const P = [await L(${JSON.stringify(n1)}), await L(${JSON.stringify(n2)}), await L(${JSON.stringify(a)}), await L(${JSON.stringify(b)})];
        const blk = (p, q, bx, by) => { let s = 0; for (let y = by; y < by + 16; y++) for (let x = bx; x < bx + 32; x++) { const k = (y * 512 + x) * 4;
            s += Math.abs(p[k] - q[k]) + Math.abs(p[k + 1] - q[k + 1]) + Math.abs(p[k + 2] - q[k + 2]); } return s / (32 * 16 * 3); };
        let best = -1e9, noise = 0;
        for (let by = 0; by < 256; by += 16) for (let bx = 0; bx < 512; bx += 32) {
            const n = blk(P[0], P[1], bx, by); noise = Math.max(noise, n); best = Math.max(best, blk(P[2], P[3], bx, by) - n); }
        return { excess: +best.toFixed(2), noise: +noise.toFixed(2) }; })()`);
}
function hudLine(since) {
    const l = after(since, 'VR 3D: name tags, hover text and voice dots drawn in the').pop();
    if (!l) return null;
    const m = l.match(/drawn in the (eyes|view): (\d+) name tags shown \(own (\d+)\)(, first person)?/);
    return m ? { where: m[1], tags: +m[2], own: +m[3], first: !!m[4], line: l.replace(/^.*VR 3D:/, 'VR 3D:') } : null;
}
await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
// the headset service starts when VR is switched on in this session (Wolf World > VR: WolfVRStreamEnabled, as the
// rig's campaign_vr.py does); WolfVRStreamMode only says where it starts
leap('LLViewerControl', "{'op':'set','group':'Global','key':'WolfVRStreamEnabled','value':true}");
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
await sleep(2500);
const a2 = await evalJs('window.__wolfvr.grab()');
leap('LLViewerControl', "{'op':'set','group':'Global','key':'RenderNameShowSelf','value':false}");
await sleep(2500);
const b = await evalJs('window.__wolfvr.grab()');
await grab('eyes_no_own_tag.jpg');
leap('LLViewerControl', "{'op':'set','group':'Global','key':'RenderNameShowSelf','value':true}");
const r = a1 && a2 && b ? await tagBlockChange(a1, a2, a2, b) : null;
console.log(`   tag on -> off: largest block change beyond its own noise ${r && r.excess} (largest noise ${r && r.noise})`);
check(!!r && r.excess > 6, `1: switching the own tag off changes the headset picture where the tag was (${r && r.excess} beyond noise)`);
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
process.exit(failures ? 1 : 0);
