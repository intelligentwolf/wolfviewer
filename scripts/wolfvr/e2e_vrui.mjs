// [VR MENU FIX / VR PANELS 2026-11-04] The VR 3D Mode interface after Paul's Quest 3 test ("i click [the Menu tab] but
// then i cant turn off the menu clicking any button on the menu does nothing ... a proper vr interface"), against the
// REAL viewer (logged in, WolfVRStreamMode "world", the stream on <url>, LEAP relay FIFO at ~/.cache/skyband/leap.fifo),
// in headless Chromium with WolfStorm's fake WebXR device and Touch Plus controllers. The left controller's menu button
// is NEVER pressed here: on the Quest the browser may keep it, so everything must work with the right trigger alone.
//   1. the Menu tab opens the bar; the bar's own Close button closes it
//   2. a bar button (Map) runs: the bar gets out of the way by itself and the tab confirms it ("Map" and a tick)
//   3. the map window is its OWN panel in the room (not on the head-locked band), and the band leaves it out
//   4. the ray on the panel's close button is on the viewer's UI there; the trigger closes the map, and the panel goes
//   5. a second window (Inventory) gets its own panel beside the first, not on top of it; the grip moves it round
//   node e2e_vrui.mjs <url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, appendFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { homedir } from 'node:os';

const [URL_, OUT, FAKE, CTL] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const H = resolve(homedir(), '.cache/skyband');
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

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--remote-debugging-port=9392', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
process.on('exit', () => { try { chrome.kill('SIGKILL'); } catch (e) { /* gone */ } });
process.on('uncaughtException', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
process.on('unhandledRejection', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9392/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => { const r = await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true }); return r.result?.result?.value; };
const RIGHT = [0.2, 1.4, -0.2];   // fake_xr_controllers.js: where the right controller is held
async function aimAt(p) {
    const dx = p[0] - RIGHT[0], dy = p[1] - RIGHT[1], dz = p[2] - RIGHT[2];
    const n = Math.hypot(dx, dy, dz);
    await evalJs(`window.__fakeCtl.aim('right', ${Math.atan2(-dx / n, -dz / n)}, ${Math.asin(dy / n)})`);
}
const press = async (hand, i, down) => evalJs(`window.__fakeCtl.press('${hand}', ${i}, ${down})`);
async function click(hand, i, holdMs = 250) { await press(hand, i, true); await sleep(holdMs); await press(hand, i, false); await sleep(600); }
async function openBar() {
    if (await evalJs('window.__wolfvr.bar().open')) return true;
    await aimAt(await evalJs('window.__wolfvr.tabPoint()')); await sleep(300);
    await click('right', 0);
    return !!(await waitFor(() => evalJs('window.__wolfvr.bar().open'), 3000));
}
async function pressSlot(slot) { await aimAt(await evalJs(`window.__wolfvr.slotPoint("${slot}")`)); await sleep(300); await click('right', 0); }
// the panel point over a viewer view (LEAP rect, UI-scaled, origin bottom-left) inside the panel `k`
async function panelPointOver(k, path) {
    const info = await leapInfo(path), root = await leapInfo('//main_view');
    const pan = (await evalJs('window.__wolfvr.panels()')).find((p) => p.k === k);
    if (!info || !info.rect || !root || !pan) return null;
    const wu = ((info.rect.left + info.rect.right) / 2) / root.rect.right, wv = 1 - ((info.rect.top + info.rect.bottom) / 2) / root.rect.top;
    const [x0, y0, x1, y1] = pan.r;
    return evalJs(`window.__wolfvr.panelPoint(${JSON.stringify(k)}, ${(wu - x0) / (x1 - x0)}, ${(wv - y0) / (y1 - y0)})`);
}

await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: URL_ });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'connected to the real viewer');
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "world")'), 30000), 'in VR: VR 3D Mode');
if (process.env.QUALITY) { await evalJs(`window.__wolfvr.setQuality(${process.env.QUALITY})`); await sleep(6000); }   // e.g. 0 = Paul's 1920 px eyes
await waitFor(() => evalJs('window.__wolfvr.layer().visible'), 20000);
console.log('   config ' + JSON.stringify(await evalJs('[window.__wolfvr.config().w, window.__wolfvr.config().h]')));
// a modal notice the region may have shown at log-in holds the mouse until answered (Escape, as on the desktop)
for (let k = 0; k < 2; k++) { leap('LLWindow', "{'op':'keyDown','keycode':i129}"); leap('LLWindow', "{'op':'keyUp','keycode':i129}"); await sleep(600); }

// ---- 1. open and close with the right trigger only ----
check(await openBar(), 'the Menu tab opens the bar (right trigger)');
await aimAt(await evalJs('window.__wolfvr.closePoint()')); await sleep(400);
check((await evalJs('window.__wolfvr.bar().hover'))?.kind === 'close', 'the ray on the bar\'s Close button lights it');
await click('right', 0);
check(await waitFor(() => evalJs('!window.__wolfvr.bar().open'), 3000), 'the Close button closes the bar (no menu button needed)');
check(await evalJs('window.__wolfvr.surfaces().tab'), 'and the Menu tab is back');

