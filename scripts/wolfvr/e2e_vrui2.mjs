// [VR KEYBOARD / PANEL CLOSE / BAR IN THE ROOM / VR WINDOW / ALLOW-LIST 2026-11-04] Paul's asks after his "VERY VERY
// CLOSE" test, against the REAL viewer (logged in, VR 3D Mode, stream on <url>, LEAP relay FIFO at ~/.cache/skyband):
//   1. the VR window (QR code) is hidden while a headset is connected and is never a panel
//   2. the bar stands in the room: opens in front, stays when the head turns, the grip moves it, opens in front again
//   3. every panel has its own Close: the mini-map closes from it
//   4. the HUD band carries no navigation / favourites bar (the junk Paul saw)
//   5. the headset keyboard opens by itself in nearby chat and types what each layout's keys mean (UK £, German ä ß,
//      French é à), read back from the chat box
//   6. after the page goes, the VR window is back
//   node e2e_vrui2.mjs <url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
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
let rq = 0;
// a LEAP request with a reply: the IN line of leap.log carrying our reqid
async function leapAsk(pump, notation) {
    const id = 'q' + (++rq) + '_' + Date.now();
    leap(pump, notation.replace('}', `,'reply':'@@REPLY@@','reqid':'${id}'}`));
    for (let i = 0; i < 20; i++) {
        await sleep(250);
        const l = readFileSync(resolve(H, 'leap.log'), 'utf8').split('\n').filter((x) => x.startsWith('IN ') && x.includes(`"${id}"`)).pop();
        if (l) { try { const j = JSON.parse(l.replace(/^IN \S+ /, '')); return j.data || j; } catch (e) { return null; } }
    }
    return null;
}
async function leapInfo(path) {
    leap('LLWindow', `{'op':'getInfo','path':'${path}','reply':'@@REPLY@@'}`);
    await sleep(700);
    const lines = readFileSync(resolve(H, 'leap.log'), 'utf8').split('\n').filter(l => l.startsWith('IN ') && l.includes('"rect"') && l.includes(path.replace('//', '')));
    if (!lines.length) return null;
    try { const j = JSON.parse(lines[lines.length - 1].replace(/^IN \S+ /, '')); return j.data || j; } catch (e) { return null; }
}
const vrWindowVisible = async () => { const r = await leapAsk('LLFloaterReg', "{'op':'instanceVisible','name':'wolf_vr_stream'}"); return r ? !!r.visible : null; };
const chatValue = async () => { const r = await leapAsk('UI', "{'op':'getValue','path':'//chat_box'}"); return r ? String(r.value ?? '') : null; };

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--remote-debugging-port=9393', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
process.on('exit', () => { try { chrome.kill('SIGKILL'); } catch (e) { /* gone */ } });
process.on('uncaughtException', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
process.on('unhandledRejection', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9393/json')).json(); } catch (e) { await sleep(250); } }
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
async function click(hand, i, holdMs = 200) { await press(hand, i, true); await sleep(holdMs); await press(hand, i, false); await sleep(350); }
async function openBar() {
    if (await evalJs('window.__wolfvr.bar().open')) return true;
    await aimAt(await evalJs('window.__wolfvr.tabPoint()')); await sleep(300);
    await click('right', 0);
    return !!(await waitFor(() => evalJs('window.__wolfvr.bar().open'), 3000));
}
async function pressSlot(slot) { await aimAt(await evalJs(`window.__wolfvr.slotPoint("${slot}")`)); await sleep(300); await click('right', 0); }
const shot = async (f) => { const d = await evalJs('window.__wolfvr.renderEye(0)'); if (d) writeFileSync(resolve(OUT, f), Buffer.from(d.split(',')[1], 'base64')); };

// the VR window open on the desktop first, as a resident has it when connecting
leap('LLFloaterReg', "{'op':'showInstance','name':'wolf_vr_stream'}");
await sleep(1500);
check(await vrWindowVisible() === true, 'the VR window is open on the desktop before the headset connects');
await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: URL_ });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'connected to the real viewer');
check(await waitFor(async () => (await vrWindowVisible()) === false, 6000), '1. the VR window hides itself while the headset is connected');
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "world")'), 30000), 'in VR: VR 3D Mode');
await waitFor(() => evalJs('window.__wolfvr.layer().visible'), 20000);
for (let k = 0; k < 2; k++) { leap('LLWindow', "{'op':'keyDown','keycode':i129}"); leap('LLWindow', "{'op':'keyUp','keycode':i129}"); await sleep(500); }
await sleep(2000);
check(!(await evalJs('window.__wolfvr.panels()')).some((p) => /wolf_vr_stream/.test(p.k)), '1. the VR window is never a panel');

