// [VR 3D 2026-11-03] VR 3D Mode against the REAL viewer, logged in as Shadow Testwolf (Claude's test avatar), all
// through the headset page: headset A (allowed beforehand, as e2e_viewer.mjs) types the login on the big screen,
// enters VR with the fake WebXR device, clicks the left stick to switch to VR 3D Mode, and turns its head.
//   node e2e_viewer_world.mjs <lan_url> <device_id> <secret> <out_dir> <cred_file> <fake_xr.js> <ctl.js>
// Positions of the login fields come from the caller's look at a_login.jpg (env USER_UV, PASS_UV as "u,v").
// The password is read from the cred file here and only ever goes over the local socket to the viewer; it is never
// printed or put on a command line. Saves w_*.jpg (pictures received in 3D mode).
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [LAN, DEV, SECRET, OUT, CRED, FAKE, CTL] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }
const port = new URL(LAN).port;
const line = readFileSync(CRED, 'utf8').trim().split(/\s+/);
const NAME = line.slice(0, 2).join(' '), PASS = line.slice(2).join(' ');
const uv = (s) => s.split(',').map(Number);

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--remote-debugging-port=9381', '--window-size=1280,1400',
    `--user-data-dir=${resolve(OUT, 'profile-A')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9381/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => (await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
const grab = async (file) => { const d = await evalJs('window.__wolfvr.grab()'); if (d) writeFileSync(resolve(OUT, file), Buffer.from(d.split(',')[1], 'base64')); return !!d; };
await send('Page.enable'); await send('Runtime.enable'); await send('Network.enable');
await send('Network.setCookie', { name: 'wvr_s_' + port, value: SECRET, url: LAN, secure: true, httpOnly: true, sameSite: 'Strict' });
await send('Page.addScriptToEvaluateOnNewDocument', { source: `try { localStorage.setItem('wolfvr.device', '${DEV}'); } catch (e) {}\n` + readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') });
await send('Page.navigate', { url: LAN });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.config() && window.__wolfvr.config().mode === "screen")'), 20000), 'big screen from the real viewer');
await sleep(2000);

// ---- log in through the big screen: click a field, type, as a resident would with the headset keyboard
const typeText = async (s) => { for (const ch of s) await evalJs(`window.__wolfvr.sendText(${JSON.stringify(ch)})`); };
const [uu, uv_] = uv(process.env.USER_UV), [pu, pv] = uv(process.env.PASS_UV);
await evalJs(`window.__wolfvr.clickAt(${uu}, ${uv_}, 0)`); await sleep(300);
await evalJs("window.__wolfvr.sendKey('a', 2)"); await sleep(150);   // Ctrl+A: replace what is there
await typeText(NAME); await sleep(300);
await evalJs(`window.__wolfvr.clickAt(${pu}, ${pv}, 0)`); await sleep(300);
await evalJs("window.__wolfvr.sendKey('a', 2)"); await sleep(150);
await typeText(PASS); await sleep(300);
await grab('a_typed.jpg');
await evalJs("window.__wolfvr.sendKey('Enter', 0)");
console.log('   logging in...');
await sleep(45000);
await grab('a_after_login.jpg');

// ---- VR, then left stick click = VR 3D Mode
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!window.__fakeXR.session')), 'Enter VR');
await sleep(1000);
await evalJs("window.__fakeCtl.press('left', 3, true)"); await sleep(150);
await evalJs("window.__fakeCtl.press('left', 3, false)");
check(await waitFor(() => evalJs('window.__wolfvr.config() && window.__wolfvr.config().mode === "world"'), 15000), 'switched to VR 3D Mode');
await sleep(4000);
check(await grab('w_yaw0.jpg'), 'a 3D picture, head straight ahead (w_yaw0.jpg)');
await evalJs('window.__fakeXR.setYaw(Math.PI / 2)'); await sleep(3000);
await grab('w_yaw90.jpg');
await evalJs('window.__fakeXR.setYaw(0); window.__fakeXR.setPitch(0.6)'); await sleep(3000);
await grab('w_pitch_up.jpg');
await evalJs('window.__fakeXR.setPitch(0)');
const n = await evalJs('window.__wolfvr.numbers().rows');
console.log('   numbers:', JSON.stringify(n));
// back to the big screen so the next start is ordinary
await evalJs("window.__fakeCtl.press('left', 3, true)"); await sleep(150);
await evalJs("window.__fakeCtl.press('left', 3, false)");
await waitFor(() => evalJs('window.__wolfvr.config().mode === "screen"'), 10000);
await evalJs('window.__wolfvr.toggleVR()');
ws.close(); chrome.kill();
console.log(failures ? `E2E VIEWER 3D FAILED (${failures})` : 'E2E VIEWER 3D PASSED');
process.exit(failures ? 1 : 0);
