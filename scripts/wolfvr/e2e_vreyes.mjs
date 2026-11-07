// [VR EYE OFFSCREEN 2026-11-07] Paul: 3D Mode "feels flat even standing still" - the picture is soft. Against the REAL
// viewer (logged in, VR 3D Mode, the stream on <url>, LEAP relay FIFO at ~/.cache/skyband/leap.fifo), headless Chromium
// with the fake headset and a Quest 3's own eye windows (__asymEyes, Paul's device line) at Paul's 1728 px step, head
// still: for each phase (viewer settings set through LEAP first, e.g. WolfVREyeOffscreen=false / true) the pictures
// a second, the stage breakdown, the XR layer line, the viewer's eye dump (WolfVRDebugEyeDump: the stereo picture as
// sent, both eyes, copied from the session's logs folder) and the headset's own render of the left eye.
//   node e2e_vreyes.mjs <url> <out_dir> <fake_xr.js> <fake_xr_controllers.js> <phase> [<phase> ...]
//   phase = label:Key=value,Key=value:seconds   (values as LEAP notation: true, false, 1.5, 'text')
// Not shipped to residents.
import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, appendFileSync, writeFileSync, copyFileSync, existsSync, statSync } from 'node:fs';
import { resolve } from 'node:path';
import { homedir } from 'node:os';

const [URL_, OUT, FAKE, CTL, ...PHASES] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const H = resolve(homedir(), '.cache/skyband');
const LOGS = resolve(H, 'prof/logs');   // session_vr.sh's throwaway profile, seen from outside its namespace
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const setting = (k, v) => appendFileSync(resolve(H, 'leap.fifo'), `LLViewerControl\t{'op':'set','group':'Global','key':'${k}','value':${v}}\n`);
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--autoplay-policy=no-user-gesture-required', '--remote-debugging-port=9397', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
process.on('exit', () => { try { chrome.kill('SIGKILL'); } catch (e) { /* gone */ } });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9397/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => (await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: URL_ });
console.log('connected', !!(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.config && window.__wolfvr.config())'), 40000)));
await evalJs(`window.__asymEyes = ${JSON.stringify([[-1.376, 0.839, -1.43, 0.96], [-0.839, 1.376, -1.43, 0.96]])}`);
await evalJs('window.__wolfvr.toggleVR()');
console.log('3D', !!(await waitFor(() => evalJs('!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "world")'), 30000)));
await evalJs('window.__wolfvr.setQuality(1)');    // Paul's Quest 3 step: 1728 px eyes at 80 Mbit/s
await evalJs('window.__fakeXR.setPitch(-0.35)');   // a little down: the near ground in the lower part, the distance above
await sleep(15000);                                // the region's textures in, the picture size settled
const results = [];
for (const ph of PHASES) {
    const [label, kv, secs] = ph.split(':');
    for (const pair of (kv || '').split(',').filter(Boolean)) { const [k, v] = pair.split('='); setting(k, v); }
    await sleep(10000);   // settle: the targets re-made, the textures' new detail in
    await evalJs('window.__wolfvr.resetStages && window.__wolfvr.resetStages()');
    await sleep(Number(secs || 25) * 1000);
    const r = { label, settings: kv, config: await evalJs('window.__wolfvr.config()'), rates: await evalJs('window.__wolfvr.rates()'),
                stages: await evalJs('window.__wolfvr.stages()'), xrLayer: await evalJs('window.__wolfvr.xrLayer()') };
    // the viewer's eye dump: the stereo picture exactly as it goes to the encoder (both eyes)
    const before = existsSync(resolve(LOGS, 'wolfvr_eye_left.png')) ? statSync(resolve(LOGS, 'wolfvr_eye_left.png')).mtimeMs : 0;
    setting('WolfVRDebugEyeDump', 'true');
    r.dump = !!(await waitFor(() => existsSync(resolve(LOGS, 'wolfvr_eye_left.png')) && statSync(resolve(LOGS, 'wolfvr_eye_left.png')).mtimeMs > before, 10000));
    await sleep(1500);
    if (r.dump) for (const e of ['left', 'right']) copyFileSync(resolve(LOGS, `wolfvr_eye_${e}.png`), resolve(OUT, `${label}_pc_${e}.png`));
    const l = await evalJs('window.__wolfvr.renderEye(0)');
    if (l) writeFileSync(resolve(OUT, `${label}_headset_left.png`), Buffer.from(l.split(',')[1], 'base64'));
    results.push(r);
    console.log(JSON.stringify(r));
}
writeFileSync(resolve(OUT, 'vreyes.json'), JSON.stringify(results, null, 1));
await evalJs('window.__wolfvr.toggleVR()');
await sleep(1000);
chrome.kill('SIGKILL');
process.exit(0);
