// [VR PAIRING 2026-11-03] End-to-end test of "no code, Allow / Deny" with no headset: the real test server
// (wolfvrpipe_selftest serve, answering on its stdin like Paul in the terminal) and headless Chromium with
// WolfStorm's fake WebXR device (firestorm repo wolfstorm/tools/fake_xr.js).
//   node scripts/wolfvr/e2e_pairing.mjs <selftest_bin> <web_root> <fake_xr.js> <out_dir> [port=4352]
// Steps: typed plain-http address is redirected to https; a new headset waits and gets no picture; Allow; picture +
// Enter VR; reload goes straight in (cookie); a second headset is Denied and stays out; a server restart still knows
// the first. Prints PASS/FAIL per step; exit code 0 only when all pass. Ports 4350+ only (4333-4340 are Paul's).
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

// ---- the server, answered on stdin
function startServer() {
    const p = spawn(BIN, ['serve', WEBROOT, '600', 'auto'], {
        env: { ...process.env, WOLFVR_PORT: PORT, WOLFVR_STATS: '1', WOLFVR_STATE_DIR: STATE },
        stdio: ['pipe', 'pipe', 'pipe'] });
    const srv = { p, lines: [], asks: 0, lan: '' };
    let buf = '';
    p.stdout.on('data', (d) => {
        buf += d.toString();
        let i;
        while ((i = buf.indexOf('\n')) >= 0) {
            const line = buf.slice(0, i); buf = buf.slice(i + 1);
            srv.lines.push(line);
            if (line.startsWith('ASK ')) srv.asks++;
            if (line.startsWith('LAN_URL ')) srv.lan = line.slice(8).trim();
            if (line.startsWith('LOG ')) console.log('   server:', line.slice(4));
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

// ---- a browser (one profile = one headset)
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
        close: async () => { ws.close(); chrome.kill(); await sleep(300); },
    };
}
const status = (b) => b.evalJs("document.getElementById('wvr-status').textContent");
const waitingShown = (b) => b.evalJs("!document.getElementById('wvr-waiting').hidden");
const hasConfig = (b) => b.evalJs('!!(window.__wolfvr && window.__wolfvr.config())');
const stats = (srv) => { const l = srv.lines.filter(x => x.startsWith('STATS ')).pop(); return l ? JSON.parse(l.slice(6)) : null; };

// ---- 1. first headset: typed address over plain http
let srv = startServer();
await waitFor(() => srv.lan);
const lanUrl = srv.lan;                                    // https://<ip>:<port>/
const typed = 'http://' + lanUrl.slice('https://'.length);   // what the browser opens for "192.168.1.9:4352"
console.log('server at', lanUrl);
const A = await browser('A');
const HELP = process.env.VR_HELP_URL;   // a local copy of wolf-grid.com/vr (gridmanager vr.php under php -S)
if (HELP) {
    // The resident opens wolf-grid.com/vr, types the short address and taps Open.
    await A.nav(HELP);
    const short = lanUrl.slice('https://'.length, -1);
    await A.evalJs(`(() => { const i = document.getElementById('addr'); i.value = 'nonsense'; document.querySelector('#addr-form button').click(); })()`);
    check(await A.evalJs("!document.getElementById('addr-error').hidden && document.getElementById('addr').getAttribute('aria-invalid') === 'true'"), '/vr: a wrong address gets a visible error next to the box');
    await A.evalJs(`(() => { const i = document.getElementById('addr'); i.value = '8.8.8.8:4333'; document.querySelector('#addr-form button').click(); })()`);
    check(await A.evalJs("!document.getElementById('addr-error').hidden") && (await A.evalJs('location.href')).startsWith(HELP), '/vr: a public address is refused');
    await A.evalJs(`(() => { const i = document.getElementById('addr'); i.value = '${short}'; document.querySelector('#addr-form button').click(); })()`);
    await sleep(1500);
    check((await A.evalJs('location.href')) === lanUrl, `/vr: typing ${short} and Open goes to ${lanUrl}`);
} else {
    await A.nav(typed);
    check((await A.evalJs('location.href')) === lanUrl, `plain http ${typed} is sent to ${lanUrl}`);
}
check(await waitFor(() => srv.asks === 1), 'the PC is asked about the new headset');
check(await waitFor(async () => (await waitingShown(A)) && (await A.evalJs("document.getElementById('wvr-waiting').textContent")).includes('Waiting for you to press Allow on your PC')), 'the headset page says "Waiting for you to press Allow on your PC"');
console.log('   page status:', await status(A));
await A.shot('1_waiting.jpg');
await sleep(1500);
check(!(await hasConfig(A)) && (stats(srv)?.sent ?? 0) === 0 && !(stats(srv)?.conn), 'no picture and no connection while waiting');
srv.p.stdin.write('a\n');
check(await waitFor(() => hasConfig(A), 10000), 'after Allow the picture starts');
console.log('   page status:', await status(A));
await A.evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => A.evalJs('!!(window.__fakeXR && window.__fakeXR.session)')), 'Enter VR works');
for (let i = 0; i < 30; i++) { await A.evalJs(`window.__fakeXR.setYaw(${Math.sin(i / 5)})`); await sleep(100); }
const s1 = stats(srv);
console.log('   numbers:', JSON.stringify(s1));
check(s1 && s1.conn === 1 && s1.sent > 50 && s1.poseHz > 10, 'frames go out and poses come in');
await A.shot('2_vr.jpg');
await A.evalJs('window.__wolfvr.toggleVR()');

