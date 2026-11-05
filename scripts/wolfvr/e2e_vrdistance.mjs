// [VR DRAW DISTANCE 2026-11-05] Paul: "add a button to the hud default it to 512m the button opens a floater with VR Draw
// distance which is seperate from the normal drawdistance". Against the REAL viewer at his Caribbean spot as a Quest 3, with
// his own Render values (RenderFarClip 4096 ...): VR Draw Distance 4096 for 200 s (the scene fills in), then the bar's
// Distance button opens the VR Draw Distance window as a headset panel, the distance goes to its default 512 for 90 s; the
// desktop's RenderFarClip stays 4096 throughout. Pictures a second come from the viewer log (VR frame parts every 30 s).
//   node e2e_vrdistance.mjs <url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
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

const setv = (k, v) => leap('LLViewerControl', `{'op':'set','group':'Global','key':'${k}','value':${v}}`);
const getv = async (k) => { const r = await leapAsk('LLViewerControl', `{'op':'get','group':'Global','key':'${k}'}`); return r ? r.value : null; };
check(Number(await getv('WolfVRDrawDistance')) === 512, 'VR Draw Distance starts at 512 m: ' + (await getv('WolfVRDrawDistance')));
for (const [k, v] of [['RenderFarClip', 'r4096'], ['RenderVolumeLODFactor', 'r3'], ['RenderFSAASamples', 'i3'], ['RenderHeroProbeResolution', 'i2048'],
                      ['RenderHeroProbeDistance', 'r6'], ['RenderMaxPartCount', 'i8192'], ['RenderAvatarMaxNonImpostors', 'i19']]) setv(k, v);
setv('WolfVRDrawDistance', 'r4096');
await sleep(1500);
await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: URL_ });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'connected to the real viewer');
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "world")'), 30000), 'in VR: VR 3D Mode');
console.log('PHASE vr4096 ' + new Date().toISOString());
await sleep(200000);
check(await openBar(), 'the bar opens');
check((await evalJs('window.__wolfvr.bar()')).ids.includes('vrdistance'), 'the bar has Distance');
await pressSlot('vrdistance');
const p = await waitFor(async () => (await evalJs('window.__wolfvr.panels()')).find((x) => /^wolf_vr_draw_distance/.test(x.k) && x.visible), 8000);
check(!!p, 'Distance opens the VR Draw Distance window as a headset panel: ' + JSON.stringify(p && { k: p.k, n: p.n }));
await sleep(1500);
const d = await evalJs('window.__wolfvr.renderEye(0)'); if (d) writeFileSync(resolve(OUT, 'eye_vrdistance.png'), Buffer.from(d.split(',')[1], 'base64'));
setv('WolfVRDrawDistance', 'r512');
console.log('PHASE vr512 ' + new Date().toISOString());
await sleep(90000);
check(Number(await getv('RenderFarClip')) === 4096, 'the desktop draw distance is still 4096 m: ' + (await getv('RenderFarClip')));
if (p) { await aimAt(await evalJs(`window.__wolfvr.panelClosePoint(${JSON.stringify(p.k)})`)); await sleep(500); await click('right', 0); }
check(!!(await waitFor(async () => !(await evalJs('window.__wolfvr.panels()')).some((x) => /^wolf_vr_draw_distance/.test(x.k)), 6000)), 'its Close closes it');
console.log('PHASE end ' + new Date().toISOString());
console.log(failures ? `${failures} FAILED` : 'ALL PASSED');
chrome.kill('SIGKILL');
process.exit(failures ? 1 : 0);
