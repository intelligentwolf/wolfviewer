// [VR SHADOWS FIX 2026-11-04] Paul's round-8 test: "flashing". The sun's shadow maps were kept every other frame
// (WolfVRShadowEvery 2) but paired with the matrices a reflection-probe render had overwritten in between, so the near
// shadows were wrong on every other picture. This test measures that signature against the REAL viewer, in headless
// Chromium with the fake headset held still: consecutive pictures are grabbed as small grey images and the mean
// absolute difference between neighbours (N, N+1) is compared with the difference two apart (N, N+2). Shadows that
// flip every other picture make the neighbour difference much larger than the two-apart one; a steady scene with the
// head still gives the two about the same (encoder noise only).
//   node e2e_shadowflicker.mjs <url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
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
    '--ignore-certificate-errors', '--remote-debugging-port=9395', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
process.on('exit', () => { try { chrome.kill('SIGKILL'); } catch (e) { /* gone */ } });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9395/json')).json(); } catch (e) { await sleep(250); } }
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
await evalJs('window.__wolfvr.setQuality(1)');    // Paul's 1728 px eyes
await evalJs('window.__fakeXR.setPitch(-0.35)');  // look a little down: ground and its shadows fill the lower picture
// Midday: World > Sun > Midday is Ctrl+Shift+Y (menu_viewer.xml "Midday", World.EnvSettings noon), pressed through the
// session's LEAP relay (LLWindow keyDown: keycode 89 = 'Y', llkeyboardsdl.cpp maps a-z to A-Z; mask as the listener
// documents it). The sun is then up, so the sun's shadow maps are drawn and any flicker in them shows.
const FIFO = process.env.LEAP_FIFO || resolve(process.env.HOME, '.cache/skyband/leap.fifo');
try {
    writeFileSync(FIFO, "LLWindow\t{'op':'keyDown','keycode':89,'mask':['CTL','SHIFT']}\n");
    writeFileSync(FIFO, "LLWindow\t{'op':'keyUp','keycode':89,'mask':['CTL','SHIFT']}\n");
    console.log('   sent Ctrl+Shift+Y (Midday) through ' + FIFO);
} catch (e) { console.log('   could not press Midday: ' + e.message); }
await sleep(15000);                               // warm-up: textures, the scene settling, the stream at its size, the sun's move
// 40 distinct consecutive pictures, the head still
const frames = [];
const t0 = Date.now();
while (frames.length < 40 && Date.now() - t0 < 30000) {
    const f = await evalJs('(() => { const p = window.__wolfvr.pictureGrey(); return p ? { frame: p.frame, g: p.g, depth: p.depth, tan: p.tan, stereo: p.stereo, meshes: p.meshes } : null; })()');
    if (f && (!frames.length || f.frame !== frames[frames.length - 1].frame)) frames.push({ frame: f.frame, g: Buffer.from(f.g, 'base64'), depth: f.depth, tan: f.tan, stereo: f.stereo, meshes: f.meshes });
    await sleep(8);
}
check(frames.length >= 30, `grabbed ${frames.length} consecutive pictures (frames ${frames[0]?.frame}..${frames[frames.length - 1]?.frame})`);
// [VR DEPTH GAPS] a picture without its depth or its eye windows is shown on the flat cards at 10 m instead of the
// world-locked meshes: the whole world jumps for that picture. Paul's 20:37Z device lines: 524 depth pictures in a
// 30 s window that decoded about 570 - one picture in twelve on the cards.
const noDepth = frames.filter(f => !f.depth), noTan = frames.filter(f => !f.tan), noMesh = frames.filter(f => !f.meshes);
console.log(`   pictures without depth: ${noDepth.length} of ${frames.length} (frames ${noDepth.map(f => f.frame).join(' ') || '-'}); without eye windows: ${noTan.length}; shown on the cards: ${noMesh.length}`);
check(noDepth.length === 0 && noTan.length === 0 && noMesh.length === 0, 'every picture came with its depth and eye windows and was shown on the meshes');
// two consecutive pictures as the left eye sees them, for looking at (renderEye: the scene drawn with the left view)
for (let i = 0; i < 2; i++) {
    const fr = await evalJs('window.__wolfvr.pictureGrey() && window.__wolfvr.pictureGrey().frame');
    const png = await evalJs('window.__wolfvr.renderEye(0)');
    if (png) writeFileSync(resolve(OUT, `eye_consecutive_${i}_frame${fr}.png`), Buffer.from(png.split(',')[1], 'base64'));
    await sleep(45);
}
const mad = (a, b) => { let s = 0; for (let i = 0; i < a.length; i++) s += Math.abs(a[i] - b[i]); return s / a.length; };
const d1 = [], d2 = [];
for (let i = 0; i + 1 < frames.length; i++) d1.push(mad(frames[i].g, frames[i + 1].g));
for (let i = 0; i + 2 < frames.length; i++) d2.push(mad(frames[i].g, frames[i + 2].g));
const med = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.floor(s.length / 2)] : NaN; };
const m1 = med(d1), m2 = med(d2);
console.log(`   neighbour difference (N, N+1): median ${m1.toFixed(2)} grey levels; two apart (N, N+2): median ${m2.toFixed(2)}; ratio ${(m1 / m2).toFixed(2)}`);
console.log('   per picture (N, N+1): ' + d1.map(x => x.toFixed(1)).join(' '));
writeFileSync(resolve(OUT, 'shadowflicker.json'), JSON.stringify({ frames: frames.map(f => f.frame), d1, d2, m1, m2 }, null, 1));
// a scene whose shadows flip every other picture: neighbours differ far more than pictures two apart
check(Number.isFinite(m1) && Number.isFinite(m2) && m1 <= 1.5 * m2 + 0.5, `no alternating change between consecutive pictures (ratio ${(m1 / m2).toFixed(2)} <= 1.5)`);
const line = await evalJs('window.__wolfvr.deviceLine()');
console.log('   ' + (/shown on the display[^;]*;[^;]*/.exec(line) || ['(no shown line)'])[0]);
console.log(failures ? `${failures} FAILED` : 'ALL PASSED');
chrome.kill('SIGKILL');
process.exit(failures ? 1 : 0);
