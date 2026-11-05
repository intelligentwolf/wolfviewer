// [VR MIC FALLBACK 2026-11-05] Paul in VR with AI Mode: "the only way i got him to listen to my voice is to open the chat
// box" (his log: the headset microphone chosen, the page never opened it, voice heard pure silence). Against the REAL viewer
// (Shadow Testwolf, VR 3D Mode), headless Chromium with a fake microphone (a steady tone):
//   1. WolfVRHeadsetMic on, the page's microphone NOT opened: voice keeps the PC microphone (viewer log)
//   2. the page's "Use the headset microphone for voice" pressed in 2D, back into VR: voice switches to the headset
//      microphone, and AI Mode's ear hears it (its 30 s "ears" line: mic peak above 0)
//   node e2e_vrmic.mjs <url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
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
    '--ignore-certificate-errors', '--use-fake-device-for-media-stream', '--autoplay-policy=no-user-gesture-required', '--use-fake-ui-for-media-stream', '--remote-debugging-port=9393', '--window-size=1280,1280',
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

import { existsSync } from 'node:fs';
const LOG = resolve(H, 'prof/logs/WolfViewer.log');
const logHas = (re) => existsSync(LOG) && re.test(readFileSync(LOG, 'utf8'));
const lastEars = () => { const m = (existsSync(LOG) ? readFileSync(LOG, 'utf8') : '').match(/ears: mic \d+ samples \(peak (\d+)\)/g); return m ? m[m.length - 1] : ''; };
leap('LLViewerControl', "{'op':'set','group':'Global','key':'WolfVRHeadsetMic','value':true}");
await sleep(1000);
await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: URL_ });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'connected to the real viewer');
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "world")'), 30000), 'in VR: VR 3D Mode');
// ---- 1 ----
check(await waitFor(async () => logHas(/voice uses the PC microphone: no sound from the headset microphone yet/), 15000), '1. headset microphone chosen but not opened: voice keeps the PC microphone');
check(!logHas(/voice uses the headset microphone/), '1. ... and never switches to the silent headset microphone');
check(await openBar(), 'the bar opens');
await pressSlot('aimode');
check(await waitFor(() => evalJs('window.__wolfvr.bar().ai'), 15000), 'AI Mode on');
// the microphone only runs while it is open (Speak latches it, vrbar.js)
check(await openBar(), 'the bar opens');
await pressSlot('speak');
check(await waitFor(() => evalJs('window.__wolfvr.bar().on.includes("speak")'), 8000), 'Speak on: the microphone is open');
await sleep(35000);
console.log('   AI Mode ear, PC microphone: ' + lastEars());
// ---- 2 ----
await evalJs('window.__wolfvr.toggleVR()');
await sleep(3000);
await evalJs("document.getElementById('wvr-mic').click()");
await sleep(3000);
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "world")'), 30000), '2. back in VR');
check(await waitFor(async () => logHas(/voice uses the headset microphone \(its sound is arriving\)/), 15000), '2. the page\'s microphone opened: voice uses the headset microphone');
const before = lastEars();
check(!!(await waitFor(async () => { const e = lastEars(); const m = e.match(/peak (\d+)/); return e !== before && m && Number(m[1]) > 0 ? e : null; }, 70000, 2000)), '2. AI Mode hears the headset microphone: ' + lastEars());
check(await openBar(), 'the bar opens');
await pressSlot('aimode');
check(await waitFor(() => evalJs('!window.__wolfvr.bar().ai'), 8000), 'AI Mode off');
check(await openBar(), 'the bar opens');
await pressSlot('speak');
console.log(failures ? `${failures} FAILED` : 'ALL PASSED');
chrome.kill('SIGKILL');
process.exit(failures ? 1 : 0);
