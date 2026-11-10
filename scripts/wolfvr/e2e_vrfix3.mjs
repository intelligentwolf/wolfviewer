// [VR FIXES 2026-11-09] Paul's Quest 3 test of the Vulkan build: (1) "When I turn my head to the left and right there's a
// tiny bit of lag, where there is black", (2) "I used the camera move then flew then landed then tried to see the floaters
// - and I couldn't see them in VR because they were on the ground with my avatar but my camera was still in the air",
// (3) "After coming out of VR the floaters are looking very transparent" - plus his VR Draw Distance (960) that came back
// as the default. Against the REAL viewer (Shadow Testwolf, session_vr.sh, LEAP relay FIFO ~/.cache/skyband/leap.fifo) in
// headless Chromium with WolfStorm's fake WebXR device, Quest 3 eye windows and Touch Plus controllers:
//   A. turn probe: with the picture held, the left eye drawn as the headset would draw it with the head turned 15 / 30 /
//      -30 degrees and pitched 20: the share of the view no picture covers (what was black), edge fill on and off, for
//      the world-locked eyes (WolfVRDepth on) and the flat cards (WolfVRDepth off: Paul's setting)
//   B. stages: the page's own stage breakdown over 30 s (readback, total) - compare runs of two builds
//   C. camera back: the Camera Controls' pan stick lifts the view, then the avatar flies up and comes down (the headset's
//      right stick), and the view is back at the avatar (the viewer's "VR camera controls (...)" log line)
//   D. a window opened again after the head turned away opens in front, not where it was
//   E. VR Draw Distance window opened, its number box clicked, closed: the setting keeps its value
//   F. Big Screen in Vulkan mode is the window's own composite (wolfvrscreen.cpp composeVkBigScreen): with a HUD worn
//      (E2E_HUD=1, worn through LEAP from Testwolf's inventory and taken off again) a window over the world is grabbed;
//      saved for the eye and measured (how much of the world shows through it, against a grab with the window shut)
//   node e2e_vrfix3.mjs <url> <out_dir> <fake_xr.js> <fake_xr_controllers.js>
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync, appendFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { homedir } from 'node:os';

