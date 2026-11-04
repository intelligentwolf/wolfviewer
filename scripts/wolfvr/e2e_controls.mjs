// [VR CONTROLS / HUD LAYER 2026-11-04] The in-VR action bar and the UI layer against the REAL viewer (logged in,
// WolfVRStreamMode "world", stream on <lan_url>), with WolfStorm's fake WebXR device plus two fake controllers:
// the HUDs layer arrives, the left menu button opens the bar, the right controller AIMED at a slot (the real raycast)
// and the trigger run the viewer's own toolbar command (Map opens the world map floater, Sit sits), and the floater
// shows up on the layer. Saves l_*.png (the layer pictures: colour on top, coverage below).
//   node e2e_controls.mjs <lan_url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [LAN, OUT, FAKE, CTL] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--remote-debugging-port=9384', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9384/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map(); const events = [];
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } else if (d.method) events.push(d); };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => { const r = await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true }); return r.result?.result?.value; };
const grabLayer = async (file) => { const d = await evalJs('window.__wolfvr.grabLayer()'); if (d) writeFileSync(resolve(OUT, file), Buffer.from(d.split(',')[1], 'base64')); return !!d; };
const grab = async (file) => { const d = await evalJs('window.__wolfvr.grab()'); if (d) writeFileSync(resolve(OUT, file), Buffer.from(d.split(',')[1], 'base64')); return !!d; };
// aim the right controller at a world point: fake_xr_controllers.js matrix() forward = (-sin y cos p, sin p, -cos y cos p)
const RIGHT = [0.2, 1.4, -0.2];
async function aimAt(p) {
    const dx = p[0] - RIGHT[0], dy = p[1] - RIGHT[1], dz = p[2] - RIGHT[2];
    const n = Math.hypot(dx, dy, dz);
    const pitch = Math.asin(dy / n), yaw = Math.atan2(-dx / n, -dz / n);
    await evalJs(`window.__fakeCtl.aim('right', ${yaw}, ${pitch})`);
}
async function pull(i = 0) {
    await evalJs(`window.__fakeCtl.press('right', ${i}, true)`); await sleep(250);
    await evalJs(`window.__fakeCtl.press('right', ${i}, false)`); await sleep(250);
}
async function press(slot) {
    const p = await evalJs(`window.__wolfvr.slotPoint(${JSON.stringify(slot)})`);
    if (!p) return false;
    await aimAt(p); await sleep(300);
    const hov = await evalJs('window.__wolfvr.bar().hover');
    await pull();
    return hov;
}

await send('Page.enable'); await send('Runtime.enable'); await send('Log.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: LAN });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.config() && window.__wolfvr.config().mode === "world")'), 30000), 'VR 3D Mode from the real viewer');
await evalJs("try { localStorage.setItem('wolfvr.headset', 'auto'); } catch (e) {}");
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!window.__fakeXR.session')), 'Enter VR');
const ui = await waitFor(() => evalJs('window.__wolfvr.bar().region ? window.__wolfvr.bar() : null'), 15000);
check(!!ui, 'the viewer sends the action bar state (region ' + (ui && ui.region) + ', ' + (ui && ui.ids.length) + ' slots, ' + (ui && ui.fav) + ' favourites)');
console.log('   quality:', JSON.stringify(await evalJs('window.__wolfvr.quality()')));
const lay = await waitFor(() => evalJs('window.__wolfvr.layer().frames > 0 ? window.__wolfvr.layer() : null'), 20000);
check(!!lay, 'the HUDs layer arrives (' + (lay && lay.cfg && lay.cfg.w + 'x' + lay.cfg.h) + ')');
await sleep(1500);
await grabLayer('l_start.png');

// left menu tap (Touch index 7) = the action bar
await evalJs("window.__fakeCtl.press('left', 7, true)"); await sleep(200);
await evalJs("window.__fakeCtl.press('left', 7, false)"); await sleep(500);
check(await evalJs('window.__wolfvr.bar().open'), 'left menu button opens the action bar');

const f0 = await evalJs('window.__wolfvr.layer().frames');
const hov = await press('map');
check(hov && hov.kind === 'action', 'the right controller ray lands on the Map slot (' + JSON.stringify(hov) + ')');
check(await waitFor(() => evalJs('window.__wolfvr.bar().on.includes("map")'), 8000), 'trigger on Map opens the viewer\'s world map (toolbar command "map" running)');
check(await waitFor(async () => (await evalJs('window.__wolfvr.layer().frames')) > f0, 8000), 'the layer sends a new picture when the map opens');
await sleep(2500);
await grabLayer('l_map.png');
await press('map');
check(await waitFor(() => evalJs('!window.__wolfvr.bar().on.includes("map")'), 8000), 'trigger on Map again closes it');

await press('sit');
check(await waitFor(() => evalJs('window.__wolfvr.bar().sit > 0'), 8000), 'Sit sits down (on the ground)');
await sleep(1500);
await press('sit');
check(await waitFor(() => evalJs('window.__wolfvr.bar().sit === 0'), 8000), 'Stand stands up');

await press('huds');
check(await waitFor(() => evalJs('!window.__wolfvr.layer().on'), 3000), 'HUDs button hides the layer');
await press('huds');
check(await waitFor(() => evalJs('window.__wolfvr.layer().on'), 3000), 'HUDs button shows it again');

// close the bar, exit VR
await evalJs("window.__fakeCtl.press('left', 7, true)"); await sleep(200);
await evalJs("window.__fakeCtl.press('left', 7, false)"); await sleep(300);
check(!(await evalJs('window.__wolfvr.bar().open')), 'left menu button closes the bar');
await grab('w_end.jpg');
const errs = events.filter(e => e.method === 'Runtime.exceptionThrown' || (e.method === 'Log.entryAdded' && e.params.entry.level === 'error')).slice(0, 8);
for (const e of errs) console.log('ERR', JSON.stringify(e.params).slice(0, 400));
check(errs.length === 0, 'no page errors');
await evalJs('window.__wolfvr.toggleVR()');
ws.close(); chrome.kill();
console.log(failures ? `E2E CONTROLS FAILED (${failures})` : 'E2E CONTROLS PASSED');
process.exit(failures ? 1 : 0);
