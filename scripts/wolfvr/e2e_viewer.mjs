// [VR BIG SCREEN 2026-11-03] Big-screen mode against the REAL viewer (started separately, isolated profile, at its
// login screen, with --set WolfVRStreamEnabled 1 --set WolfVRStreamPort <port>). Headset A connects straight in and
// gets the viewer window; headset B is told someone is already connected. [VR SIMPLE] No pairing: <device_id> is only
// the page's own id (a reload takes its place back), <secret> is no longer used.
//   node e2e_viewer.mjs <lan_url> <device_id> <secret> <out_dir>
// Saves a_login.jpg (the picture A received).
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [LAN, DEV, SECRET, OUT] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }
const port = new URL(LAN).port;

async function browser(name, cdp) {
    const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
        '--ignore-certificate-errors', `--remote-debugging-port=${cdp}`, '--window-size=1280,1400',
        `--user-data-dir=${resolve(OUT, 'profile-' + name)}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
    chrome.stderr.on('data', () => {});
    let t = null;
    for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch(`http://127.0.0.1:${cdp}/json`)).json(); } catch (e) { await sleep(250); } }
    const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
    await new Promise(r => ws.onopen = r);
    let id = 0; const pend = new Map();
    ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
    const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
    const evalJs = async (e) => (await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
    await send('Page.enable'); await send('Runtime.enable'); await send('Network.enable');
    return { send, evalJs, close: () => { ws.close(); chrome.kill(); } };
}
const grab = async (b, file) => {
    const d = await b.evalJs('window.__wolfvr.grab()');
    if (d) writeFileSync(resolve(OUT, file), Buffer.from(d.split(',')[1], 'base64'));
    return !!d;
};

const A = await browser('A', 9371);
await A.send('Network.setCookie', { name: 'wvr_s_' + port, value: SECRET, url: LAN, secure: true, httpOnly: true, sameSite: 'Strict' });
await A.send('Page.addScriptToEvaluateOnNewDocument', { source: `try { localStorage.setItem('wolfvr.device', '${DEV}'); } catch (e) {}` });
await A.send('Page.navigate', { url: LAN });
check(await waitFor(() => A.evalJs('!!(window.__wolfvr && window.__wolfvr.config())'), 20000), 'allowed headset A gets the stream from the real viewer');
const cfg = await A.evalJs('window.__wolfvr.config()');
console.log('   config:', JSON.stringify(cfg));
check(cfg && cfg.mode === 'screen', 'big-screen mode');
await sleep(2500);
check(await grab(A, 'a_login.jpg'), 'A received a picture of the viewer window (a_login.jpg)');

const B = await browser('B', 9372);
await B.send('Page.navigate', { url: LAN });
check(await waitFor(() => B.evalJs("(document.getElementById('wvr-error').textContent || '').includes('Someone is already connected')")),
      'a second headset is told someone is already connected (no question at the PC)');
A.close(); B.close();
console.log(failures ? `E2E VIEWER FAILED (${failures})` : 'E2E VIEWER PASSED');
process.exit(failures ? 1 : 0);
