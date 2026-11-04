// [VR 3D FIX 2026-11-04] VR 3D Mode after Paul's Quest 3 test, against the REAL viewer (logged in, WolfVRStreamMode
// "world", stream on <lan_url>, LEAP relay FIFO at ~/.cache/skyband/leap.fifo), in headless Chromium with WolfStorm's fake
// WebXR device and Touch Plus controllers (fake_xr_controllers.js: Quest 3 button layout, thumb on the thumbrest, the
// browser's select events):
//   1. stereo routing: each half of the picture is on the layer only its own eye's XR camera draws, by XRView.eye, also
//      when the browser lists the right eye first; each eye's own frustum (tangents) is sent and comes back in the
//      picture header; the two halves differ (near things shifted, sky not); the eye images saved by the viewer
//   2. HUD clicks: the thumbrest does not toggle the bar; the Menu tab and the menu button (index 7) open it; a press
//      on a bar button that slides off onto the layer before it is let go does not click the layer (Map stays open)
//   3. world pointer: the ray finds an object in front, B (right click) opens the viewer's menu there, the trigger on
//      its Sit item - pointed at on the HUDs layer - sits the avatar ON THE OBJECT
//   4. Big Screen and back: no 3D surface left over
//   node e2e_vr3d.mjs <lan_url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync, appendFileSync, copyFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { homedir } from 'node:os';

const [LAN, OUT, FAKE, CTL] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const H = resolve(homedir(), '.cache/skyband');
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }
const leap = (pump, notation) => appendFileSync(resolve(H, 'leap.fifo'), `${pump}\t${notation}\n`);
// the reply to a LEAP getInfo for `path`: the last IN line of leap.log naming it
async function leapInfo(path) {
    leap('LLWindow', `{'op':'getInfo','path':'${path}','reply':'@@REPLY@@'}`);
    await sleep(700);
    const lines = readFileSync(resolve(H, 'leap.log'), 'utf8').split('\n').filter(l => l.startsWith('IN ') && l.includes('"rect"') && l.includes(path.replace('//', '')));
    if (!lines.length) return null;
    try { const j = JSON.parse(lines[lines.length - 1].replace(/^IN \S+ /, '')); return j.data || j; } catch (e) { return null; }
}

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--remote-debugging-port=9386', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
// never leave the browser behind (a stray tab would keep reconnecting to the next test viewer)
process.on('exit', () => { try { chrome.kill(); } catch (e) { /* gone */ } });
process.on('uncaughtException', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
process.on('unhandledRejection', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9386/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map(); const events = [];
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } else if (d.method) events.push(d); };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => { const r = await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true }); return r.result?.result?.value; };
const grab = async (file) => { const d = await evalJs('window.__wolfvr.grab()'); if (d) writeFileSync(resolve(OUT, file), Buffer.from(d.split(',')[1], 'base64')); return !!d; };
const RIGHT = [0.2, 1.4, -0.2];
async function aimAt(p) {
    const dx = p[0] - RIGHT[0], dy = p[1] - RIGHT[1], dz = p[2] - RIGHT[2];
    const n = Math.hypot(dx, dy, dz);
    await evalJs(`window.__fakeCtl.aim('right', ${Math.atan2(-dx / n, -dz / n)}, ${Math.asin(dy / n)})`);
}
const press = async (hand, i, down) => evalJs(`window.__fakeCtl.press('${hand}', ${i}, ${down})`);
async function click(hand, i, holdMs = 250) { await press(hand, i, true); await sleep(holdMs); await press(hand, i, false); await sleep(300); }
// best horizontal shift of the right half against the left in a band of rows (sum of absolute grey differences)
const DISPARITY = `async (y0, y1, maxShift) => {
    const g = window.__wolfvr.grab(); if (!g) return null;
    const img = new Image(); img.src = g; await img.decode();
    const c = document.createElement('canvas'); c.width = img.width; c.height = img.height;
    const x = c.getContext('2d'); x.drawImage(img, 0, 0);
    const W = img.width / 2, H = img.height, d = x.getImageData(0, 0, img.width, H).data;
    const grey = (px, py) => { const i = (py * img.width + px) * 4; return d[i] * 0.3 + d[i + 1] * 0.59 + d[i + 2] * 0.11; };
    const r0 = Math.floor(H * y0), r1 = Math.floor(H * y1);
    let best = -1, bestSad = Infinity, zeroSad = 0;
    for (let s = 0; s <= maxShift; s++) {
        let sad = 0, n = 0;
        for (let py = r0; py < r1; py += 2) for (let px = maxShift + 8; px < W - 8; px += 2) { sad += Math.abs(grey(px, py) - grey(W + px - s, py)); n++; }
        sad /= n; if (s === 0) zeroSad = sad;
        if (sad < bestSad) { bestSad = sad; best = s; }
    }
    return { shift: best, sad: +bestSad.toFixed(2), sadAtZero: +zeroSad.toFixed(2), w: img.width, h: H };
}`;