// ---- 2. reload: remembered, no question
await A.reload();
check(await waitFor(() => hasConfig(A), 10000) && srv.asks === 1, 'reload goes straight in, nobody is asked again');

// ---- 3. a second headset is denied
const B = await browser('B');
await B.nav(typed);
check((await B.evalJs('location.href')) === lanUrl, `plain http ${typed} is sent to ${lanUrl}`);
check(await waitFor(() => srv.asks === 2), 'a second headset is asked about');
srv.p.stdin.write('d\n');
check(await waitFor(async () => (await status(B)) === 'Not allowed.'), 'Deny: the page says the PC said no');
console.log('   page error:', await B.evalJs("document.getElementById('wvr-error').textContent"));
check(await hasConfig(A), 'the first headset kept its picture');
await B.reload();
await sleep(2000);
check(srv.asks === 2 && (await status(B)) === 'Not allowed.', 'the denied headset stays out without asking again');
check(!(await hasConfig(B)), 'the denied headset never got a picture');
await B.close();

// ---- 4. restart: the allowed headset is remembered on disk (and /vr remembers the address in A's browser)
if (HELP) {
    await A.nav(HELP);
    const btn = await A.evalJs("(document.querySelector('#saved button.pc') || {}).textContent || ''");
    check(btn.startsWith('Open ' + lanUrl.slice('https://'.length, -1)), '/vr shows the remembered PC as a big button: ' + btn);
    await A.shot('3_vr_help_saved.jpg');
    await A.evalJs("document.querySelector('#saved button.pc').click()");
    await sleep(1500);
}
await stopServer(srv);
srv = startServer();
await waitFor(() => srv.lan);
check(srv.lan === lanUrl, 'same address after a restart (default port kept)');
await A.reload();
check(await waitFor(() => hasConfig(A), 15000) && srv.asks === 0, 'after a restart the first headset goes straight in');
const devices = JSON.parse(readFileSync(resolve(STATE, 'devices.json'), 'utf8'));
check(devices.devices.length === 1 && /^[0-9a-f]{64}$/.test(devices.devices[0].hash), 'devices.json holds one headset, hash only');
await A.close();
await stopServer(srv);
console.log(failures ? `E2E FAILED (${failures})` : 'E2E PASSED');
process.exit(failures ? 1 : 0);
