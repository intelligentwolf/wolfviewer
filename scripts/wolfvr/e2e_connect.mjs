// [VR SIMPLE 2026-11-03] End-to-end test of "connect straight in" with no headset: the real test server
// (wolfvrpipe_selftest serve) and headless Chromium with WolfStorm's fake WebXR device (wolfstorm/tools/fake_xr.js).
//   node scripts/wolfvr/e2e_connect.mjs <selftest_bin> <web_root> <fake_xr.js> <out_dir> [port=4352]
// Steps: typed plain-http address is redirected to https; the headset gets the picture with nothing to press but Enter
// VR; a reload takes its own place back; a second headset is told someone is connected and gets no picture, then
// joins by itself when the first leaves; a server restart keeps the same address. Ports 4350+ only.
import { spawn } from 'node:child_process';
import { readFileSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { resolve } from 'node:path';

const [BIN, WEBROOT, FAKE, OUT, PORT = '4352'] = process.argv.slice(2);
if (!BIN || !WEBROOT || !FAKE || !OUT) { console.error('usage: e2e_pairing.mjs <bin> <web_root> <fake_xr.js> <out> [port]'); process.exit(2); }
if (Number(PORT) >= 4333 && Number(PORT) <= 4340) { console.error('ports 4333-4340 are for Paul'); process.exit(2); }
rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
const STATE = resolve(OUT, 'state');
const fake = readFileSync(FAKE, 'utf8');
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };

// ---- the server
function startServer() {
    const p = spawn(BIN, ['serve', WEBROOT, '600', 'auto'], {
        env: { ...process.env, WOLFVR_PORT: PORT, WOLFVR_STATS: '1', WOLFVR_STATE_DIR: STATE },
        stdio: ['ignore', 'pipe', 'pipe'] });
    const srv = { p, lines: [], lan: '' };
    let buf = '';
    p.stdout.on('data', (d) => {
        buf += d.toString();
        let i;
        while ((i = buf.indexOf('\n')) >= 0) {
            const line = buf.slice(0, i); buf = buf.slice(i + 1);
            srv.lines.push(line);
            if (line.startsWith('LAN_URL ')) srv.lan = line.slice(8).trim();
            if (line.startsWith('LOG ') || line.startsWith('HEADSET')) console.log('   server:', line);
        }
    });
    p.stderr.on('data', () => {});
    return srv;
}
async function waitFor(fn, ms = 10000, step = 200) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); }
    return null;
}
async function stopServer(srv) { srv.p.kill('SIGTERM'); await new Promise(r => srv.p.once('exit', r)); }

let cdpPort = 9351;
async function browser(name) {
    const port = cdpPort++;
    const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
        '--ignore-certificate-errors', `--remote-debugging-port=${port}`, '--window-size=1280,1280',
        `--user-data-dir=${resolve(OUT, 'profile-' + name)}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
    chrome.stderr.on('data', () => {});
    let targets = null;
    for (let i = 0; i < 40 && !targets; i++) { try { targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json(); } catch (e) { await sleep(250); } }
    if (!targets) throw new Error('no CDP');
    const ws = new WebSocket(targets.find(t => t.type === 'page').webSocketDebuggerUrl);
    await new Promise(r => ws.onopen = r);
    let id = 0; const pending = new Map();
    ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); } };
    const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
    const evalJs = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true }); return r.result?.result?.value; };
    await send('Page.enable'); await send('Runtime.enable');
    await send('Page.addScriptToEvaluateOnNewDocument', { source: fake });
    return {
        evalJs,
        nav: async (url) => { await send('Page.navigate', { url }); await sleep(1500); },
        reload: async () => { await send('Page.reload', { ignoreCache: true }); await sleep(1500); },
        shot: async (file) => { const r = await send('Page.captureScreenshot', { format: 'jpeg', quality: 70 }); writeFileSync(resolve(OUT, file), Buffer.from(r.result.data, 'base64')); },
        close: async () => { ws.close(); chrome.kill(); await sleep(500); },
    };
}
const status = (b) => b.evalJs("document.getElementById('wvr-status').textContent");
const errorText = (b) => b.evalJs("document.getElementById('wvr-error').textContent");
const hasConfig = (b) => b.evalJs('!!(window.__wolfvr && window.__wolfvr.config())');
const stats = (srv) => { const l = srv.lines.filter(x => x.startsWith('STATS ')).pop(); return l ? JSON.parse(l.slice(6)) : null; };

let srv = startServer();
await waitFor(() => srv.lan);
const lanUrl = srv.lan;
const typed = 'http://' + lanUrl.slice('https://'.length);
console.log('server at', lanUrl);
const A = await browser('A');
const HELP = process.env.VR_HELP_URL;   // a local copy of wolf-grid.com/vr (gridmanager vr.php under php -S)
if (HELP) {
    await A.nav(HELP);
    const short = lanUrl.slice('https://'.length, -1);
    await A.evalJs(`(() => { const i = document.getElementById('addr'); i.value = '${short}'; document.querySelector('#addr-form button').click(); })()`);
    await sleep(1500);
    check((await A.evalJs('location.href')) === lanUrl, `/vr: typing ${short} and Open goes to ${lanUrl}`);
} else {
    await A.nav(typed);
    check((await A.evalJs('location.href')) === lanUrl, `plain http ${typed} is sent to ${lanUrl}`);
}
check(await waitFor(() => hasConfig(A), 10000), 'the headset gets the picture straight away: nothing to allow');
check(srv.lines.some(l => l.startsWith('HEADSET CONNECTED')), 'the PC is told "headset connected" (a notice, nothing to click)');
check(!(await A.evalJs("document.getElementById('vr-btn').disabled")), 'the big Enter VR button is ready');
await A.shot('1_connected.jpg');
await A.evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => A.evalJs('!!(window.__fakeXR && window.__fakeXR.session)')), 'Enter VR works');
for (let i = 0; i < 30; i++) { await A.evalJs(`window.__fakeXR.setYaw(${Math.sin(i / 5)})`); await sleep(100); }
const s1 = stats(srv);
console.log('   numbers:', JSON.stringify(s1));
check(s1 && s1.conn === 1 && s1.sent > 50 && s1.poseHz > 10, 'frames go out and poses come in');
await A.evalJs('window.__wolfvr.toggleVR()');

await A.reload();
check(await waitFor(() => hasConfig(A), 10000), 'a reload takes its own place back');

const B = await browser('B');
await B.nav(lanUrl);
check(await waitFor(async () => (await errorText(B) || '').includes('Someone is already connected')), 'a second headset is told someone is already connected');
check(!(await hasConfig(B)) && await hasConfig(A), 'the second one gets no picture; the first keeps it');
await B.shot('2_busy.jpg');
await A.close();
check(await waitFor(() => hasConfig(B), 20000), 'when the first one leaves, the second joins by itself');
await B.close();

await stopServer(srv);
srv = startServer();
await waitFor(() => srv.lan);
check(srv.lan === lanUrl, 'same address after a restart (default port kept)');
await stopServer(srv);
console.log(failures ? `E2E FAILED (${failures})` : 'E2E PASSED');
process.exit(failures ? 1 : 0);