const [LAN, OUT, FAKE, CTL] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
const H = resolve(homedir(), '.cache/skyband');
const LOG = resolve(H, 'prof/logs/WolfViewer.log');
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failures++; };
const note = (...a) => console.log('   ', ...a);
async function waitFor(fn, ms = 15000, step = 250) { const t0 = Date.now(); while (Date.now() - t0 < ms) { const v = await fn(); if (v) return v; await sleep(step); } return null; }
const leap = (pump, notation) => appendFileSync(resolve(H, 'leap.fifo'), `${pump}\t${notation}\n`);
const leapLog = () => readFileSync(resolve(H, 'leap.log'), 'utf8').split('\n');
// the newest reply after `since` leap.log lines that `pick` accepts (JSON of the IN line)
async function leapReply(since, pick, ms = 8000) {
    return waitFor(() => {
        for (const l of leapLog().slice(since).reverse()) {
            if (!l.startsWith('IN ')) continue;
            try { const j = JSON.parse(l.replace(/^IN \S+ /, '')); const d = j.data || j; if (pick(d)) return d; } catch (e) { /* not JSON */ }
        }
        return null;
    }, ms);
}
async function leapInfo(path) {
    const since = leapLog().length;
    leap('LLWindow', `{'op':'getInfo','path':'${path}','reply':'@@REPLY@@'}`);
    return leapReply(since, (d) => d.rect && d.path && d.path.endsWith(path.replace('//', '')), 4000);
}
async function getSetting(key) {
    const since = leapLog().length;
    leap('LLViewerControl', `{'op':'get','group':'Global','key':'${key}','reply':'@@REPLY@@'}`);
    const d = await leapReply(since, (x) => 'value' in x, 4000);
    return d ? d.value : undefined;
}
const setSetting = (key, v) => leap('LLViewerControl', `{'op':'set','group':'Global','key':'${key}','value':${v}}`);
const logLines = () => readFileSync(LOG, 'utf8').split('\n');
function viewLine(since, why) {
    const l = logLines().slice(since).filter((x) => x.includes('VR camera controls (') && (!why || x.includes(`(${why})`))).pop();
    if (!l) return null;
    const m = l.match(/VR camera controls \((.*?)\): view moved forward (-?[\d.]+) m, left (-?[\d.]+) m, up (-?[\d.]+) m, zoom x(-?[\d.]+), looking (-?\d+) degrees left of the (.*?); head (-?[\d.]+) m above the avatar's eyes, (-?[\d.]+) m from them/);
    return m ? { why: m[1], fwd: +m[2], left: +m[3], up: +m[4], zoom: +m[5], headUp: +m[8], headDist: +m[9], line: l.replace(/^.*VR camera/, 'VR camera') } : null;
}

const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', '--use-angle=vulkan', '--enable-unsafe-swiftshader',
    '--ignore-certificate-errors', '--remote-debugging-port=9387', '--window-size=1280,1280',
    `--user-data-dir=${resolve(OUT, 'profile')}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
chrome.stderr.on('data', () => {});
process.on('exit', () => { try { chrome.kill(); } catch (e) { /* gone */ } });
process.on('uncaughtException', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
process.on('unhandledRejection', (e) => { console.log('ERROR', e && e.stack || e); process.exit(2); });
let t = null;
for (let i = 0; i < 40 && !t; i++) { try { t = await (await fetch('http://127.0.0.1:9387/json')).json(); } catch (e) { await sleep(250); } }
const ws = new WebSocket(t.find(x => x.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); } };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async (e) => { const r = await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true }); return r.result?.result?.value; };
const savePng = (file, dataUrl) => { if (dataUrl) writeFileSync(resolve(OUT, file), Buffer.from(dataUrl.split(',')[1], 'base64')); };
const grab = async (file) => { const d = await evalJs('window.__wolfvr.grab()'); savePng(file, d); return d; };
const RIGHT = [0.2, 1.4, -0.2];
async function aimAt(p) {
    const dx = p[0] - RIGHT[0], dy = p[1] - RIGHT[1], dz = p[2] - RIGHT[2];
    const n = Math.hypot(dx, dy, dz);
    await evalJs(`window.__fakeCtl.aim('right', ${Math.atan2(-dx / n, -dz / n)}, ${Math.asin(dy / n)})`);
}
const press = async (hand, i, down) => evalJs(`window.__fakeCtl.press('${hand}', ${i}, ${down})`);
async function click(hand, i, holdMs = 250) { await press(hand, i, true); await sleep(holdMs); await press(hand, i, false); await sleep(600); }
async function aimAtView(path, fu = 0.5, fv = 0.5) {
    const info = await leapInfo(path), root = await leapInfo('//main_view');
    if (!info || !info.visible_chain || !info.rect || !root) return null;
    const x = info.rect.left + fu * (info.rect.right - info.rect.left), y = info.rect.top - fv * (info.rect.top - info.rect.bottom);
    await aimAt(await evalJs(`window.__wolfvr.windowPoint(${x / root.rect.right}, ${1 - y / root.rect.top})`)); await sleep(600);
    return await evalJs('window.__wolfvr.aim()');
}
// the relay logs an LLSD uuid as 32 hex digits (relay.py parse 'u'); the viewer's notation parser wants it dashed
const dashed = (h) => /^[0-9a-f]{32}$/.test(h) ? `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}` : h;
const mode = () => evalJs('window.__wolfvr.config() && window.__wolfvr.config().mode');

// the Quest 3's own eye windows (VR_HANDOVER.md section 6, Paul's device line), so the pictures are his shape
await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: readFileSync(FAKE, 'utf8') + '\n' + readFileSync(CTL, 'utf8') +
    '\nwindow.__asymEyes = [[-1.6003, 0.9657, -1.6643, 1.1106], [-0.9657, 1.6003, -1.6643, 1.1106]];' });
await send('Page.navigate', { url: LAN });
check(await waitFor(() => evalJs('!!(window.__wolfvr && window.__wolfvr.xr && window.__wolfvr.xr().said === false)'), 30000), 'connected to the real viewer');
await evalJs("try { localStorage.setItem('wolfvr.headset', 'auto'); } catch (e) {}");
await evalJs('window.__wolfvr.toggleVR()');
check(await waitFor(() => evalJs('!!window.__fakeXR.session')), 'Enter VR');
// E2E_ONLY_F=1: only F, started in Big Screen (WolfVRStreamMode screen) - no 3D Mode at all
const ONLY_F = process.env.E2E_ONLY_F === '1', ONLY_C = process.env.E2E_ONLY_C === '1';   // E2E_ONLY_C=1: only C and D
check(await waitFor(() => evalJs(`!!(window.__wolfvr.config() && window.__wolfvr.config().mode === "${ONLY_F ? 'screen' : 'world'}")`), 30000), ONLY_F ? 'in Big Screen' : 'in VR 3D Mode');
for (let k = 0; k < 2; k++) { leap('LLWindow', "{'op':'keyDown','keycode':i129}"); leap('LLWindow', "{'op':'keyUp','keycode':i129}"); await sleep(600); }
const hasTurnProbe = await evalJs('typeof window.__wolfvr.turnProbe === "function"');
console.log(`page has the turn probe: ${hasTurnProbe}`);
if (process.env.E2E_FILL_OFF === '1' && hasTurnProbe) { await evalJs('window.__wolfvr.edgeFill(false)'); console.log('   edge fill switched off for this run'); }

// ---------------------------------------------------------------- A. turn probe
async function turnRound(tag) {
    await evalJs('window.__wolfvr.freeze(true)');
    await sleep(600);
    const res = {}, still = {};
    for (const fill of [true, false]) {
        await evalJs(`window.__wolfvr.edgeFill(${fill})`);
        await sleep(300);
        for (const [name, yaw, pitch] of [['still', 0, 0], ['left5', 5, 0], ['left8', 8, 0], ['right8', -8, 0], ['left15', 15, 0],
                                          ['left30', 30, 0], ['right30', -30, 0], ['up20', 0, 20]]) {
            const r = await evalJs(`window.__wolfvr.turnProbe(${yaw}, ${pitch})`);
            if (!r) continue;
            res[`${fill ? 'fill' : 'nofill'}_${name}`] = r.empty;
            if (name === 'left30' || name === 'left15' || name === 'left8' || name === 'still') savePng(`turn_${tag}_${fill ? 'fill' : 'nofill'}_${name}.png`, r.png);
            if (name === 'still') still[fill ? 'fill' : 'nofill'] = r.png;
        }
    }
    await evalJs('window.__wolfvr.edgeFill(true)');
    await evalJs('window.__wolfvr.freeze(false)');
    // head still: every pixel the picture covered without the fill is the same with it (the look is untouched)
    res.stillChanged = await evalJs(`(async () => { const L = async (s) => { const i = new Image(); i.src = s; await i.decode();
        const c = document.createElement('canvas'); c.width = i.width; c.height = i.height; const x = c.getContext('2d'); x.drawImage(i, 0, 0);
        return x.getImageData(0, 0, i.width, i.height).data; };
        const a = await L(${JSON.stringify(still.nofill)}), b = await L(${JSON.stringify(still.fill)}); let n = 0, m = 0;
        for (let k = 0; k < a.length; k += 4) { if (a[k] > 245 && a[k + 1] < 10 && a[k + 2] > 245) continue; m++;
            if (Math.abs(a[k] - b[k]) + Math.abs(a[k + 1] - b[k + 1]) + Math.abs(a[k + 2] - b[k + 2]) > 3) n++; }
        return m ? +(n / m).toFixed(5) : -1; })()`);
    console.log(`   turn ${tag}: ${JSON.stringify(res)}`);
    return res;
}
const res0 = {};
if (hasTurnProbe && !ONLY_F && !ONLY_C && process.env.E2E_SKIP_A !== '1') {
    await sleep(15000);   // pictures with depth arriving
    const d = await evalJs('window.__wolfvr.depth()');
    note('depth', JSON.stringify(d && { active: d.active, frames: d.frames, grid: d.grid }));
    // [VR OVERSCAN 2026-11-09] the middle's sharpness: pixels per degree across the left eye's middle, as drawn now (the
    // viewer's widened window over the picture's eye size) against the 4-degree window at the step's own size (r51 and
    // before), from the headset's own window
    const pic = await evalJs('window.__wolfvr.picture()');
    if (pic && pic.tan && pic.own && pic.eye) {
        const d2r = Math.PI / 180, w4 = (t, s) => Math.tan(Math.atan(t) + s * 4 * d2r);
        const now = pic.eye / (pic.tan[1] - pic.tan[0]) * d2r;
        const marginDeg = (Math.atan(pic.tan[1]) - Math.atan(pic.own[1])) / d2r;
        res0.sharp = { eyePx: pic.eye, marginDeg: +marginDeg.toFixed(2), pxPerDegNow: +now.toFixed(2), sides: pic.sides,
                       pxPerDegBefore: process.env.E2E_BASE_EYE ? +(+process.env.E2E_BASE_EYE / (w4(pic.own[1], 1) - w4(pic.own[0], -1)) * d2r).toFixed(2) : null };
        console.log('   middle sharpness:', JSON.stringify(res0.sharp));
        if (res0.sharp.pxPerDegBefore) check(now >= res0.sharp.pxPerDegBefore * 0.99, `the middle is at least as sharp as before: ${res0.sharp.pxPerDegNow} px/deg (before ${res0.sharp.pxPerDegBefore}), windows widened ${res0.sharp.marginDeg} degrees`);
    }
    const a = await turnRound('depth');
    const pct = (x) => (100 * x).toFixed(1) + '%';
    console.log(`   world-locked eyes, real picture only (no fade frame): uncovered still ${pct(a.nofill_still)}, 5 ${pct(a.nofill_left5)}, 8 ${pct(a.nofill_left8)} / -8 ${pct(a.nofill_right8)}, 15 ${pct(a.nofill_left15)}, 30 ${pct(a.nofill_left30)}, up 20 ${pct(a.nofill_up20)}`);
    check(a.fill_left30 < 0.005 && a.fill_right30 < 0.005 && a.fill_left15 < 0.005 && a.fill_left8 < 0.005 && a.fill_up20 < 0.005,
          `world-locked eyes, with the fade frame: nothing black at 8/15/30/-30 degrees and 20 up (${[a.fill_left8, a.fill_left15, a.fill_left30, a.fill_right30, a.fill_up20].map(pct).join(' / ')})`);
    check(a.stillChanged >= 0 && a.stillChanged < 0.001, `head still: what the picture covers is unchanged by the frame (${a.stillChanged} of those pixels differ)`);
    setSetting('WolfVRDepth', 'false');   // Paul's own setting (his settings.xml: WolfVRDepth 0)
    await sleep(8000);
    const c = await turnRound('cards');
    console.log(`   flat cards, real picture only: uncovered still ${pct(c.nofill_still)}, 5 ${pct(c.nofill_left5)}, 8 ${pct(c.nofill_left8)} / -8 ${pct(c.nofill_right8)}, 15 ${pct(c.nofill_left15)}, 30 ${pct(c.nofill_left30)}, up 20 ${pct(c.nofill_up20)}`);
    check(c.fill_left30 < 0.005 && c.fill_right30 < 0.005 && c.fill_left15 < 0.005 && c.fill_left8 < 0.005 && c.fill_up20 < 0.005,
          `flat cards (Paul's WolfVRDepth off), with the fade frame: nothing black (${[c.fill_left8, c.fill_left15, c.fill_left30, c.fill_right30, c.fill_up20].map(pct).join(' / ')})`);
    check(c.stillChanged >= 0 && c.stillChanged < 0.001, `flat cards, head still: unchanged by the frame (${c.stillChanged} differ)`);
    writeFileSync(resolve(OUT, 'turns.json'), JSON.stringify({ sharp: res0.sharp, depth: a, cards: c }, null, 1));
    setSetting('WolfVRDepth', 'true');
    await sleep(3000);
}

