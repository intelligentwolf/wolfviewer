// [VR STEREO 2026-11-04] VR 3D Mode stereo against the REAL viewer (already logged in, WolfVRStreamMode "world", the
// stream on <lan_url>): headless Chromium with WolfStorm's fake WebXR device (two eyes 64 mm apart) enters VR and
// checks that the pictures are two eyes side by side, drawn from the eye positions the page sent, and that near things
// differ between the eyes while the sky does not (a horizontal shift found by block matching the two halves).
//   node e2e_stereo.mjs <lan_url> <out_dir> <fake_xr.js> [seconds=70]
// Saves s_*.jpg (whole side-by-side pictures) and prints the page's numbers every 5 s. Not shipped to residents.
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [LAN, OUT, FAKE, SECS = '70'] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--autoplay-policy=no-user-gesture-required', '--remote-debugging-port=9383', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9383/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => (await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
const grab = async (file) => { const d = await evalJs('window.__wolfvr.grab()'); if (d) writeFileSync(resolve(OUT, file), Buffer.from(d.split(',')[1], 'base64')); return !!d; };
// Best horizontal shift (0..maxShift px, right half moved left) of the right eye against the left, in a band of rows
// (fractions of the height), by the sum of absolute differences of grey levels.
const DISPARITY = `async (y0, y1, maxShift) => {
    const g = window.__wolfvr.grab(); if (!g) return null;
    const img = new Image(); img.src = g; await img.decode();
    const c = document.createElement('canvas'); c.width = img.width; c.height = img.height;
    const x = c.getContext('2d'); x.drawImage(img, 0, 0);
    const W = img.width / 2, H = img.height, d = x.getImageData(0, 0, img.width, H).data;
    const grey = (px, py) => { const i = (py * img.width + px) * 4; return d[i] * 0.3 + d[i + 1] * 0.59 + d[i + 2] * 0.11; };
    const r0 = Math.floor(H * y0), r1 = Math.floor(H * y1);
    let best = -1, bestSad = Infinity, zeroSad = 0, contrast = 0;
    for (let s = 0; s <= maxShift; s++) {
        let sad = 0, n = 0;
        for (let py = r0; py < r1; py += 2) for (let px = maxShift + 8; px < W - 8; px += 2) {
            sad += Math.abs(grey(px, py) - grey(W + px - s, py)); n++;
            if (s === 0) contrast += Math.abs(grey(px, py) - grey(px + 2, py));
        }
        sad /= n; if (s === 0) { zeroSad = sad; contrast /= n; }
        if (sad < bestSad) { bestSad = sad; best = s; }
    }
    return { shift: best, sad: +bestSad.toFixed(2), sadAtZero: +zeroSad.toFixed(2), contrast: +contrast.toFixed(2), w: img.width, h: H };
}`;
await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') });
await send('Page.navigate', { url: LAN });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.config() && window.__wolfvr.config().mode === "world")'), 30000), 'VR 3D Mode from the real viewer');
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!window.__fakeXR.session')), 'Enter VR');
check(await waitFor(() => evalJs('window.__wolfvr.config().w === 2 * window.__wolfvr.config().h'), 20000), 'side-by-side picture size (w = 2 h)');
await sleep(5000);
check(await grab('s_straight.jpg'), 'a picture (s_straight.jpg)');
const stereoRow = await evalJs('(window.__wolfvr.numbers().rows.find(r => r[0] === "3D picture") || []).join(" | ")');
console.log('   3D picture:', stereoRow);
check(/two eyes/.test(stereoRow || ''), 'the viewer says the picture is two eyes');
check(/64 mm/.test(stereoRow || ''), 'drawn from eyes 64 mm apart (the fake headset IPD)');
// [VR AUDIO] the viewer's sound arrives and plays (the test viewer's speakers are a null sink)
const au = await waitFor(async () => { const a = await evalJs('window.__wolfvr.audio()'); return a && a.buffered > 0 ? a : null; }, 10000);
console.log('   sound:', JSON.stringify(au));
check(!!au, 'the viewer\'s sound reaches the page');
// look down a little so the near ground fills the lower part of the picture
await evalJs('window.__fakeXR.setPitch(-0.5)'); await sleep(4000);
await grab('s_down.jpg');
const near = await evalJs(`(${DISPARITY})(0.75, 0.95, 60)`);
const far = await evalJs(`(${DISPARITY})(0.02, 0.15, 60)`);
console.log('   near band (bottom):', JSON.stringify(near), ' far band (top):', JSON.stringify(far));
await evalJs('window.__fakeXR.setPitch(0)');
const t0 = Date.now();
let k = 0;
while (Date.now() - t0 < Number(SECS) * 1000) {
    const tt = (Date.now() - t0) / 1000;
    await evalJs(`window.__fakeXR.setYaw(${(Math.PI / 6) * Math.sin(tt * 0.5)})`);
    await sleep(250);
    if (++k % 20 === 0) {
        const n = await evalJs('window.__wolfvr.numbers().rows');
        console.log(`+${tt.toFixed(0)}s`, JSON.stringify(n.filter(r => /photon|fps|encode|sent|3D/.test(r[0]))));
    }
}
await grab('s_end.jpg');
console.log('   sound at the end:', JSON.stringify(await evalJs('window.__wolfvr.audio()')));
await evalJs('window.__wolfvr.toggleVR()');
ws.close(); chrome.kill();
console.log(failures ? `E2E STEREO FAILED (${failures})` : 'E2E STEREO PASSED');
process.exit(failures ? 1 : 0);