// ---- 2. the bar in the room ----
check(await openBar(), '2. the Menu tab opens the bar');
const b0 = await evalJs('window.__wolfvr.barPlace()');
await evalJs('window.__fakeXR.setYaw(0.6)'); await sleep(800);   // the head turns ~34 degrees left
const b1 = await evalJs('window.__wolfvr.barPlace()');
check(b0 && b1 && Math.abs(b0.yaw - b1.yaw) < 0.5, `2. the bar stays put while the head turns (yaw ${b0 && b0.yaw} -> ${b1 && b1.yaw})`);
await evalJs('window.__fakeXR.setYaw(0)'); await sleep(600);
await aimAt(await evalJs('window.__wolfvr.slotPoint("chat")')); await sleep(300);
await press('right', 1, true); await sleep(300);
const ctr = await evalJs('window.__wolfvr.slotPoint("chat")');
const base = Math.atan2(-(ctr[0] - RIGHT[0]), -(ctr[2] - RIGHT[2]));
for (let k = 1; k <= 4; k++) { await evalJs(`window.__fakeCtl.aim('right', ${base + k * 5 * Math.PI / 180}, ${Math.asin((ctr[1] - RIGHT[1]) / Math.hypot(ctr[0] - RIGHT[0], ctr[1] - RIGHT[1], ctr[2] - RIGHT[2]))})`); await sleep(200); }
await press('right', 1, false); await sleep(400);
const b2 = await evalJs('window.__wolfvr.barPlace()');
check(b2 && Math.abs((b2.yaw - b0.yaw) - 20) < 6, `2. the grip moves the bar round (yaw ${b0 && b0.yaw} -> ${b2 && b2.yaw}, about +20 wanted)`);
await shot('eye_bar.png');
await aimAt(await evalJs('window.__wolfvr.closePoint()')); await sleep(300); await click('right', 0);
await evalJs('window.__fakeXR.setYaw(-0.5)'); await sleep(600);
check(await openBar(), '2. the bar opens again');
const b3 = await evalJs('window.__wolfvr.barPlace()');
check(b3 && Math.abs(b3.yaw - (-0.5 * 180 / Math.PI)) < 3, `2. ... in front of where the head looks now (yaw ${b3 && b3.yaw}, ${(-0.5 * 180 / Math.PI).toFixed(1)} wanted)`);
await evalJs('window.__fakeXR.setYaw(0)'); await sleep(400);
await aimAt(await evalJs('window.__wolfvr.closePoint()')); await sleep(300); await click('right', 0);

// ---- 3. a panel's own Close, on the mini-map ----
check(await openBar(), '3. the bar opens for the mini-map');
await pressSlot('minimap');
const mm = await waitFor(async () => (await evalJs('window.__wolfvr.panels()')).find((p) => /mini_?map/i.test(p.k) && p.visible), 8000);
check(!!mm, '3. the mini-map is its own panel: ' + JSON.stringify(mm && { k: mm.k, yaw: mm.yaw, arc: mm.arc }));
await sleep(1200);
await shot('eye_minimap_close.png');
if (mm) {
    await aimAt(await evalJs(`window.__wolfvr.panelClosePoint(${JSON.stringify(mm.k)})`)); await sleep(500);
    await click('right', 0);
    check(await waitFor(() => evalJs('!window.__wolfvr.bar().on.includes("minimap")'), 6000), '3. the Close drawn on its panel closes the mini-map in the viewer');
    check(await waitFor(async () => !(await evalJs('window.__wolfvr.panels()')).some((p) => /mini_?map/i.test(p.k)), 6000), '3. ... and its panel goes');
}