// ---------------------------------------------------------------- B. stages
if (!ONLY_F && !ONLY_C) {
await evalJs('window.__wolfvr.resetStages()');
await sleep(30000);
const st = await evalJs('window.__wolfvr.stages()');
const rt = await evalJs('window.__wolfvr.rates()');
console.log('   stages (p50 ms):', JSON.stringify(Object.fromEntries(Object.entries(st || {}).map(([k, v]) => [k, v.p50 == null ? null : +(+v.p50).toFixed(1)]))));
console.log('   rates:', JSON.stringify(rt && Object.fromEntries(Object.entries(rt).map(([k, v]) => [k, +(+v).toFixed(1)]))));
writeFileSync(resolve(OUT, 'stages.json'), JSON.stringify({ stages: st, rates: rt }, null, 1));
}

// ---------------------------------------------------------------- C. camera back to the avatar after flying
async function cameraWindow() {
    leap('LLFloaterReg', "{'op':'showInstance','name':'camera'}");
    await sleep(2500);
    const tab = await leapInfo('//joysticks_tab');
    if (tab && tab.visible_chain) { await aimAtView('//joysticks_tab'); await click('right', 0, 200); await sleep(800); }
    return !!(await waitFor(async () => { const i = await leapInfo('//cam_track_stick'); return i && i.visible_chain; }, 8000));
}
if (process.env.E2E_SKIP_CAM !== '1' && !ONLY_F) {
    check(await cameraWindow(), 'camera: the Camera Controls window is open in the headset');
    await sleep(1200);
    await aimAtView('//cam_track_stick', 0.5, 0.12);
    let since = logLines().length;
    await press('right', 0, true); await sleep(3500); await press('right', 0, false);
    const up = await waitFor(() => viewLine(since, 'move ended'), 6000);
    note('pan up ->', up && up.line);
    check(!!up && up.up > 1.5, `camera: pan stick held at its top - the headset view rose (up ${up && up.up} m)`);
    await evalJs("window.__fakeCtl.aim('right', 0, -0.6)");   // off the window: the stick flies, not scrolls
    await sleep(800);
    // fly up (3D Mode: right stick forward = fly up, wolfvr.js pollRightStick), then down until landed
    // the log's last line may still be half written when counted: start one line back (no reset line can be there yet:
    // the pan's "move ended" line was read in full above)
    since = logLines().length - 1;
    note('fly up from', new Date().toISOString());
    await evalJs("window.__fakeCtl.axis('right', 3, -1)"); await sleep(2500); await evalJs("window.__fakeCtl.axis('right', 3, 0)");
    const back = await waitFor(() => viewLine(since, 'the avatar moved: back to the avatar'), 8000);
    note('flying ->', back && back.line);
    check(!!back && back.up === 0 && back.fwd === 0 && back.left === 0, 'camera: flying brought the headset view back to the avatar (pan undone), as the desktop camera does');
    await evalJs("window.__fakeCtl.axis('right', 3, 1)"); await sleep(9000); await evalJs("window.__fakeCtl.axis('right', 3, 0)");
    await sleep(2500);
    // a preset press logs where the head is now without moving it (cameraPreset -> logView): at the avatar, 0.45 m above its eyes
    since = logLines().length;
    await aimAtView('//rear_view'); await click('right', 0, 200);
    const now = await waitFor(() => viewLine(since, 'preset'), 6000);
    note('after landing ->', now && now.line);
    check(!!now && Math.abs(now.headUp - 0.45) < 0.5 && now.headDist < 2.5, `camera: after landing the head is with the avatar (${now && now.headUp} m above its eyes, ${now && now.headDist} m from them)`);

    // ---------------------------------------------------------------- D. a window reopened after the head turned away
    await sleep(1500);
    const p0 = (await evalJs('window.__wolfvr.panels()') || []).find((p) => p.k.startsWith('camera'));
    note('camera panel at', JSON.stringify(p0 && { yaw: p0.yaw }));
    leap('LLFloaterReg', "{'op':'hideInstance','name':'camera'}");
    await sleep(2500);
    await evalJs(`window.__fakeXR.setYaw(${(100 * Math.PI / 180).toFixed(4)})`);   // the head turned 100 degrees left
    await sleep(1500);
    leap('LLFloaterReg', "{'op':'showInstance','name':'camera'}");
    const p1 = await waitFor(async () => (await evalJs('window.__wolfvr.panels()') || []).find((p) => p.k.startsWith('camera')), 8000);
    note('reopened at', JSON.stringify(p1 && { yaw: p1.yaw }));
    check(!!p0 && !!p1 && Math.abs(p1.yaw - 100) <= 40, `a window opened again after the head turned 100 degrees away opens in front (yaw ${p1 && p1.yaw}, was ${p0 && p0.yaw})`);
    leap('LLFloaterReg', "{'op':'hideInstance','name':'camera'}");
    await evalJs('window.__fakeXR.setYaw(0)');
    await sleep(2000);
}