// ---- 2. a button runs, the bar gets out of the way, the tab confirms ----
check(await openBar(), 'the bar opens again');
await pressSlot('map');
check(await waitFor(() => evalJs('!window.__wolfvr.bar().open'), 2000), 'pressing Map closes the bar by itself (the map can be seen)');
const said = await waitFor(async () => { const s = await evalJs('window.__wolfvr.tabText()'); return s.includes('✓') ? s : null; }, 6000);
check(!!said, `the Menu tab confirms what was done: "${said}"`);
check(await waitFor(() => evalJs('window.__wolfvr.bar().on.includes("map")'), 6000), 'the world map is open in the viewer');

// ---- 3. the map is its own panel ----
const mapPanel = await waitFor(async () => (await evalJs('window.__wolfvr.panels()')).find((p) => /world_?map/i.test(p.k) && p.visible), 8000);
check(!!mapPanel, 'the world map is its own panel in the room: ' + JSON.stringify(mapPanel));
await sleep(1500);
// what the headset's left eye draws now (the page's own scene: world, HUD band, panels, bar), and its own report
for (const [f, dx] of [['eye_map.png', 0]]) { const d = await evalJs(`window.__wolfvr.renderEye(${dx})`); if (d) writeFileSync(resolve(OUT, f), Buffer.from(d.split(',')[1], 'base64')); }
console.log('   device: ' + await evalJs('window.__wolfvr.deviceLine()'));
writeFileSync(resolve(OUT, 'panels_map.json'), JSON.stringify(await evalJs('window.__wolfvr.panels()'), null, 1));
// the band leaves it out: a ray through the map's place on the head-locked band does not land on the band
if (mapPanel) {
    const [x0, y0, x1, y1] = mapPanel.r;
    const r = await evalJs(`window.__wolfvr.bandHit(${(x0 + x1) / 2}, ${(y0 + y1) / 2})`);
    check(r === null, 'the HUD band leaves the map out (no band hit where it was): ' + JSON.stringify(r));
}

// ---- 4. its close button, pointed at on the panel, closes it ----
const closePt = mapPanel && await panelPointOver(mapPanel.k, '//worldmap/llfloater_close_btn');
if (closePt) { await aimAt(closePt); await sleep(700); }
const onClose = await evalJs('window.__wolfvr.aim()');
check(!!closePt && onClose && onClose.k === 'ui', 'the ray on the map panel\'s close button is on the viewer\'s UI: ' + JSON.stringify(onClose && { k: onClose.k, c: onClose.c }));
await click('right', 0);
check(await waitFor(() => evalJs('!window.__wolfvr.bar().on.includes("map")'), 6000), 'the trigger there closes the map in the viewer');
check(await waitFor(async () => !(await evalJs('window.__wolfvr.panels()')).some((p) => /world_?map/i.test(p.k)), 6000), 'and its panel goes');

// ---- 5. two windows side by side; the grip moves one ----
check(await openBar(), 'the bar opens for Inventory');
await pressSlot('inventory');
check(await waitFor(() => evalJs('window.__wolfvr.bar().on.includes("inventory")'), 6000), 'Inventory opened from the bar');
check(await openBar(), 'the bar opens for People');
await pressSlot('people');
check(await waitFor(() => evalJs('window.__wolfvr.bar().on.includes("people")'), 6000), 'People opened from the bar');
const two = await waitFor(async () => { const l = (await evalJs('window.__wolfvr.panels()')).filter((p) => p.visible); return l.length >= 2 ? l : null; }, 8000);
writeFileSync(resolve(OUT, 'panels_two.json'), JSON.stringify(two, null, 1));
const apart = two && two.every((a, i) => two.every((b, j) => i === j || Math.abs(a.yaw - b.yaw) >= (a.arc + b.arc) / 2));
check(!!two && apart, 'two windows: two panels side by side, not overlapping: ' + JSON.stringify(two && two.map((p) => ({ k: p.k, yaw: p.yaw, arc: p.arc }))));
if (two) {
    const p0 = two[0];
    await aimAt(await evalJs(`window.__wolfvr.panelPoint(${JSON.stringify(p0.k)}, 0.5, 0.5)`)); await sleep(500);
    await press('right', 1, true); await sleep(400);
    // swing the controller 25 degrees to the right, in steps, grip held
    const ctr = await evalJs(`window.__wolfvr.panelPoint(${JSON.stringify(p0.k)}, 0.5, 0.5)`);
    const base = Math.atan2(-(ctr[0] - RIGHT[0]), -(ctr[2] - RIGHT[2]));
    for (let k = 1; k <= 5; k++) { await evalJs(`window.__fakeCtl.aim('right', ${base - k * 5 * Math.PI / 180}, 0)`); await sleep(250); }
    await press('right', 1, false); await sleep(500);
    const after = (await evalJs('window.__wolfvr.panels()')).find((p) => p.k === p0.k);
    check(!!after && Math.abs((after.yaw - p0.yaw) + 25) < 8, `the grip moved the panel round the arc (yaw ${p0.yaw} -> ${after && after.yaw}, about -25 wanted)`);
}
for (const slot of ['inventory', 'people']) { if (await evalJs(`window.__wolfvr.bar().on.includes("${slot}")`)) { await openBar(); await pressSlot(slot); await sleep(800); } }

await evalJs('window.__wolfvr.toggleVR()');
await sleep(800);
console.log(failures ? `${failures} FAILED` : 'ALL PASSED');
chrome.kill('SIGKILL');
process.exit(failures ? 1 : 0);