await send('Page.enable'); await send('Runtime.enable'); await send('Log.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: LAN });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'connected to the real viewer (not in VR yet: no picture is drawn for it)');
await evalJs("try { localStorage.setItem('wolfvr.headset', 'auto'); } catch (e) {}");
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!window.__fakeXR.session')), 'Enter VR');
check(await waitFor(() => evalJs('!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "world")'), 30000), 'in VR: VR 3D Mode from the real viewer');
check(await waitFor(() => evalJs('window.__wolfvr.config().w === 2 * window.__wolfvr.config().h'), 20000), 'side-by-side picture (w = 2 h): ' + JSON.stringify(await evalJs('[window.__wolfvr.config().w, window.__wolfvr.config().h]')));

// E2E_ONLY=world: only the world pointer part (quicker while working on it)
const ONLY = process.env.E2E_ONLY || '';
if (!ONLY) {
// ---- 1. stereo ----
const e1 = await waitFor(async () => { const e = await evalJs('window.__wolfvr.eyes()'); return e && e.hdr && e.hdr.tan && e.cameraMasks.length === 2 ? e : null; }, 20000);
console.log('   XR views (eye, projection matrix, printed once for the test):');
for (const v of (e1 && e1.views) || []) console.log(`     ${v.eye}: [${v.proj.join(', ')}]`);
console.log('   eye windows sent (tangents L,R,B,T per eye):', JSON.stringify(e1 && e1.tan), ' in the picture header:', JSON.stringify(e1 && e1.hdr.tan));
console.log('   eye offsets in the header (m):', JSON.stringify(e1 && e1.hdr.eyes), ' layers:', JSON.stringify(e1 && { eyeLayer: e1.eyeLayer, halfLayers: e1.halfLayers, cameraMasks: e1.cameraMasks }));
const routed = (e) => !!e && (e.halfLayers[0] & e.cameraMasks[e.eyeLayer[0] - 1]) && !(e.halfLayers[0] & e.cameraMasks[e.eyeLayer[1] - 1]) &&
    (e.halfLayers[1] & e.cameraMasks[e.eyeLayer[1] - 1]) && !(e.halfLayers[1] & e.cameraMasks[e.eyeLayer[0] - 1]);
check(!!e1 && e1.hdr.stereo && e1.visible[0] && e1.visible[1] && !e1.mono, 'two eye halves shown, no single-view screen');
check(!!e1 && e1.views[0].eye === 'left' && e1.eyeLayer[0] === 1 && routed(e1), 'left half only on the left-eye XR camera (view 0 = left), right half only on the right');
check(!!e1 && Math.abs(e1.hdr.eyes[3] - e1.hdr.eyes[0] - 0.064) < 0.002, 'the halves are drawn from eyes 64 mm apart (the fake headset IPD)');
check(!!e1 && e1.tan.every((x, k) => Math.abs(x - e1.hdr.tan[k]) < 1e-4), 'each half drawn for exactly the eye window the page sent (to float precision)');
await evalJs('window.__swapViews = true'); await sleep(800);
const e2 = await evalJs('window.__wolfvr.eyes()');
check(!!e2 && e2.views[0].eye === 'right' && e2.eyeLayer[0] === 2 && routed(e2), 'right eye listed first: the left half moves to the layer of the left-eye camera (' + JSON.stringify(e2 && e2.eyeLayer) + ')');
await evalJs('window.__swapViews = false'); await sleep(500);
await evalJs('window.__fakeXR.setPitch(-0.45)'); await sleep(4000);
await grab('s_down.jpg');
const near = await evalJs(`(${DISPARITY})(0.80, 0.97, 120)`), far = await evalJs(`(${DISPARITY})(0.02, 0.12, 120)`);
console.log('   disparity near band (bottom, ground at the feet):', JSON.stringify(near), ' far band (top):', JSON.stringify(far));
check(!!near && !!far && near.shift > far.shift + 3, 'near things shift between the eyes more than far ones (near ' + (near && near.shift) + ' px, far ' + (far && far.shift) + ' px)');
// the viewer's own eye images, for measuring (WolfVRDebugEyeDump writes them to its logs folder and switches itself off)
leap('LLViewerControl', "{'op':'set','group':'Global','key':'WolfVRDebugEyeDump','value':true}");
await sleep(2500);
const LOGS = resolve(H, 'prof/logs');
const keep = (tag) => { for (const e of ['left', 'right']) { try { copyFileSync(resolve(LOGS, `wolfvr_eye_${e}.png`), resolve(OUT, `eye_${tag}_${e}.png`)); } catch (x) { /* not written */ } } };
keep('behind');
// [VR VIEW] Hide Me on the bar: first person, the avatar not drawn; the eyes again
await click('left', 7);
await aimAt(await evalJs('window.__wolfvr.slotPoint("hideme")')); await sleep(300); await click('right', 0);
check(await waitFor(() => evalJs('window.__wolfvr.bar().hide === true'), 6000), 'Hide Me on the bar: the viewer switches to the first-person view with the avatar hidden');
await click('left', 7);
await sleep(2000);
leap('LLViewerControl', "{'op':'set','group':'Global','key':'WolfVRDebugEyeDump','value':true}");
await sleep(2500);
keep('hidden');
await click('left', 7);
await aimAt(await evalJs('window.__wolfvr.slotPoint("hideme")')); await sleep(300); await click('right', 0);
check(await waitFor(() => evalJs('window.__wolfvr.bar().hide === false'), 6000), 'Show Me: back to seeing the avatar from behind');
await click('left', 7);
await evalJs('window.__fakeXR.setPitch(0)'); await sleep(1500);

// ---- 2. HUD clicks ----
await evalJs("window.__fakeCtl.thumbrest('left', false)"); await sleep(300);
await evalJs("window.__fakeCtl.thumbrest('left', true)"); await sleep(300);
await evalJs("window.__fakeCtl.thumbrest('left', false)"); await sleep(300);
check(!(await evalJs('window.__wolfvr.bar().open')), 'the thumb on and off the thumbrest does not open the bar');
check(await evalJs('window.__wolfvr.surfaces().tab'), 'the Menu tab shows while the bar is closed');
await aimAt(await evalJs('window.__wolfvr.tabPoint()')); await sleep(300);
await click('right', 0);
check(await evalJs('window.__wolfvr.bar().open'), 'pointing at the Menu tab and pulling the trigger opens the bar');
await click('left', 7);
check(!(await evalJs('window.__wolfvr.bar().open')), 'the menu button (index 7) closes it');
await click('left', 7);
check(await evalJs('window.__wolfvr.bar().open'), 'and opens it again');
const mapPt = await evalJs('window.__wolfvr.slotPoint("map")');
await aimAt(mapPt); await sleep(300);
await press('right', 0, true); await sleep(300);
await aimAt(await evalJs('window.__wolfvr.layerPoint(0.5, 0.5)')); await sleep(700);   // slide off the bar, still held
await press('right', 0, false); await sleep(300);
check(await waitFor(() => evalJs('window.__wolfvr.bar().on.includes("map")'), 6000), 'Map opened by the press on the bar');
await sleep(2500);
check(await evalJs('window.__wolfvr.bar().on.includes("map")'), 'and still open after the trigger was let go over the layer (no second click, no select double)');
await aimAt(mapPt); await sleep(300); await click('right', 0);
check(await waitFor(() => evalJs('!window.__wolfvr.bar().on.includes("map")'), 6000), 'Map closed again from the bar');
await click('left', 7);

}
// ---- 3. world pointer: point at an object, right click, Sit ----
// a modal notice the region may have shown at log-in (e.g. CurrencyURIOverrideReceived on The Caribbean) holds the
// mouse until it is answered, in the headset as on the desktop: answer it first (Enter = its default button)
for (let k = 0; k < 2; k++) { leap('LLWindow', "{'op':'keyDown','keycode':i129}"); leap('LLWindow', "{'op':'keyUp','keycode':i129}"); await sleep(600); }
// candidates: objects (not the ground) under the ray, upright things first (a level ray), each tried until one can be
// sat on - the grid decides what has room to sit ("There is no suitable surface to sit on" for a paving slab)
// a wide sweep (the avatar turns 45 degrees between sweeps): objects the viewer says can be sat on with a click
// (cursor "sit") first, then objects standing above the floor (the hit 0.3 m or more above it), nearest first
const YAWS = [];
for (let y = -0.9; y <= 0.91; y += 0.1) YAWS.push(+y.toFixed(2));
const cands = [];
for (let turn = 0; turn < 8 && !cands.some((c) => c.c === 'sit'); turn++) {
    for (const pitch of [0.15, 0.05, -0.05, -0.15, -0.25]) {
        for (const yaw of YAWS) {
            await evalJs(`window.__fakeCtl.aim('right', ${yaw}, ${pitch})`);
            await sleep(110);
            const a = await evalJs('window.__wolfvr.aim()');
            const hitY = 1.4 + Math.sin(pitch) * (a ? a.d : 0);   // the fake controller is 1.4 m above the floor
            if (a && a.k === 'world' && a.o && a.d > 0.8 && a.d < 30 && (a.c === 'sit' || hitY > 0.3)) cands.push({ yaw, pitch, turn, hitY: +hitY.toFixed(2), ...a });
        }
    }
    if (!cands.some((c) => c.c === 'sit')) { await evalJs("window.__fakeCtl.axis('right', 2, 1)"); await sleep(300); await evalJs("window.__fakeCtl.axis('right', 2, 0)"); await sleep(1500); }
}
cands.sort((a, b) => (b.c === 'sit') - (a.c === 'sit') || a.d - b.d);
console.log('   objects under the ray:', cands.length, JSON.stringify(cands.slice(0, 3)));
check(cands.length > 0 && cands[0].reticle, 'the ray finds objects in the world and the dot is drawn on them (' + (cands[0] && cands[0].d) + ' m)');
let menuSeen = false, uiSeen = false, sat = false, tries = 0;
for (const found of cands) {
    if (sat || tries >= 8) break;
    tries++;
    await evalJs(`window.__fakeCtl.aim('right', ${found.yaw}, ${found.pitch})`); await sleep(400);
    if (found.c === 'sit') {
        // a sit-on-click object: the trigger is the left click, and the viewer sits on it as the desktop would
        await click('right', 0, 200);
        sat = !!(await waitFor(() => evalJs('window.__wolfvr.bar().sit === 2'), 6000));
        console.log(`   try ${tries}: sit-on-click object ${found.d} m, trigger: ${sat ? 'seated' : 'not seated'}`);
        if (sat) break;
        await evalJs(`window.__fakeCtl.aim('right', ${found.yaw}, ${found.pitch})`); await sleep(400);
    }
    await click('right', 5, 200);   // B = right click
    await sleep(1200);
    let item = null;
    for (const p of ['//Pie Object Sit', '//Object Sit']) { const i = await leapInfo(p); if (i && i.visible_chain && i.rect) { item = { path: p, ...i }; break; } }
    const root = await leapInfo('//main_view');
    if (!item || !root) { leap('LLWindow', "{'op':'keyDown','keycode':i129}"); continue; }
    menuSeen = true;
    if (tries === 1) await grab('w_menu.jpg');
    // where to point: a context-menu item at its own rect's centre; a pie slice (its rect is not where it is drawn)
    // at its place round the pie's centre - "Sit Here" is the 4th slice of menu_pie_object.xml (Open, Create, Touch,
    // Sit Here), and slice i is drawn at (PIE_X[i], PIE_Y[i]) from the centre: piemenu.cpp PIE_X[3] = -45, PIE_Y[3] = 44
    let cx = (item.rect.left + item.rect.right) / 2, cy = (item.rect.top + item.rect.bottom) / 2;
    if (/PieSlice/.test(item.class)) {
        const pie = await leapInfo('//Object Pie');
        if (pie && pie.rect) { cx = (pie.rect.left + pie.rect.right) / 2 - 45; cy = (pie.rect.top + pie.rect.bottom) / 2 + 44; }
    }
    const u = cx / root.rect.right, v = 1 - cy / root.rect.top;
    await aimAt(await evalJs(`window.__wolfvr.layerPoint(${u}, ${v})`)); await sleep(500);
    const a2 = await evalJs('window.__wolfvr.aim()');
    uiSeen = uiSeen || (a2 && a2.k === 'ui');
    await click('right', 0, 200);
    sat = !!(await waitFor(() => evalJs('window.__wolfvr.bar().sit === 2'), 5000));
    console.log(`   try ${tries}: object ${found.d} m (${found.hitY} m up) at yaw ${found.yaw} pitch ${found.pitch} turn ${found.turn}, menu Sit at ${cx},${cy}: ${sat ? 'seated' : 'not seated'}`);
}
check(menuSeen, "B opened the viewer's own menu on an object, with Sit in it");
check(uiSeen, 'pointing at Sit on the HUDs layer: the viewer says the ray is on its UI');
check(sat, 'the trigger on Sit sits the avatar ON an object (after ' + tries + ' tries)');
if (sat) {
    await sleep(2500);
    await grab('w_seated.jpg');
    await click('left', 7);
    await aimAt(await evalJs('window.__wolfvr.slotPoint("sit")')); await sleep(300); await click('right', 0);
    check(await waitFor(() => evalJs('window.__wolfvr.bar().sit === 0'), 8000), 'Stand from the bar');
    await click('left', 7);
}

if (!ONLY) {
// ---- 4. Big Screen and back ----
await press('left', 3, true); await sleep(200); await press('left', 3, false);
const big = await waitFor(async () => { const s = await evalJs('window.__wolfvr.surfaces()'); return s && s.big ? s : null; }, 15000);
console.log('   surfaces in Big Screen:', JSON.stringify(big));
check(!!big && !big.eyes[0] && !big.eyes[1] && !big.mono && !big.layer && !big.bar && !big.tab && !big.reticle && !big.check, 'Big Screen: only the curved screen, nothing of 3D Mode left');
await press('left', 3, true); await sleep(200); await press('left', 3, false);
const back = await waitFor(async () => { const s = await evalJs('window.__wolfvr.surfaces()'); return s && !s.big && s.eyes[0] ? s : null; }, 20000);
console.log('   surfaces back in 3D:', JSON.stringify(back));
check(!!back && !back.big && back.tab, '3D Mode again: the eyes and the Menu tab, no big screen');
}

// (the page's own reconnect attempts after the viewer is closed at the end are not errors of the page)
const errs = events.filter(e => e.method === 'Runtime.exceptionThrown' || (e.method === 'Log.entryAdded' && e.params.entry.level === 'error' && e.params.entry.source !== 'network')).slice(0, 8);
for (const e of errs) console.log('ERR', JSON.stringify(e.params).slice(0, 400));
check(errs.length === 0, 'no page errors');
await evalJs('window.__wolfvr.toggleVR()');
ws.close(); chrome.kill();
console.log(failures ? `E2E VR3D FAILED (${failures})` : 'E2E VR3D PASSED');
process.exit(failures ? 1 : 0);