// ---------------------------------------------------------------- E. VR Draw Distance keeps its value
if (!ONLY_F && !ONLY_C) {
    const before = await getSetting('WolfVRDrawDistance');
    leap('LLFloaterReg', "{'op':'showInstance','name':'wolf_vr_draw_distance'}");
    await sleep(3000);
    // the number box at the slider's right end (an editable text: the pointer gives it the keyboard)
    const a = await aimAtView('//vr_draw_distance', 0.95, 0.5);
    note('aim on the number box', JSON.stringify(a && { k: a.k, c: a.c }));
    await click('right', 0, 200);
    await sleep(1500);
    leap('LLFloaterReg', "{'op':'hideInstance','name':'wolf_vr_draw_distance'}");
    await sleep(2000);
    const after = await getSetting('WolfVRDrawDistance');
    check(before === 960 && after === 960, `VR Draw Distance window opened, its number box clicked, closed: ${before} -> ${after} (set 960 at start)`);
}

if (ONLY_C) { console.log(`RESULT failures ${failures}`); process.exit(failures ? 1 : 0); }
// ---------------------------------------------------------------- F. a window over the world, Big Screen (Vulkan: the window's own composite)
{
    let hud = null;
    if (process.env.E2E_HUD === '1') {
        let since = leapLog().length;
        leap('LLInventory', "{'op':'getBasicFolderID','ft_name':'root_inv','reply':'@@REPLY@@'}");
        const root = await leapReply(since, (d) => d.id && !d.items, 5000);
        if (root) {
            since = leapLog().length;
            leap('LLInventory', `{'op':'collectDescendantsIf','folder_id':'${dashed(root.id)}','name':'HUD','type':'object','filter_links':'EXCLUDE_LINKS','limit':i2,'reply':'@@REPLY@@'}`);
            const r = await leapReply(since, (d) => d.items, 10000);
            const items = r ? Object.entries(r.items).map(([k, v]) => ({ ...v, id: k })) : [];
            note('HUD objects in the inventory:', items.slice(0, 8).map((i) => i.name).join(' | '));
            hud = items.find((i) => /hud/i.test(i.name)) || null;
            if (hud) {
                const hs = logLines().length;
                leap('LLAppearance', `{'op':'wearItems','items_id':['${hud.id}'],'replace':false}`);
                const worn = await waitFor(() => logLines().slice(hs).some((l) => /HUD objects count: [1-9]/.test(l)), 30000, 1000);
                note(`wore "${hud.name}":`, worn ? 'a HUD object is now worn' : 'no HUD seen in the log yet');
            }
        }
        check(!!hud, 'a HUD from Testwolf\'s inventory is worn');
    }
    // to Big Screen (the left stick click)
    if (!ONLY_F) { await press('left', 3, true); await sleep(120); await press('left', 3, false); }
    check(!!(await waitFor(async () => (await mode()) === 'screen', 15000)), 'Big Screen');
    await sleep(6000);
    leap('LLFloaterReg', "{'op':'showInstance','name':'about_land'}");
    await sleep(5000);
    const info = await leapInfo('//floater_about_land') || await leapInfo('//about_land');
    const root = await leapInfo('//main_view');
    await grab('big_window_open.jpg');
    leap('LLFloaterReg', "{'op':'hideInstance','name':'about_land'}");
    await sleep(4000);
    await grab('big_window_shut.jpg');
    if (info && root) {
        const r = [info.rect.left / root.rect.right, 1 - info.rect.top / root.rect.top, info.rect.right / root.rect.right, 1 - info.rect.bottom / root.rect.top];
        writeFileSync(resolve(OUT, 'big_window_rect.json'), JSON.stringify(r));
        note('window rectangle (0..1 from the top-left):', r.map((x) => x.toFixed(3)).join(','));
    }
    if (!ONLY_F) { await press('left', 3, true); await sleep(120); await press('left', 3, false); await waitFor(async () => (await mode()) === 'world', 15000); }
    if (hud) { leap('LLAppearance', `{'op':'detachItems','items_id':['${hud.id}']}`); await sleep(4000); note('HUD taken off again'); }
}
console.log(`RESULT failures ${failures}`);
process.exit(failures ? 1 : 0);