// ---- 4. no navigation / favourites bar on the HUD band ----
const g = await evalJs('window.__wolfvr.grabLayer()');
if (g) writeFileSync(resolve(OUT, 'layer.png'), Buffer.from(g.split(',')[1], 'base64'));
const topRows = await evalJs(`(async () => { const g = window.__wolfvr.grabLayer(); if (!g) return null; const im = new Image(); im.src = g; await im.decode();
    const c = document.createElement('canvas'); c.width = im.width; c.height = im.height; const x = c.getContext('2d'); x.drawImage(im, 0, 0);
    const H2 = im.height / 2, d = x.getImageData(0, H2, im.width, Math.floor(H2 * 0.08)).data; let n = 0;   // the coverage half's top 8 %
    for (let i = 0; i < d.length; i += 4) if (d[i] > 16) n++; return n; })()`);
check(topRows === 0, `4. nothing drawn along the top of the band where the navigation and favourites bars were (${topRows} covered pixels)`);

// ---- 5. the keyboard: opens in nearby chat, each layout types what its keys mean ----
check(await openBar(), '5. the bar opens for Chat');
await pressSlot('chat');
await sleep(2500);
const cb = await leapInfo('//chat_box'), root = await leapInfo('//main_view');
const chatPanel = (await evalJs('window.__wolfvr.panels()')).find((p) => {
    if (!cb || !root) return false;
    const u = ((cb.rect.left + cb.rect.right) / 2) / root.rect.right, v = 1 - ((cb.rect.top + cb.rect.bottom) / 2) / root.rect.top;
    return u >= p.r[0] && u <= p.r[2] && v >= p.r[1] && v <= p.r[3];
});
if (cb && root) {
    const u = ((cb.rect.left + cb.rect.right) / 2) / root.rect.right, v = 1 - ((cb.rect.top + cb.rect.bottom) / 2) / root.rect.top;
    await aimAt(await evalJs(`window.__wolfvr.windowPoint(${u}, ${v})`)); await sleep(500);
    await click('right', 0);
}
check(!!chatPanel, '5. nearby chat is a panel');
check(await waitFor(() => evalJs('window.__wolfvr.keyboard().open'), 4000), '5. a click in the chat box opens the keyboard by itself');
await sleep(600);
await shot('eye_keyboard.png');
async function typeKeys(labels) {
    for (const l of labels) {
        const p = await evalJs(`window.__wolfvr.keyPoint(${JSON.stringify(l)})`);
        if (!p) { console.log('   no key', l, JSON.stringify(await evalJs('window.__wolfvr.keyboard().keys'))); return false; }
        await aimAt(p); await sleep(250); await click('right', 0, 150);
    }
    return true;
}
for (const [langs, want, keys, expect] of [
    [['en-GB', 'en'], 'gb', ['h', 'i', 'Space', '?123', '£', '5', 'ABC'], 'hi £5'],
    [['de-DE'], 'de', ['Space', 'ä', 'ß', 'z'], ' äßz'],
    [['fr-FR'], 'fr', ['Space', 'é', 'à', 'a'], ' éàa'],
]) {
    const k = await evalJs(`window.__wolfvr.kbLanguages(${JSON.stringify(langs)})`);
    check(k === want, `5. ${langs[0]} gives the ${want} layout (${k})`);
    await sleep(400);
    await typeKeys(keys);
    await sleep(800);
}
const typed = await chatValue();
check(typed === 'hi £5 äßz éàa', `5. the chat box holds what was typed on the UK, German and French keyboards: ${JSON.stringify(typed)}`);
// clear it without sending (Backspace), and close the keyboard with its own Close
for (let i = 0; i < 14; i++) { const p = await evalJs('window.__wolfvr.keyPoint("\\u232b")'); if (p) { await aimAt(p); await sleep(120); await click('right', 0, 100); } }
await aimAt(await evalJs('window.__wolfvr.keyPoint("\\u2715  Close")')); await sleep(300); await click('right', 0);
check(!(await evalJs('window.__wolfvr.keyboard().open')), '5. the keyboard\'s Close closes it');
console.log('   device: ' + await evalJs('window.__wolfvr.deviceLine()'));

// ---- 6. the page goes: the VR window comes back ----
await evalJs('window.__wolfvr.toggleVR()');
await sleep(500);
await send('Page.navigate', { url: 'about:blank' });
check(await waitFor(async () => (await vrWindowVisible()) === true, 10000), '6. the headset gone, the VR window is back for the next person');
console.log(failures ? `${failures} FAILED` : 'ALL PASSED');
chrome.kill('SIGKILL');
process.exit(failures ? 1 : 0);
