/*
 * WolfViewer VR in the headset browser - stage 1: the pipe.
 * Copyright (C) 2026 IntelligentWolf Ltd. (LGPL 2.1, as the viewer)
 *
 * Served by WolfViewer itself on the home network (indra/newview/wolfvrpipe.cpp). Sends the head and controller
 * poses to the viewer at the display rate, shows the pictures the viewer sends back, and REPROJECTS each picture
 * to the newest head pose: the picture is a square window into the world, placed in the direction of the head
 * pose it was rendered for, so turning your head turns the view at once even while the next picture is on its way.
 *
 * SHARED WITH WOLFSTORM (so it behaves the same as WolfStorm VR, which residents already know):
 *   three.js r185 + WolfStorm's multiview patch     three/ (byte copies of wolfstorm/js/vendor/three, PROVENANCE.txt)
 *   VR button: id, icon, label "VR"/"Exit VR",      wolfstorm/index.php #vr-btn, css/toolbar.css, and
 *     title "Enter VR Mode", green pulse in VR        render_manager.js toggleVR() label handling
 *   Entry flow: navigator.xr touched only on click, render_manager.js toggleVR(): isSessionSupported on click,
 *     re-entry guard, makeXRCompatible on click,     _vrEntering guard, makeXRCompatible, sessionInit
 *     the same optionalFeatures, focus guard,        {optionalFeatures: local-floor, bounded-floor, hand-tracking,
 *     try/catch around the XR frame callback          layers}, _vrFocusGuard, _xrAnimateGuarded
 *   Same messages: "VR is not supported in this browser" / "... on this device"
 *   Controls: right stick X = snap turn 45 deg       render_manager.js _updateVRInput (arm past 0.6, re-arm under
 *     (stick right = turn right)                      0.3, step = -sign(x) * deg)
 *   Left controller menu button (any index >= 6)    vr_menu.js _pollMenuButtons (toggles the HUD there; here the
 *     toggles the panel                               numbers panel, the only panel stage 1 has)
 *   Numbers panel look: rows, fonts, colours        vr_menu.js _updateStats; docked to the camera with identity
 *                                                   rotation like vr_hud.js surfaces (rev 3: screen-locked, no
 *                                                   keystone) - never lazy-follow (Paul 09-02: "they just fly around")
 *   Stick/button indices: xr-standard               input_manager.js _vrLocomotionFlags, render_manager.js
 *                                                   (0 trigger, 1 grip, 3 stick press, 4 A/X, 5 B/Y; axes 2/3 stick)
 * Later stages reuse WolfStorm's vr_hud.js action bar / panels / radar / wrist gauge and vr_menu.js pointer for the
 * in-VR menus; the controller state is already sent to the viewer ("c" in each pose) for that.
 */
import * as THREE from './three/three.module.js';
import { VRBar } from './vrbar.js';
import { VRLayer } from './vrlayer.js';
import { VRAudio } from './vraudio.js';
import { DepthEyes, inflateDepth, SHELL } from './vrdepth.js';

// [VR PAIRING 2026-11-03] No code in the address (Paul: "get rid of that code"). This headset's own random id, kept in
// this browser; the PC asks Allow / Deny the first time it sees it, then remembers it (wolfvrpairing.h). The secret
// that proves it is an HttpOnly cookie this script never sees.
function deviceId() {
    const ok = (v) => typeof v === 'string' && /^[0-9a-f]{32}$/.test(v);
    try {
        const v = localStorage.getItem('wolfvr.device');
        if (ok(v)) return v;
    } catch (e) { /* storage blocked: a new id each visit, so the PC asks each time */ }
    const b = new Uint8Array(16);
    crypto.getRandomValues(b);
    const v = Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
    try { localStorage.setItem('wolfvr.device', v); } catch (e) { /* see above */ }
    return v;
}
const DEVICE = deviceId();
const HEADER_MIN = 72;            // wolfvrpipe.cpp kHeaderSize before stereo; the real size is at byte 4 (96 now)
const MAGIC = 0x31525657;         // "WVR1"
const PICTURE_DISTANCE = 10;      // metres: far enough that walking about barely moves the picture

// ------------------------------------------------------------------------------------------------ page feedback
const $ = (id) => document.getElementById(id);
function setStatus(text) { $('wvr-status').textContent = text; }
function showError(text) {
    // Failures stay until the next success: the reader must be able to read and copy them.
    const e = $('wvr-error');
    e.textContent = text;
    e.hidden = !text;
}
function toast(text) {
    const t = document.createElement('div');
    t.className = 'wvr-toast';
    t.textContent = text;
    $('wvr-toasts').appendChild(t);
    setTimeout(() => t.remove(), 10000);   // information only; failures stay in the error box until fixed
}

// ------------------------------------------------------------------------------------------------ numbers
class Rolling {
    constructor(n = 120) { this.n = n; this.v = []; }
    add(x) { if (Number.isFinite(x)) { this.v.push(x); if (this.v.length > this.n) this.v.shift(); } }
    avg() { return this.v.length ? this.v.reduce((a, b) => a + b, 0) / this.v.length : NaN; }
    pct(p) { if (!this.v.length) return NaN; const s = [...this.v].sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.round(p * (s.length - 1)))]; }
}
const num = {
    p2p: new Rolling(), v2p: new Rolling(), decode: new Rolling(),
    xrFrames: 0, newFrames: 0, lastT: performance.now(),
    fps: NaN, newFps: NaN, viewer: {}, rtt: NaN, got: 0, gotFps: NaN, decoded: 0, decodedFps: NaN, gotTotal: 0, summaryTick: 0,
};
// [VR STAGES 2026-11-04] Where the time goes, per picture, from the head pose leaving here to the picture shown here:
// the viewer stamps its stages on the picture (wolfvrpipe.cpp header 160..195, ms after the pose arrived there), this
// page adds arrival, decoded and shown. Kept in memory (the last 600 pictures, about 30 s) and sent to the viewer's
// log in the 30-second summary only - nothing per frame (Paul: no logging or stats in hot paths).
const STAGES = ['up', 'wait', 'eyeL', 'eyeR', 'readback', 'queue', 'encode', 'pack', 'down', 'decode', 'show', 'total'];
const stages = Object.fromEntries(STAGES.map((k) => [k, new Rolling(600)]));
function noteStages(hdr, drawn) {
    const st = hdr.st;
    if (!st || !Number.isFinite(clockOff) || hdr.poseClientMs < 0) return;
    const R = st.R - clockOff;                      // when the pose arrived at the viewer, on this page's clock
    const at = (ms) => (ms >= 0 ? R + ms : NaN);    // a viewer stage on this page's clock
    const t = [hdr.poseClientMs, R, at(st.frame), at(st.eye0 >= 0 ? st.eye0 : st.issue), at(st.issue), at(st.collect),
               at(st.encIn), at(st.encOut), at(st.sent), hdr.recvAt, hdr.decodedAt, drawn];
    for (let i = 0; i < 11; i++) stages[STAGES[i]].add(t[i + 1] - t[i]);
    stages.total.add(drawn - hdr.poseClientMs);
}
// [VR DEVICE REPORT 2026-11-04] What this headset itself has on show, once in each 30-second summary: the window panels
// (name, direction, size, shown), the HUD band, the HUD picture's size, its input devices and what the pointer did.
const ptrLog = { presses: { bar: 0, ptr: 0, tp: 0, select: 0, none: 0 }, aim: '-', viewer: '-' };
// [VR DEVICE REPORT] is there anything IN the HUD picture? Two samples, once a summary: the decoded picture itself (a
// 2D canvas, so a decode that gives an empty picture shows) and the texture WebGL holds (read back through a small
// framebuffer, so an upload that gives an empty texture shows). Colour at the middle, the most covered sample of a
// 9 x 9 grid over the coverage half; "-" where it could not be read.
function layerProbe() {
    const img = layer.shown;
    let pic = '-', tex = '-';
    try {
        if (img) {
            const c = document.createElement('canvas'); c.width = 9; c.height = 18;
            const x = c.getContext('2d'); x.drawImage(img, 0, 0, 9, 18);
            const d = x.getImageData(0, 0, 9, 18).data;
            let top = 0, bot = 0;
            for (let i = 0; i < 9 * 9; i++) top = Math.max(top, d[i * 4]);
            for (let i = 9 * 9; i < 9 * 18; i++) bot = Math.max(bot, d[i * 4]);
            pic = `red max upper half ${top} lower half ${bot}`;   // one half is the colour, the other the coverage
        }
    } catch (e) { pic = 'error ' + e.message; }
    try {
        const t = layer.mat.uniforms.map.value, p = renderer.properties.get(t), gl = renderer.getContext();
        if (p && p.__webglTexture && t.image) {
            const w = t.image.displayWidth || t.image.width, h = t.image.displayHeight || t.image.height;
            const fb = gl.createFramebuffer(), prev = gl.getParameter(gl.FRAMEBUFFER_BINDING);
            gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
            gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, p.__webglTexture, 0);
            const ok = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
            let best = 0; const px = new Uint8Array(4);
            if (ok) {
                for (let j = 1; j < 10; j++) for (let i = 1; i < 10; i++) {
                    gl.readPixels(Math.floor(w * i / 10), Math.floor(h * j / 20), 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
                    best = Math.max(best, px[0]);
                }
            }
            gl.bindFramebuffer(gl.FRAMEBUFFER, prev);
            gl.deleteFramebuffer(fb);
            tex = ok ? `${t.isVideoFrameTexture ? 'video' : t.isCanvasTexture ? 'canvas' : 'bitmap'} ${w}x${h} coverage max ${best}` : 'not readable';
        } else tex = p && p.__webglTexture ? 'no image' : 'never uploaded';
    } catch (e) { tex = 'error ' + e.message; }
    // and the layer's own material drawn on a flat card (the same shader, texture and blending as the band) into a
    // 32 x 32 target: what it puts on screen, outside XR - a shader this GPU will not run shows as nothing drawn
    let drawn = '-';
    try {
        const sc = new THREE.Scene(), cam = new THREE.OrthographicCamera(-0.5, 0.5, 0.5, -0.5, 0.1, 10);
        cam.position.z = 1;
        const card = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), layer.mat);
        sc.add(card);
        const rt = new THREE.WebGLRenderTarget(32, 32);
        const xr = renderer.xr.enabled; renderer.xr.enabled = false;
        const prevRT = renderer.getRenderTarget();
        const cc = new THREE.Color(), ca = renderer.getClearAlpha(); renderer.getClearColor(cc);
        renderer.setRenderTarget(rt); renderer.setClearColor(0x000000, 0); renderer.clear(); renderer.render(sc, cam);
        renderer.setRenderTarget(prevRT); renderer.setClearColor(cc, ca);
        renderer.xr.enabled = xr;
        const px = new Uint8Array(32 * 32 * 4);
        renderer.readRenderTargetPixels(rt, 0, 0, 32, 32, px);
        let lit = 0, amax = 0;
        for (let i = 0; i < 32 * 32; i++) { amax = Math.max(amax, px[i * 4 + 3]); if (px[i * 4 + 3] > 8) lit++; }
        rt.dispose(); card.geometry.dispose();
        drawn = `${lit} of 1024 pixels, alpha max ${amax}`;
    } catch (e) { drawn = 'error ' + e.message; }
    return `picture ${pic}; texture ${tex}; drawn ${drawn}`;
}
function deviceLine() {
    const img = layer.shown;
    const iw = img ? (img.displayWidth || img.width || 0) : 0, ih = img ? (img.displayHeight || img.height || 0) : 0;
    const pans = [...layer.panels.values()].map((p) => `${p.k.split(':')[0]}@${(p.yaw * 180 / Math.PI).toFixed(0)}deg/${(p.arc * 180 / Math.PI).toFixed(0)}deg/y${p.c.y.toFixed(2)}${p.mesh.visible ? '' : '(hidden)'}`);
    const sess = renderer.xr.getSession && renderer.xr.getSession();
    const ins = sess ? Array.from(sess.inputSources).map((x) => `${x.handedness}/${x.targetRayMode}/${(x.profiles || [])[0] || '?'}/${x.hand ? 'hand' : x.gamepad ? 'pad' + x.gamepad.buttons.length : 'none'}`) : [];
    const pr = ptrLog.presses;
    return `device: HUD band ${layer.on ? (layer.mesh.visible ? 'shown' : 'on, nothing shown') : (layer.want ? 'not active' : 'switched off')}, ` +
        `[${layerProbe()}] ` +
        `picture ${iw}x${ih} (config ${layer.cfg ? layer.cfg.w + 'x' + layer.cfg.h + ' ' + layer.cfg.codec : 'none'}), ${layer.frames} shown; ` +
        `windows from the viewer ${layer.rects.length}, panels ${pans.length}${pans.length ? ': ' + pans.join(' ') : ''}; ` +
        `inputs ${ins.join(' ') || 'none'}; presses bar ${pr.bar} viewer ${pr.ptr} teleport ${pr.tp} hand ${pr.select} on nothing ${pr.none}; ` +
        `last aim ${ptrLog.aim}, viewer said ${worldAim.k}${worldAim.c ? '/' + worldAim.c : ''}; ` +
        `errors ${devLog.errors}${devLog.shaderErrors ? ' (shaders ' + devLog.shaderErrors + ')' : ''}${devLog.last ? ' last: ' + devLog.last : ''}; page ${pageId || '?'}`;
}
function stagesLine() {
    return STAGES.map((k) => `${k} ${fmt(stages[k].pct(0.5), 0)}`).join(', ') + ` (median ms; total p95 ${fmt(stages.total.pct(0.95), 0)})`;
}

// ------------------------------------------------------------------------------------------------ device report
// [VR DEVICE REPORT 2026-11-04] There is no console in a headset: errors (uncaught, unhandled promise rejections, and
// console.error, where three.js reports a shader it could not compile) go to the viewer's log, at most 10 a minute.
let pageId = '';
const devLog = { window: 0, n: 0, errors: 0, last: '' };
function reportError(kind, msg, stack) {
    devLog.errors++;
    devLog.last = String(msg).slice(0, 160);
    const now = performance.now();
    if (now - devLog.window > 60000) { devLog.window = now; devLog.n = 0; }
    if (++devLog.n > 10) return;
    try { send({ t: 'log', msg: kind + ': ' + String(msg).slice(0, 280), stack: String(stack || '').slice(0, 580) }); } catch (e) { /* not connected */ }
}
window.addEventListener('error', (e) => reportError('error', e.message + ' @' + (e.filename || '').split('/').pop() + ':' + e.lineno, e.error && e.error.stack));
window.addEventListener('unhandledrejection', (e) => reportError('unhandled rejection', e.reason && (e.reason.message || e.reason), e.reason && e.reason.stack));
{
    const ce = console.error.bind(console);
    console.error = (...a) => { ce(...a); reportError('console.error', a.map((x) => (x && x.message) || String(x)).join(' '), ''); };
}

// ------------------------------------------------------------------------------------------------ connection
let ws = null;
let config = null;
let decoder = null, decoderCodec = '';
let waitingKey = true, lastKeyAsk = 0;
const pendingMeta = new Map();     // frameId -> { hdr, t0 }
let latest = null;                 // newest decoded picture not yet drawn: { img, hdr, decodeMs }
let shown = null;                  // picture currently on the texture (closed when replaced)
let clockOff = NaN;                // viewer clock minus page clock (ms), from the best ping
const pings = [];
let pingTimer = 0, retryTimer = 0, poseSeq = 0;

// [VR HUD CLICKS 2026-11-04] Only the messages that a newer one replaces anyway (poses, pointer rays, numbers) are
// dropped while the socket is backed up (the headset microphone shares it); a press, a release, a button or a mode
// switch is always sent, or a click would be lost - or worse, a release, leaving a button held down in the viewer.
const DROPPABLE = new Set(['pose', 'stats', 'ray']);
function send(obj) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const drop = DROPPABLE.has(obj.t) || (obj.t === 'in' && (obj.e === 'ray' || obj.e === 'move' || obj.e === 'stick'));
    if (drop ? ws.bufferedAmount < 65536 : ws.bufferedAmount < 4194304) ws.send(JSON.stringify(obj));
}

function connect() {
    clearTimeout(retryTimer);
    const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws?d=' + DEVICE;
    ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => {
        showError('');
        setStatus('Connected to WolfViewer…');
    };
    ws.onmessage = (ev) => {
        if (typeof ev.data === 'string') onText(ev.data);
        else onPicture(ev.data);
    };
    ws.onclose = (ev) => {
        clearInterval(pingTimer);
        // [VR SIMPLE 2026-11-03] Plain words, and the page keeps trying by itself for as long as it is open: nobody
        // has to be quick, reload, or press anything (Paul: some residents need time).
        if (ev.code === 4000) {
            showError('This headset opened WolfViewer VR again in another tab or window, so this one stopped. ' +
                      'Use the newest one, or reload this page to bring it back here.');
            setStatus('Not connected.');
            return;
        }
        if (ev.code === 4009) {
            showError('Someone is already connected to WolfViewer on this PC. This page joins by itself as soon as ' +
                      'they leave.');
            setStatus('Waiting for the other headset to leave…');
            retryTimer = setTimeout(connect, 5000);
            return;
        }
        if (ev.code === 4003) {
            showError('This headset is not on the same home network as your PC. Connect the headset to the same Wi-Fi ' +
                      'as your PC; this page keeps trying.');
            setStatus('Not connected.');
            retryTimer = setTimeout(connect, 5000);
            return;
        }
        setStatus(ev.code === 1001 ? 'VR is switched off in WolfViewer. This page connects by itself when it is switched on again.'
                                   : 'Looking for WolfViewer on your PC… This page keeps trying by itself.');
        retryTimer = setTimeout(connect, 3000);
    };
}

// [VR 3D FIX 2026-11-04] Can this browser decode H.265 (HEVC Main, level 5.1)? Told to the viewer once (logged there),
// for choosing a sharper codec later; nothing depends on it yet.
const hevcProbe = (typeof VideoDecoder !== 'undefined' && VideoDecoder.isConfigSupported
    ? VideoDecoder.isConfigSupported({ codec: 'hvc1.1.6.L153.B0' }).then((r) => !!r.supported).catch(() => false)
    : Promise.resolve(false));

// Connected (the socket said "welcome"): start the picture and the clock sync.
// [VR IMMERSIVE 2026-11-04] Tell the viewer whether this headset is IN VR right now: in a session, the session shown
// (not under the headset's system menu or asleep: XRSession.visibilityState "visible") and this page not hidden. The
// viewer draws nothing for the headset and never pauses or slows its desktop window unless it is (Paul's PC ran at
// 0 fps when an open tab reconnected by itself at log-in).
let xrSaid = null;
function inVR() {
    return !!(vrSession && renderer.xr.isPresenting && document.visibilityState === 'visible' &&
              (!vrSession.visibilityState || vrSession.visibilityState === 'visible'));
}
function tellXR(force) {
    const on = inVR();
    if (on === xrSaid && !force) return;
    xrSaid = on;
    send({ t: 'xr', on });
}
document.addEventListener('visibilitychange', () => tellXR(false));

function onAllowed() {
    showError('');
    xrSaid = null;
    tellXR(true);   // a new connection: say at once whether this page is in VR (a reconnecting tab usually is not)
    setStatus('Connected to WolfViewer. Waiting for the picture…');
    hevcProbe.then((hevc) => send({ t: 'hello', webcodecs: typeof VideoDecoder !== 'undefined', hevc, ua: navigator.userAgent }));
    quality.sent = -1;   // [VR HEADSETS] tell this (possibly restarted) viewer our quality step with the next config
    pings.length = 0;
    let n = 0;
    clearInterval(pingTimer);
    pingTimer = setInterval(() => { send({ t: 'ping', ct: performance.now() }); if (++n === 10) { clearInterval(pingTimer); pingTimer = setInterval(() => send({ t: 'ping', ct: performance.now() }), 2000); } }, 250);
}

function onText(text) {
    let m;
    try { m = JSON.parse(text); } catch (e) { return; }
    if (m.t === 'busy') return;   // the close (4009) that follows says it
    if (vrAudio.onText(m)) return;   // [VR AUDIO]
    if (m.t === 'welcome') {
        // [VR PAGE ID 2026-11-04] the viewer serves a different page now (it was updated and restarted while this tab
        // stayed open): load the new one, once, instead of running the old page against the new viewer
        if (typeof m.page === 'string' && m.page) {
            if (!pageId) pageId = m.page;
            else if (m.page !== pageId) { setStatus('WolfViewer was updated - loading the new version…'); location.reload(); return; }
        }
        onAllowed(); vrAudio.onConnected(); return;
    }
    if (m.t === 'pong') {
        const now = performance.now();
        const rtt = now - m.ct;
        pings.push({ rtt, off: m.st - (m.ct + now) / 2 });
        if (pings.length > 10) pings.shift();
        const best = pings.reduce((a, b) => (b.rtt < a.rtt ? b : a));
        clockOff = best.off;
        num.rtt = best.rtt;
        send({ t: 'sync', off: best.off, rtt: best.rtt });
    } else if (m.t === 'config') {
        config = m;
        // [VR LEAN 2026-11-04] the numbers only when WolfVRDebugStats is on in the viewer (Paul: no unnecessary stats)
        $('wvr-numbers-card').hidden = !m.debug;
        bar.setDebug(!!m.debug);
        if (!m.debug) statsOn = false;
        setupDecoder(m);
        $('wvr-big-controls').hidden = !BIG();
        $('wvr-preview-hint').textContent = BIG()
            ? 'This is your WolfViewer window. Click, right-click and scroll on it here; in VR, point with the right controller.'
            : 'Drag the picture to turn your head here on the page; in the headset, just look around.';
        enterMode();
        checkCard.visible = !!m.check && WORLD();
        releaseKeys();
        if (quality.sent !== quality.level) setQuality(quality.level, '');   // this headset's starting step
        setStatus(`Connected. Picture ${m.w}×${m.h}, ${m.codec === 'h264' ? 'H.264 video' : 'JPEG pictures'} from ${m.encoder}.`);
    } else if (m.t === 'stats') {
        num.viewer = m;
    } else if (m.t === 'ui') {
        // [VR CONTROLS 2026-11-04] the action bar's state from the viewer (wolfvrui.cpp pump)
        bar.setState(m);
        layer.setSeated(m.sit === 2);
        if (typeof m.headset === 'string' && m.headset !== viewerHeadset) { viewerHeadset = m.headset; headsetChanged(); }
        if (typeof m.mic === 'boolean') micSetting = m.mic;
        showMicButton();
    } else if (m.t === 'panels') {
        layer.setPanels(m.p);   // [VR PANELS 2026-11-04] the viewer's open windows, each its own panel
    } else if (m.t === 'acted') {
        bar.onActed(m);   // [VR MENU FIX 2026-11-04] what a bar button did, shown on the Menu tab (or why not, on the bar)
    } else if (m.t === 'layerconfig') {
        layer.onConfig(m);   // [VR HUD LAYER]
    } else if (m.t === 'aim') {
        // [VR WORLD POINTER] where the viewer found the ray landed, and what it would do there
        worldAim.k = m.k; worldAim.d = typeof m.d === 'number' ? m.d : -1; worldAim.c = typeof m.c === 'string' ? m.c : ''; worldAim.o = m.o === 1;
        worldAim.at = performance.now();
        const label = worldAim.k === 'world' && !worldAim.c && worldAim.d > 0 ? 'go' : worldAim.c;
        if (label !== reticle.label) drawReticleLabel(label);
    }
}

function setupDecoder(cfg) {
    waitingKey = true;
    if (cfg.codec !== 'h264') { closeDecoder(); return; }
    if (decoder && decoderCodec === cfg.codecString && decoder.state === 'configured') return;   // same stream restarted
    closeDecoder();
    const conf = { codec: cfg.codecString, optimizeForLatency: true };
    VideoDecoder.isConfigSupported(conf).then((r) => {
        if (!r.supported) {
            // [VR 3D FIX 2026-11-04] Too big for this headset's decoder: the next quality step down (a smaller
            // picture), and only when even the smallest will not decode, JPEG pictures instead.
            if (cfg.w > 1800 && quality.level < QUALITY_STEPS - 1) { setQuality(quality.level + 1, ''); quality.top = Math.max(quality.top, quality.level); return; }
            send({ t: 'hello', webcodecs: false, ua: navigator.userAgent });
            return;
        }
        decoder = new VideoDecoder({
            output: onDecoded,
            error: (e) => { showError('The video decoder stopped: ' + e.message + '. Asking WolfViewer to start the picture again.'); closeDecoder(); askKey(true); }
        });
        decoder.configure(conf);
        decoderCodec = cfg.codecString;
    }).catch((e) => showError('Video set-up failed: ' + e.message));
}

function closeDecoder() {
    if (decoder) { try { decoder.close(); } catch (e) { /* already closed */ } }
    decoder = null;
    decoderCodec = '';
    pendingMeta.clear();
}

function askKey(force) {
    const now = performance.now();
    if (force || now - lastKeyAsk > 1000) { lastKeyAsk = now; send({ t: 'key' }); }
}

function parseHeader(buf) {
    const d = new DataView(buf);
    if (buf.byteLength < HEADER_MIN || d.getUint32(0, true) !== MAGIC) return null;
    const size = d.getUint32(4, true);
    if (size < HEADER_MIN || size > buf.byteLength) return null;
    // [VR STEREO 2026-11-04] byte 66 = two eyes side by side, drawn from the eye positions at 72..95 (head space)
    const sb = size >= 96 ? d.getUint8(66) : 0;
    const stereo = sb === 1 || sb === 2;
    const centre = sb === 3;   // [VR CENTRE] one centre view for both eyes, its window the first 4 tangents
    const eyes = stereo ? [0, 1, 2, 3, 4, 5].map((k) => d.getFloat32(72 + 4 * k, true)) : null;
    // [VR 3D FIX 2026-11-04] 2 = each half drawn for its own off-axis window, tangents at 96..127 (L, R, B, T per eye)
    const tan = (sb === 2 || sb === 3) && size >= 128 ? [0, 1, 2, 3, 4, 5, 6, 7].map((k) => d.getFloat32(96 + 4 * k, true)) : null;
    // [VR DEPTH 2026-11-04] 128..139 the head position drawn for (140 = known), 144 depth bytes after the picture,
    // 148 / 150 the depth grid's size, 152 / 156 near / far
    const deep = size >= 160;
    return {
        size, stereo, centre, eyes, tan, stream: d.getUint8(67),
        pos: deep && d.getUint8(140) === 1 ? [d.getFloat32(128, true), d.getFloat32(132, true), d.getFloat32(136, true)] : null,
        depthBytes: deep ? d.getUint32(144, true) : 0,
        dw: deep ? d.getUint16(148, true) : 0, dh: deep ? d.getUint16(150, true) : 0,
        near: deep ? d.getFloat32(152, true) : 0, far: deep ? d.getFloat32(156, true) : 0,
        frame: Number(d.getBigUint64(8, true)),
        captureMs: d.getFloat64(16, true),
        poseClientMs: d.getFloat64(24, true),
        poseSeq: d.getUint32(32, true),
        q: new THREE.Quaternion(d.getFloat32(36, true), d.getFloat32(40, true), d.getFloat32(44, true), d.getFloat32(48, true)),
        fov: d.getFloat32(52, true),
        renderMs: d.getFloat32(56, true),
        encodeMs: d.getFloat32(60, true),
        key: d.getUint8(64) === 1,
        codec: d.getUint8(65) === 0 ? 'h264' : 'jpeg',
        bytes: d.getUint32(68, true),
        st: size >= 200 && d.getFloat64(160, true) > 0 ? {   // [VR STAGES]
            R: d.getFloat64(160, true), frame: d.getFloat32(168, true), eye0: d.getFloat32(172, true),
            issue: d.getFloat32(176, true), collect: d.getFloat32(180, true), encIn: d.getFloat32(184, true),
            encOut: d.getFloat32(188, true), sent: d.getFloat32(192, true) } : null,
        recvAt: performance.now(), decodedAt: NaN,
    };
}

function offerPicture(img, hdr, decodeMs, depth) {
    // Newest wins: a picture that was never drawn is dropped for a newer one (and one that comes late is dropped).
    if (latest && latest.hdr.frame > hdr.frame) { img.close(); return; }
    if (latest) latest.img.close();
    latest = { img, hdr, decodeMs, depth };
}
// [VR DEPTH] the picture's depth, unpacked alongside its decode; a picture waits for its own depth
function depthOf(buf, hdr) {
    if (!hdr.depthBytes || !hdr.dw || !hdr.dh || !hdr.pos || hdr.size + hdr.bytes + hdr.depthBytes > buf.byteLength) return null;
    return inflateDepth(new Uint8Array(buf, hdr.size + hdr.bytes, hdr.depthBytes)).catch(() => null);
}

function onPicture(buf) {
    const hdr = parseHeader(buf);
    if (!hdr) return;
    if (hdr.stream === 0) { num.got++; num.gotTotal++; }   // pictures that arrived (adaptQuality: did the decoder keep up?)
    if (hdr.size + hdr.bytes > buf.byteLength) return;
    if (hdr.stream === 1) { layer.onPicture(buf, hdr); return; }   // [VR HUD LAYER] its own decoder
    if (hdr.stream === 2) { if (onSound) onSound(buf, hdr); return; }   // [VR AUDIO]
    const payload = new Uint8Array(buf, hdr.size, hdr.bytes);
    if (hdr.codec === 'jpeg') {
        const t0 = performance.now();
        // WebGL ignores UNPACK_FLIP_Y for ImageBitmap, so flip while decoding (texture.flipY = false below).
        const dp = depthOf(buf, hdr);
        createImageBitmap(new Blob([payload], { type: 'image/jpeg' }), { imageOrientation: 'flipY' })
            .then(async (bmp) => { num.decoded++; hdr.decodedAt = performance.now(); offerPicture(bmp, hdr, performance.now() - t0, dp ? await dp : null); })
            .catch(() => { /* a bad picture is skipped */ });
        return;
    }
    if (!decoder || decoder.state !== 'configured') return;
    if (waitingKey) {
        if (!hdr.key) { askKey(false); return; }
        waitingKey = false;
    }
    if (decoder.decodeQueueSize > 4) { waitingKey = true; askKey(false); return; }   // decoder far behind: restart clean
    pendingMeta.set(hdr.frame, { hdr, t0: performance.now(), depth: depthOf(buf, hdr) });
    decoder.decode(new EncodedVideoChunk({ type: hdr.key ? 'key' : 'delta', timestamp: hdr.frame, data: payload }));
}

function onDecoded(vf) {
    num.decoded++;
    const meta = pendingMeta.get(vf.timestamp);
    pendingMeta.delete(vf.timestamp);
    if (!meta) { vf.close(); return; }
    const ms = performance.now() - meta.t0;
    meta.hdr.decodedAt = performance.now();   // [VR STAGES]
    if (meta.depth) meta.depth.then((d) => offerPicture(vf, meta.hdr, ms, d));
    else offerPicture(vf, meta.hdr, ms, null);
}

// ------------------------------------------------------------------------------------------------ scene
const wrap = $('wvr-canvas-wrap');
const renderer = new THREE.WebGLRenderer({ antialias: false, alpha: false });
// [VR DEVICE REPORT] a shader this headset's GPU will not compile (three skips drawing it): its own words to the log
renderer.debug.onShaderError = (gl, program, vs, fs) => {
    const info = [gl.getProgramInfoLog(program), gl.getShaderInfoLog(vs), gl.getShaderInfoLog(fs)].filter(Boolean).join(' | ');
    devLog.shaderErrors = (devLog.shaderErrors || 0) + 1;
    reportError('shader', info || 'link failed', '');
};
renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
renderer.outputColorSpace = THREE.SRGBColorSpace;
wrap.appendChild(renderer.domElement);
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x000000);
const dolly = new THREE.Group();            // the XR reference-space origin (threejs-webxr-contract: view = camera.parent)
scene.add(dolly);
// [VR DEPTH] far enough for the world-locked eyes' far shell (vrdepth.js SHELL) and the card behind it
const camera = new THREE.PerspectiveCamera(100, 1, 0.05, 2000);
camera.position.set(0, 1.6, 0);
dolly.add(camera);

const videoTex = new THREE.VideoFrameTexture();
videoTex.colorSpace = THREE.SRGBColorSpace;
const bitmapTex = new THREE.Texture();
bitmapTex.colorSpace = THREE.SRGBColorSpace;
bitmapTex.flipY = false;
bitmapTex.generateMipmaps = false;
bitmapTex.minFilter = THREE.LinearFilter;
const screenMat = new THREE.MeshBasicMaterial({ color: 0xffffff, side: THREE.DoubleSide, toneMapped: false });
const screen = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), screenMat);
screen.frustumCulled = false;
screen.visible = false;
scene.add(screen);

// [VR STEREO 2026-11-04] Paul: "do stereo now please". A stereo picture is two eyes side by side (left half = left eye),
// each drawn by the viewer from that eye's own position. Each half goes on its own window into the world, seen by
// that eye only: three.js draws layer 1 for the left eye and layer 2 for the right (WebXRManager cameraL / cameraR
// layer masks), and each window is placed from its own eye, so the depth is the viewer's, not the page's.
function halfPlane(right) {
    const g = new THREE.PlaneGeometry(1, 1);
    const uv = g.attributes.uv;
    for (let i = 0; i < uv.count; i++) uv.setX(i, uv.getX(i) * 0.5 + (right ? 0.5 : 0));
    const m = new THREE.Mesh(g, screenMat);
    m.frustumCulled = false;
    m.visible = false;
    m.layers.set(right ? 2 : 1);
    scene.add(m);
    return m;
}
const eyeScreens = [halfPlane(false), halfPlane(true)];
// [VR DEPTH 2026-11-04] each eye's picture as a world-locked mesh from its depth (vrdepth.js); the cards above stay as
// the far shell behind it (what shows through the cut edges, and the sky), or as before for a picture without depth
for (const m of eyeScreens) m.renderOrder = -10;
screenMat.depthWrite = false;
const depthEyes = new DepthEyes(scene);
const SHELL_DISTANCE = SHELL + 100;
// [VR 3D FIX 2026-11-04] Which XR camera is which eye: three.js gives XR view i to its camera i, and camera 0 draws
// layer 1, camera 1 layer 2 (WebXRManager: cameraL mask without layer 2, cameraR without layer 1). The views are put
// in whatever order the browser lists them, so each half goes on the layer of the view whose XRView.eye says it is
// that eye, not on "the first one" (eyesOf sets this).
const eyeLayer = [1, 2];

// [VR HUD CLICKS 2026-11-04] The docked surfaces (action bar, Menu tab, HUDs layer) hang on a head group placed HERE
// from the viewer pose each frame, before anything is aimed at them, so what is drawn and what is hit are the same:
// three's own XR camera sits a few centimetres behind the eyes (setProjectionFromUnion) and a frame late for the ray.
const dock = new THREE.Group();
dock.name = 'WolfVRDock';
scene.add(dock);

// [VR WORLD POINTER 2026-11-04] The dot where the right controller's ray lands, and what the viewer would do there
// (its cursor: "touch", "sit", "buy", ...) as a small label beside it, so a resident can see a chair can be sat on
// before pressing. A press makes it pulse (the press landed).
const reticle = { group: new THREE.Group(), label: '', pulseUntil: 0 };
{
    const ring = new THREE.Mesh(new THREE.RingGeometry(0.6, 1.0, 32), new THREE.MeshBasicMaterial({ color: 0x84cc16, depthTest: false, transparent: true, opacity: 0.95 }));
    ring.renderOrder = 9999;
    const core = new THREE.Mesh(new THREE.CircleGeometry(0.3, 20), new THREE.MeshBasicMaterial({ color: 0xffffff, depthTest: false }));
    core.renderOrder = 9999;
    const lc = document.createElement('canvas');
    lc.width = 256; lc.height = 64;
    reticle.ctx = lc.getContext('2d');
    reticle.tex = new THREE.CanvasTexture(lc);
    reticle.tex.colorSpace = THREE.SRGBColorSpace;
    reticle.tag = new THREE.Mesh(new THREE.PlaneGeometry(4, 1), new THREE.MeshBasicMaterial({ map: reticle.tex, transparent: true, depthTest: false }));
    reticle.tag.position.set(3.2, 0, 0);
    reticle.tag.renderOrder = 9999;
    reticle.tag.visible = false;
    reticle.ring = ring;
    for (const m of [ring, core, reticle.tag]) { m.frustumCulled = false; reticle.group.add(m); }
    reticle.group.visible = false;
    scene.add(reticle.group);
}
const RETICLE_WORDS = { touch: 'Touch', sit: 'Sit', buy: 'Buy', pay: 'Pay', open: 'Open', media: 'Play', grab: 'Grab', text: '',
                        go: 'Twice: go here' };   // [VR TELEPORT] a spot with nothing to do: the trigger twice goes there
function drawReticleLabel(c) {
    reticle.label = c;
    const word = RETICLE_WORDS[c] || '';
    const ctx = reticle.ctx;
    ctx.clearRect(0, 0, 256, 64);
    if (word) {
        ctx.font = c === 'go' ? 'bold 26px sans-serif' : 'bold 34px sans-serif';
        ctx.fillStyle = 'rgba(15, 23, 42, 0.85)';
        ctx.fillRect(0, 6, Math.min(256, ctx.measureText(word).width + 24), 52);
        ctx.fillStyle = '#ffffff';
        ctx.textBaseline = 'middle';
        ctx.fillText(word, 12, 33);
    }
    reticle.tex.needsUpdate = true;
    reticle.tag.visible = !!word;
    reticle.ring.material.color.set(word && c !== 'go' ? 0x84cc16 : c === 'go' ? 0x38bdf8 : 0xffffff);   // go: blue
}
// put the reticle at `p` (world), facing `from`, sized to stay the same apparent size
function placeReticle(p, from, big) {
    const dist = Math.max(0.2, p.distanceTo(from));
    reticle.group.position.copy(p).lerp(from, 0.004);
    reticle.group.lookAt(from);
    const pulse = performance.now() < reticle.pulseUntil ? 1.8 : 1;
    reticle.group.scale.setScalar(dist * (big ? 0.012 : 0.008) * pulse);
    reticle.group.visible = true;
}
const worldAim = { k: 'none', d: -1, c: '', at: 0 };
// [VR 3D FIX 2026-11-04] WolfVRStereoCheck: a card the HEADSET draws itself, 1 m in front of you, beside the world, so a
// resident (or Paul) can compare: the card is certainly in 3D; the world should look just as deep.
const checkCard = new THREE.Group();
{
    const c = document.createElement('canvas');
    c.width = 512; c.height = 256;
    const g = c.getContext('2d');
    g.fillStyle = 'rgba(15, 23, 42, 0.9)'; g.fillRect(0, 0, 512, 256);
    g.strokeStyle = '#84cc16'; g.lineWidth = 6; g.strokeRect(3, 3, 506, 250);
    g.fillStyle = '#ffffff'; g.font = 'bold 34px sans-serif'; g.textAlign = 'center';
    g.fillText('3D check: 1 metre away', 256, 70);
    g.font = '26px sans-serif';
    g.fillText('Close one eye, then the other:', 256, 130);
    g.fillText('this card and the world both shift.', 256, 170);
    const t = new THREE.CanvasTexture(c);
    t.colorSpace = THREE.SRGBColorSpace;
    const card = new THREE.Mesh(new THREE.PlaneGeometry(0.4, 0.2), new THREE.MeshBasicMaterial({ map: t, transparent: true }));
    card.position.set(-0.25, 0.05, -1.0);
    const cube = new THREE.Mesh(new THREE.BoxGeometry(0.1, 0.1, 0.1), new THREE.MeshBasicMaterial({ color: 0x84cc16, wireframe: true }));
    cube.position.set(0.2, 0.05, -1.0);
    cube.rotation.set(0.5, 0.6, 0);
    checkCard.add(card, cube);
    checkCard.visible = false;
    dock.add(checkCard);
}

// [VR CONTROLS 2026-11-04] WolfStorm's VR action bar (vrbar.js) and the viewer's UI layer (vrlayer.js), both docked to
// the head like WolfStorm's surfaces. VR 3D Mode only: Big Screen Mode already shows the whole viewer window.
const WORLD = () => !!(config && config.mode === 'world');
const bar = new VRBar(dock, send);
// a HUDs-layer problem is shown IN the headset too (the bar's message line), not only on the 2D page nobody sees in VR
const layer = new VRLayer(dock, send, (t) => { showError(t); bar.toast(t, 15000); });
layer.attachWorld(scene);   // [VR PANELS 2026-11-04] each viewer window its own panel, standing in the room
// [VR AUDIO 2026-11-04] the viewer's sound in the headset (vraudio.js), and the headset microphone for voice chat
const vrAudio = new VRAudio({
    send,
    sendBinary: (buf) => { if (ws && ws.readyState === WebSocket.OPEN && ws.bufferedAmount < 262144) ws.send(buf); },
    clockOff: () => clockOff,
    onError: showError,
    onMicWanted: () => showMicButton(),
});
const onSound = (buf, hdr) => vrAudio.onBlock(buf, hdr);
// The microphone switch on this page: the same setting as "Use the headset microphone for voice" in WolfViewer's
// Headset Settings (wolfvrui.cpp "mic"); turning it on asks this browser's permission here, in 2D, once.
let micSetting = false;
function showMicButton() {
    const b = $('wvr-mic');
    if (!b) return;
    b.hidden = !ws || ws.readyState !== WebSocket.OPEN;
    b.textContent = micSetting ? 'Stop using the headset microphone' : 'Use the headset microphone for voice';
    b.setAttribute('aria-pressed', micSetting ? 'true' : 'false');
}
bar.setPageState('huds', layer.want);
bar.onPage = (id) => {
    if (id === 'huds') { layer.setWant(!layer.want); bar.setPageState('huds', layer.want); bar.toast(layer.want ? 'HUDs and windows shown' : 'HUDs and windows hidden'); }
    else if (id === 'bigscreen') { bar.hide(); switchMode(); }
    else if (id === 'numbers') { statsOn = !statsOn; bar.setPageState('numbers', statsOn); }
    else if (id === 'exit') { bar.hide(); toggleVR(); }
};
// A button that opens a window in the viewer: make sure the HUDs layer (where that window shows) is on.
const OPENS_WINDOW = new Set(['chat', 'voice', 'move', 'view', 'people', 'radar', 'search', 'map', 'minimap', 'wolf_experiences',
    'inventory', 'appearance', 'animationoverride', 'gestures', 'snapshot', 'quickprefs', 'wolf_helm', 'wolf_dashboard']);
bar.onCommand = (a) => {
    if (OPENS_WINDOW.has(a.cmd) && !layer.want) { layer.setWant(true); bar.setPageState('huds', true); }
};

// ------------------------------------------------------------------------------------------------ headsets
// [VR HEADSETS 2026-11-04] Paul: "make sure it works on a Meta Quest 2" and "also let the user choose what VR they
// have". Automatic (the default) works the headset out from its browser; a choice here or in WolfViewer's Headset
// Settings sets where the picture quality STARTS and the display rate; then it adapts by itself (adaptQuality): one
// step down when this headset's decoder or the network falls behind, one step back up after 30 s of steady pictures,
// never above the headset's own top step. Steps: wolfvrstream.cpp kQuality ([VR 3D FIX 2026-11-04] 0 = 2 x 1920 px
// eyes at 100 Mbit/s, 1 = 2 x 1728 at 80, 2 = 2 x 1440 at 55, 3 = 2 x 1152 at 35, 4 = 2 x 896 at 20).
//   level: starting step; top: the best step it may climb to; hz: display rate asked for (0 = nearest the stream's);
//   ctl: what the controllers are
const QUALITY_STEPS = 5;
const HEADSETS = {
    // [VR QUALITY 2026-11-04] Paul's Quest 3 climbed to 1920 px eyes (step 0) and got 11-18 pictures a second, the
    // encoder 19-28 ms a picture at that size: it starts at 1440 now and never goes past 1728 by itself
    quest3:   { label: 'Meta Quest 3 / 3S', level: 2, top: 1, hz: 0, ctl: 'touch' },
    quest2:   { label: 'Meta Quest 2', level: 3, top: 3, hz: 72, ctl: 'touch' },
    questpro: { label: 'Meta Quest Pro', level: 2, top: 2, hz: 72, ctl: 'touch' },
    pico4:    { label: 'Pico 4 / Pico 4 Ultra', level: 2, top: 2, hz: 72, ctl: 'pico' },
    avp:      { label: 'Apple Vision Pro', level: 1, top: 0, hz: 90, ctl: 'hands' },
    other:    { label: 'Other headset', level: 3, top: 3, hz: 0, ctl: 'touch' },
};
// The browser says which headset it is: Meta's Quest Browser names the model in its user agent ("Quest 3",
// "Quest 3S", "Quest 2", "Quest Pro"); PICO's browser says "Pico"; Safari on Vision Pro reports a Mac but, unlike
// Safari on a Mac, has WebXR (navigator.xr).
function detectHeadset() {
    const ua = navigator.userAgent || '';
    if (/Quest 3/i.test(ua)) return 'quest3';
    if (/Quest Pro/i.test(ua)) return 'questpro';
    if (/Quest 2/i.test(ua)) return 'quest2';
    if (/Quest/i.test(ua)) return 'quest2';            // an unnamed Quest: the gentler settings
    if (/Pico/i.test(ua)) return 'pico4';
    if (/Macintosh/.test(ua) && 'xr' in navigator && !/Chrome|Firefox|Edg/.test(ua)) return 'avp';
    return 'other';
}
let viewerHeadset = 'auto';
function pageHeadset() { try { return localStorage.getItem('wolfvr.headset') || ''; } catch (e) { return ''; } }
function headsetKind() {
    if (viewerHeadset && viewerHeadset !== 'auto' && HEADSETS[viewerHeadset]) return viewerHeadset;
    const p = pageHeadset();
    if (p && p !== 'auto' && HEADSETS[p]) return p;
    return detectHeadset();
}
function headsetAuto() { return !(viewerHeadset && viewerHeadset !== 'auto') && !(pageHeadset() && pageHeadset() !== 'auto'); }
const quality = { level: HEADSETS[headsetKind()].level, top: HEADSETS[headsetKind()].top, sent: -1, bad: 0, goodSince: performance.now(), coolUntil: 0 };
function setQuality(level, why) {
    quality.level = Math.max(0, Math.min(QUALITY_STEPS - 1, level));
    quality.sent = quality.level;
    send({ t: 'quality', l: quality.level });
    if (why) toast(why);
}
function headsetChanged() {
    const h = HEADSETS[headsetKind()];
    quality.top = h.top;
    if (quality.level !== h.level) setQuality(h.level, '');
    quality.goodSince = performance.now();
    showHeadsetLine();
}
// Every second, from the numbers: is this headset keeping up?
function adaptQuality(now) {
    if (!renderer.xr.isPresenting || quality.pinned) return;
    if (now < quality.coolUntil) return;
    // behind = this headset's decoder cannot keep up with the pictures it was sent (decoded fewer than arrived, or its
    // queue grows), or the pictures arrive late (viewer capture to drawn here, on the synced clock: a queue building up
    // on the network). [VR 3D FIX 2026-11-04] Not "drawn fewer than arrived" (a 60 a second stream on a 72 or 90 Hz
    // display shows two pictures in one display frame now and then, by design) and not the decode TIME (a hardware
    // decoder takes longer per picture the bigger it is, while still keeping up): on Paul's Quest 3 those two stepped
    // the picture down to its smallest within 30 s, which is why it looked soft.
    const got = num.gotFps;
    // [VR QUALITY 2026-11-04] which one it was, for the 30-second summary in the viewer's log (Paul's Quest 3 stepped
    // down to 1152 px eyes 12 s in and nobody could tell why)
    const why = (got > 20 && num.decodedFps < got * 0.85) ? `decoded ${fmt(num.decodedFps, 0)} of ${fmt(got, 0)}/s`
              : num.v2p.avg() > 250 ? `pictures ${fmt(num.v2p.avg(), 0)} ms old`
              : (decoder && decoder.decodeQueueSize > 3) ? `decoder queue ${decoder.decodeQueueSize}`
              // [VR QUALITY 2026-11-04] too few pictures: a smaller picture is quicker to encode, send and decode (it
              // does not make the PC draw faster, so not below 1152 px for this reason)
              : (got > 0 && got < 24 && quality.level < 3) ? `only ${fmt(got, 0)} pictures a second` : '';
    if (why) {
        quality.goodSince = now;
        quality.lastWhy = why;
        // five seconds in a row (a single slow second - a teleport, a big texture load - is not the headset)
        if (++quality.bad >= 5 && quality.level < QUALITY_STEPS - 1) {
            quality.downs = (quality.downs || 0) + 1;
            quality.bad = 0;
            quality.coolUntil = now + 8000;
            setQuality(quality.level + 1, 'The picture is a little less sharp for now, so it keeps up.');
        }
        return;
    }
    quality.bad = 0;
    const top = quality.top;
    if (quality.level > top && now - quality.goodSince > 20000 && got >= 40) {   // sharper only with frames to spare
        quality.goodSince = now;
        quality.coolUntil = now + 8000;
        setQuality(quality.level - 1, 'The picture is sharper again.');
    }
}
function showHeadsetLine() {
    const k = headsetKind();
    const el = $('wvr-headset-line');
    if (el) el.textContent = 'Headset: ' + HEADSETS[k].label + (headsetAuto() ? ' (worked out by itself)' : '');
    const note = $('wvr-headset-note');
    if (note) {
        note.textContent = HEADSETS[k].ctl === 'hands'
            ? 'On this headset: look at a button and pinch to press it. Walking about needs controllers, which this headset does not have.'
            : HEADSETS[k].ctl === 'pico'
                ? 'PICO controllers work like the ones described here: the left has X, Y and the menu button, the right has A and B.'
                : '';
        note.hidden = !note.textContent;
    }
}
// First visit only (nothing chosen here or in WolfViewer): ask, with Automatic already ticked. Skippable; remembered.
function setupHeadsetCard() {
    const card = $('wvr-headset-card');
    if (!card) return;
    const form = $('wvr-headset-form');
    form.addEventListener('submit', (e) => e.preventDefault());   // a choice, not a form to send
    const pick = (v) => {
        try { localStorage.setItem('wolfvr.headset', v); } catch (e) { /* not remembered: asked again next time */ }
        card.hidden = true;
        headsetChanged();
        toast(v === 'auto' ? 'WolfViewer will work out your headset by itself.' : 'Headset set to ' + HEADSETS[v].label + '.');
    };
    $('wvr-headset-use').addEventListener('click', () => {
        const v = (form.querySelector('input[name="wvr-headset"]:checked') || {}).value || 'auto';
        pick(v);
    });
    $('wvr-headset-skip').addEventListener('click', () => pick('auto'));
    $('wvr-headset-change').addEventListener('click', () => {
        const cur = pageHeadset() || 'auto';
        const r = form.querySelector(`input[value="${cur}"]`);
        if (r) r.checked = true;
        card.hidden = false;
        card.scrollIntoView({ block: 'center' });
    });
    card.hidden = !!pageHeadset();
    showHeadsetLine();
}
setupHeadsetCard();
$('wvr-mic').addEventListener('click', () => {
    micSetting = !micSetting;
    send({ t: 'act', a: 'mic', x: micSetting ? '1' : '0' });
    if (micSetting) { vrAudio.micWanted = true; vrAudio.prepareMic(); }   // asked now, from this press
    toast(micSetting ? 'Your headset microphone is used for voice chat.' : 'Your PC microphone is used for voice chat again.');
    showMicButton();
});

// Each eye's position in head space and its own frustum, from the XR views (WebXR XRView.eye, .transform,
// .projectionMatrix), sent with each pose for the viewer to draw the eyes from; left eye first, by XRView.eye.
// [VR 3D FIX 2026-11-04] The frustum goes as the tangents of its four edges (the viewer draws exactly that off-axis
// window), each widened by EYE_MARGIN_DEG so a quick head turn does not show the picture's edge before the next one
// comes (reprojection); the square field of view covering both ("f") stays for a viewer drawing one view.
const EYE_MARGIN_DEG = 4;
const _hinv = new THREE.Matrix4(), _ev = new THREE.Vector3();
let eyeFov = 0, eyeTan = null, lastViews = null;
function widen(t, sign) { return Math.tan(Math.atan(t) + sign * EYE_MARGIN_DEG * Math.PI / 180); }
function eyesOf(pose) {
    if (!pose || !pose.views || pose.views.length < 2) return null;
    _hinv.fromArray(pose.transform.matrix).invert();
    // which view is which eye ("none" / missing: the first is the left, as three.js assumes)
    let li = pose.views.findIndex((v) => v.eye === 'left'), ri = pose.views.findIndex((v) => v.eye === 'right');
    if (li < 0 || ri < 0 || li > 1 || ri > 1) { li = 0; ri = 1; }
    eyeLayer[0] = 1 + li; eyeLayer[1] = 1 + ri;
    const e = [], pt = [];
    let maxTan = 0;
    for (const i of [li, ri]) {
        const v = pose.views[i];
        _ev.setFromMatrixPosition(_m.fromArray(v.transform.matrix)).applyMatrix4(_hinv);
        e.push(+_ev.x.toFixed(4), +_ev.y.toFixed(4), +_ev.z.toFixed(4));
        // a perspective projection's edges: x/-z = (m8 -+ 1) / m0, y/-z = (m9 -+ 1) / m5 (column-major)
        const p = v.projectionMatrix;
        const L = (p[8] - 1) / p[0], R = (p[8] + 1) / p[0], B = (p[9] - 1) / p[5], T = (p[9] + 1) / p[5];
        pt.push(+widen(L, -1).toFixed(4), +widen(R, 1).toFixed(4), +widen(B, -1).toFixed(4), +widen(T, 1).toFixed(4));
        maxTan = Math.max(maxTan, Math.abs(L), Math.abs(R), Math.abs(B), Math.abs(T));
    }
    eyeFov = Math.min(120, Math.max(90, Math.round(2 * Math.atan(maxTan) * 180 / Math.PI + 4)));
    eyeTan = pt;
    lastViews = pose.views;   // for the test hook only (window.__wolfvr.eyes); nothing is copied per frame
    return e;
}

// ------------------------------------------------------------------------------------------------ big screen
// [VR BIG SCREEN 2026-11-03] Paul: "I'm dying to go in-world with it". config.mode "screen": WolfViewer sends its
// whole window; it is shown on a large, gently curved screen in front of you (part of a cylinder centred on where
// you stand, so every point of it is the same distance from your eyes), and the right controller is the mouse.
// Sizes are the screen's width in metres at SCREEN_RADIUS; the default (index 2) is about a 75-inch TV seen from
// 2.5 m. Left stick up/down changes it, the left menu button recentres it in front of you.
const BIG = () => !!(config && config.mode === 'screen');
const SCREEN_RADIUS = 2.5;
const SCREEN_SIZES = [1.6, 2.0, 2.6, 3.2, 4.0, 5.0];
let screenSizeIndex = 3;
try {
    const raw = localStorage.getItem('wolfvr.screenSize');   // null on a first visit (Number(null) would be 0)
    const v = raw === null ? NaN : Number(raw);
    if (Number.isInteger(v) && v >= 0 && v < SCREEN_SIZES.length) screenSizeIndex = v;
} catch (e) { /* storage blocked: the default size */ }
const bigGroup = new THREE.Group();          // centred on the user's head when placed; faces -Z
bigGroup.position.set(0, 1.6, 0);
scene.add(bigGroup);
const bigMat = new THREE.MeshBasicMaterial({ color: 0xffffff, side: THREE.DoubleSide, toneMapped: false });
const bigMesh = new THREE.Mesh(new THREE.BufferGeometry(), bigMat);
bigMesh.frustumCulled = false;
bigMesh.visible = false;
bigGroup.add(bigMesh);
let bigArc = 1, bigHeight = 1;               // radians, metres
function buildBigScreen() {
    const width = SCREEN_SIZES[screenSizeIndex];
    const aspect = config && config.w && config.h ? config.w / config.h : 16 / 9;
    bigArc = width / SCREEN_RADIUS;
    bigHeight = width / aspect;
    const N = 48, pos = [], uv = [], idx = [];
    for (let i = 0; i <= N; i++) {
        const u = i / N, a = -bigArc / 2 + bigArc * u;
        const x = SCREEN_RADIUS * Math.sin(a), z = -SCREEN_RADIUS * Math.cos(a);
        pos.push(x, bigHeight / 2, z, x, -bigHeight / 2, z);
        uv.push(u, 1, u, 0);
        if (i < N) { const k = i * 2; idx.push(k, k + 1, k + 2, k + 1, k + 3, k + 2); }
    }
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
    g.setAttribute('uv', new THREE.Float32BufferAttribute(uv, 2));
    g.setIndex(idx);
    bigMesh.geometry.dispose();
    bigMesh.geometry = g;
    const lbl = $('wvr-size');
    if (lbl) lbl.textContent = `${width.toFixed(1)} m wide`;
}
function setScreenSize(i) {
    screenSizeIndex = Math.max(0, Math.min(SCREEN_SIZES.length - 1, i));
    try { localStorage.setItem('wolfvr.screenSize', String(screenSizeIndex)); } catch (e) { /* see above */ }
    buildBigScreen();
    toast(`Screen ${SCREEN_SIZES[screenSizeIndex].toFixed(1)} m wide`);
    sizeFlashUntil = performance.now() + 2500;   // in VR the numbers panel shows the new size for a moment
}
// Put the screen straight in front of the head (yaw only, so it stays upright).
let bigPlaced = false, sizeFlashUntil = 0;
function recentre(headPos, headQuat) {
    const e = new THREE.Euler().setFromQuaternion(headQuat, 'YXZ');
    bigGroup.position.copy(headPos);
    bigGroup.quaternion.setFromAxisAngle(new THREE.Vector3(0, 1, 0), e.y);
    bigPlaced = true;
}
// The laser and the dot where it meets the screen (WolfStorm vr_menu.js pointer look: a thin light ray, bright tip).
const laserGeom = new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(), new THREE.Vector3(0, 0, -1)]);
const laser = new THREE.Line(laserGeom, new THREE.LineBasicMaterial({ color: 0x9fd0ff, transparent: true, opacity: 0.7, depthTest: false }));
laser.renderOrder = 9998; laser.frustumCulled = false; laser.visible = false;
scene.add(laser);
const dot = new THREE.Mesh(new THREE.CircleGeometry(0.012, 20), new THREE.MeshBasicMaterial({ color: 0xffffff, depthTest: false }));
dot.renderOrder = 9999; dot.frustumCulled = false; dot.visible = false;
scene.add(dot);
const _inv = new THREE.Matrix4(), _lo = new THREE.Vector3(), _ld = new THREE.Vector3();
// Ray (world) -> {u, v, point} on the big screen, or null. In the screen's own frame the surface is x^2 + z^2 = R^2,
// |y| <= height/2, angle within the arc; the ray starts inside the cylinder, so the far root is the hit.
function hitBigScreen(origin, dir) {
    bigGroup.updateMatrixWorld();
    _inv.copy(bigGroup.matrixWorld).invert();
    _lo.copy(origin).applyMatrix4(_inv);
    _ld.copy(dir).transformDirection(_inv);
    const a = _ld.x * _ld.x + _ld.z * _ld.z;
    if (a < 1e-9) return null;
    const b = 2 * (_lo.x * _ld.x + _lo.z * _ld.z), c = _lo.x * _lo.x + _lo.z * _lo.z - SCREEN_RADIUS * SCREEN_RADIUS;
    const disc = b * b - 4 * a * c;
    if (disc < 0) return null;
    const t = (-b + Math.sqrt(disc)) / (2 * a);
    if (t <= 0) return null;
    const p = _lo.clone().addScaledVector(_ld, t);
    const ang = Math.atan2(p.x, -p.z);
    const u = (ang + bigArc / 2) / bigArc, v = 0.5 - p.y / bigHeight;
    if (u < 0 || u > 1 || v < 0 || v > 1) return null;
    return { u, v, point: p.applyMatrix4(bigGroup.matrixWorld), t };
}
// Pointer state sent to the viewer: moves at most once per frame and only when it moved; buttons on their edges.
const ptr = { u: -1, v: -1, buttons: {}, lastWheel: 0 };
function pointerMove(u, v) {
    if (Math.abs(u - ptr.u) < 0.0003 && Math.abs(v - ptr.v) < 0.0003) return;
    ptr.u = u; ptr.v = v;
    send({ t: 'in', e: 'move', u, v });
}
function pointerButton(b, down) {
    if (!!ptr.buttons[b] === down) return;
    ptr.buttons[b] = down;
    if (ptr.u < 0) return;
    send({ t: 'in', e: down ? 'down' : 'up', u: ptr.u, v: ptr.v, b });
}
function pointerWheel(d) {
    if (ptr.u < 0 || !d) return;
    send({ t: 'in', e: 'wheel', u: ptr.u, v: ptr.v, d });
}

// Numbers panel - WolfStorm vr_menu.js _buildStats/_updateStats look, docked to the camera (vr_hud.js rev 3).
const statsCanvas = document.createElement('canvas');
statsCanvas.width = 512; statsCanvas.height = 512;
const statsCtx = statsCanvas.getContext('2d');
const statsTex = new THREE.CanvasTexture(statsCanvas);
statsTex.colorSpace = THREE.SRGBColorSpace;
statsTex.generateMipmaps = false;
statsTex.minFilter = THREE.LinearFilter;
const statsMesh = new THREE.Mesh(new THREE.PlaneGeometry(0.34, 0.34),
    new THREE.MeshBasicMaterial({ map: statsTex, transparent: true, depthTest: false, side: THREE.DoubleSide }));
statsMesh.renderOrder = 9999;
statsMesh.frustumCulled = false;
statsMesh.position.set(-0.30, 0.16, -0.9);   // upper left of view, as WolfStorm's readout
statsMesh.visible = false;
camera.add(statsMesh);
let statsOn = false, statsLastDraw = 0;
// [VR SIMPLE 2026-11-03] Comfort vignette for VR 3D Mode: the outer part of the view darkens while the avatar walks,
// the usual remedy for motion sickness from moving without moving (a dark ring docked to the head, the centre clear).
const vignette = new THREE.Mesh(new THREE.RingGeometry(0.42, 3, 48),
    new THREE.MeshBasicMaterial({ color: 0x000000, transparent: true, opacity: 0.55, depthTest: false }));
vignette.position.set(0, 0, -0.5);
vignette.renderOrder = 9997;
vignette.frustumCulled = false;
vignette.visible = false;
camera.add(vignette);   // numbers panel: off until asked for (left menu held 1 s)

function fmt(x, d = 1) { return Number.isFinite(x) ? x.toFixed(d) : '–'; }
function numberRows() {
    const v = num.viewer;
    return [
        ['pose→photon', fmt(num.p2p.avg()) + ' ms', `p95 ${fmt(num.p2p.pct(0.95))}`],
        ['viewer→photon', fmt(num.v2p.avg()) + ' ms', ''],
        ['headset fps', fmt(num.fps), `new pictures ${fmt(num.newFps)}`],
        ['decode', fmt(num.decode.avg(), 2) + ' ms', ''],
        ['viewer encode', fmt(v.encodeMs, 2) + ' ms', `p95 ${fmt(v.encodeP95, 2)}`],
        ['viewer render', fmt(v.renderMs, 2) + ' ms', ''],
        ['pose→viewer', fmt(v.poseLatMs) + ' ms', `${fmt(v.poseHz)} poses/s`],
        ['round trip', fmt(num.rtt) + ' ms', ''],
        ['sent', fmt(v.fps) + ' fps', `${fmt((v.kbps || NaN) / 1000)} Mbit/s`],
        ['encoder', v.encoder || (config ? config.encoder : '–'), v.skipped ? `skipped ${v.skipped}` : ''],
    ].concat(BIG() ? [['screen', `${SCREEN_SIZES[screenSizeIndex].toFixed(1)} m wide`, 'left stick up/down']] : [])
     .concat([['sound', vrAudio.bufferedMs ? `${fmt(vrAudio.bufferedMs, 0)} ms buffered` : 'off',
        Number.isFinite(vrAudio.skewMs) ? `with picture ${fmt(vrAudio.skewMs, 0)} ms` : '']])
     .concat(!BIG() && screen.userData.hdr ? [['3D picture', screen.userData.hdr.stereo ? 'two eyes' : 'one view',
        `${fmt(screen.userData.hdr.fov, 0)}°` + (screen.userData.hdr.stereo ? ` eyes ${fmt(Math.abs(screen.userData.hdr.eyes[3] - screen.userData.hdr.eyes[0]) * 1000, 0)} mm apart` : '')]] : []);
}

function drawStatsPanel(now) {
    if (now - statsLastDraw < 500) return;
    statsLastDraw = now;
    const rows = numberRows();
    // DOM copy for the 2D page
    const dl = $('wvr-numbers');
    dl.replaceChildren(...rows.flatMap(([k, a, b]) => {
        const dt = document.createElement('dt'); dt.textContent = k;
        const dd = document.createElement('dd'); dd.textContent = b ? `${a}  (${b})` : a;
        return [dt, dd];
    }));
    if (!statsMesh.visible) return;
    const ctx = statsCtx;
    ctx.clearRect(0, 0, 512, 512);
    ctx.fillStyle = 'rgba(10, 14, 26, 0.85)';
    ctx.fillRect(0, 40, 512, 432);
    ctx.strokeStyle = 'rgba(255,255,255,0.25)';
    ctx.lineWidth = 3;
    ctx.strokeRect(0, 40, 512, 432);
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    let y = 66;
    for (const [k, a, b] of rows) {
        ctx.fillStyle = 'rgba(255,255,255,0.62)';
        ctx.font = '22px monospace';
        ctx.fillText(k, 14, y);
        ctx.fillStyle = '#eaf2ff';
        ctx.font = 'bold 24px monospace';
        ctx.fillText(a, 210, y);
        if (b) { ctx.fillStyle = 'rgba(255,255,255,0.62)'; ctx.font = '18px monospace'; ctx.fillText(b, 360, y); }
        y += 36;
    }
    statsTex.needsUpdate = true;
}

function resize() {
    const w = Math.max(1, wrap.clientWidth), h = Math.max(1, wrap.clientHeight);
    if (renderer.xr.isPresenting) return;
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
}
new ResizeObserver(resize).observe(wrap);

// 2D preview: drag to turn the head.
let dragYaw = 0, dragPitch = 0, dragging = null;
const _ray = new THREE.Raycaster();
function previewHit(e) {
    const r = renderer.domElement.getBoundingClientRect();
    _ray.setFromCamera(new THREE.Vector2(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1), camera);
    return hitBigScreen(_ray.ray.origin, _ray.ray.direction);
}
wrap.addEventListener('contextmenu', (e) => { if (BIG()) e.preventDefault(); });
wrap.addEventListener('wheel', (e) => {
    if (!BIG()) return;
    e.preventDefault();
    const h = previewHit(e);
    if (h) { pointerMove(h.u, h.v); pointerWheel(Math.sign(e.deltaY)); }
}, { passive: false });
wrap.addEventListener('pointerdown', (e) => {
    if (BIG()) {
        const h = previewHit(e);
        if (h) { pointerMove(h.u, h.v); pointerButton(e.button === 2 ? 2 : 0, true); }
        wrap.setPointerCapture(e.pointerId);
        return;
    }
    dragging = { x: e.clientX, y: e.clientY, yaw: dragYaw, pitch: dragPitch }; wrap.setPointerCapture(e.pointerId);
});
wrap.addEventListener('pointerup', (e) => { if (BIG()) pointerButton(e.button === 2 ? 2 : 0, false); });
wrap.addEventListener('pointermove', (e) => {
    if (BIG()) { const h = previewHit(e); if (h) pointerMove(h.u, h.v); return; }
    if (!dragging) return;
    dragYaw = dragging.yaw + (e.clientX - dragging.x) * 0.005;
    dragPitch = Math.max(-1.4, Math.min(1.4, dragging.pitch + (e.clientY - dragging.y) * 0.005));
});
wrap.addEventListener('pointerup', () => { dragging = null; });

// ------------------------------------------------------------------------------------------------ VR entry (WolfStorm toggleVR)
let vrSession = null, vrEntering = false, rigYaw = 0, snapArmed = false;
const menuWas = {};
const focusGuard = (ev) => { if (renderer.xr.isPresenting && ev.target && ev.target.blur && ev.target.id !== 'wvr-type') ev.target.blur(); };

async function toggleVR() {
    vrAudio.start();   // [VR AUDIO] the sound may only start from a press: this one, before anything is awaited
    if (!('xr' in navigator)) { showError('VR is not supported in this browser'); return; }
    const btn = $('vr-btn');
    if (vrSession === null) {
        if (vrEntering) return;
        vrEntering = true;
        try {
            const supported = await navigator.xr.isSessionSupported('immersive-vr');
            if (!supported) { showError('VR is not supported on this device'); return; }
            const gl = renderer.getContext();
            if (gl && gl.makeXRCompatible) await gl.makeXRCompatible();
            renderer.xr.enabled = true;
            renderer.xr.setFoveation(0);   // the streamed picture is the whole scene: keep its edges sharp
            document.addEventListener('focusin', focusGuard, true);
            const session = await navigator.xr.requestSession('immersive-vr', {
                optionalFeatures: ['local-floor', 'bounded-floor', 'hand-tracking', 'layers']
            });
            vrSession = session;
            session.addEventListener('end', onSessionEnd);
            session.addEventListener('visibilitychange', () => tellXR(false));   // [VR IMMERSIVE] headset menu / asleep
            session.addEventListener('select', onSelect);   // [VR CONTROLS] hands / look-and-pinch
            await renderer.xr.setSession(session);
            // As WolfStorm's updateTargetFrameRate: the supported display rate nearest the viewer's frame rate.
            const want = HEADSETS[headsetKind()].hz || (config && config.fps) || 72;   // [VR HEADSETS] Quest 2: 72 Hz
            if (session.supportedFrameRates && session.updateTargetFrameRate) {
                const rates = Array.from(session.supportedFrameRates);
                if (rates.length) {
                    const best = rates.reduce((a, b) => (Math.abs(b - want) < Math.abs(a - want) ? b : a));
                    session.updateTargetFrameRate(best).catch(() => {});
                }
            }
            statsMesh.visible = statsOn;
            tellXR(false);   // [VR IMMERSIVE] in VR now
            btn.classList.add('vr-active');
            btn.querySelector('.btn-label').textContent = 'Exit VR';
            btn.title = 'Exit VR Mode';
            showError('');
        } catch (e) {
            showError('Could not start VR: ' + (e && e.message ? e.message : e));
            if (vrSession) { try { await vrSession.end(); } catch (e2) { /* already ended */ } }
            else restoreAfterVR();
        } finally {
            vrEntering = false;
        }
    } else {
        await vrSession.end();
    }
}

function restoreAfterVR() {
    vrSession = null;
    tellXR(false);   // [VR IMMERSIVE] out of VR: the viewer's desktop is itself again at once
    document.removeEventListener('focusin', focusGuard, true);
    statsMesh.visible = false;
    const btn = $('vr-btn');
    btn.classList.remove('vr-active');
    btn.querySelector('.btn-label').textContent = 'Enter VR';
    btn.scrollIntoView({ block: 'center' });   // one tap to go back in
    btn.title = 'Enter VR Mode';
    camera.position.set(0, 1.6, 0);
    rigYaw = 0;
    bigGroup.position.set(0, 1.6, 0);
    bigGroup.quaternion.identity();
    bigPlaced = false;
    laser.visible = dot.visible = false;
    for (const b of [0, 2]) pointerButton(b, false);   // never leave a button held down in the viewer
    for (const b of [0, 2]) layerButton(b, false);
    for (const b of [0, 2]) { if (worldPointer.owner[b] === 'ptr') ptrButton(b, false); worldPointer.owner[b] = null; }
    worldPointer.trig = worldPointer.b = false;
    reticle.group.visible = false;
    bar.setTabActive(false);
    if (talkHeld) { talkHeld = false; if (bar.state.on.has('speak')) send({ t: 'act', a: 'speak' }); }   // stop talking
    bar.hide();
    releaseKeys();
    dolly.quaternion.identity();
    resize();
}
function onSessionEnd() { restoreAfterVR(); }
$('vr-btn').addEventListener('click', toggleVR);
// [VR MOVE 2026-11-03] Where the browser offers it (WebXR "sessiongranted", e.g. Quest Browser after a navigation from
// another VR page), go straight back into VR with no tap at all.
if (navigator.xr && navigator.xr.addEventListener) {
    navigator.xr.addEventListener('sessiongranted', () => { if (!vrSession && !vrEntering) toggleVR(); });
}

// [VR MOVE 2026-11-03] Controls, after Paul's first in-world try ("left controller must move him, not show stats").
// One map for both modes; WolfStorm's VR mapping where it has one (input_manager.js _vrLocomotionFlags,
// render_manager.js _updateVRInput), xr-standard indices (WebXR Gamepads Module): 0 trigger, 1 grip, 3 stick click,
// 4 A/X, 5 B/Y, 6+ menu; axes 2/3 = thumbstick x/y (y negative pushed forward).
//   LEFT   stick           walk / strafe; pushed all the way = run           (WolfStorm: left stick translates)
//          X (hold)        jump / fly up                                     (WolfStorm: A/X = up)
//          grip (hold)     fly down
//          menu            put the big screen back in front of you; hold 1 s = numbers panel
//          Y               the headset keyboard (big screen)
//          stick click     switch VR Big Screen Mode <-> VR 3D Mode
//   RIGHT  stick left/right  snap turn 45 degrees                            (WolfStorm: right X rotates, snap default)
//          big screen: aim = pointer, trigger = click, grip or B = right click,
//                      stick up/down = scroll while pointing at the screen, screen size while pointing away
//          3D: stick up/down = fly up / down                                 (WolfStorm: right Y = up/down)
const STICK_DEAD = 0.25;          // WolfStorm _vrLocomotionFlags DEAD
const leftWas = {}, rightWas = {};
let sizeArmed = true, menuDownAt = 0, menuLong = false;
const move = { x: 0, y: 0, sentAt: 0, lift: 0, liftAt: 0 };
function sendStick(x, y) {
    x = Math.abs(x) > STICK_DEAD ? Math.round(x * 100) / 100 : 0;
    y = Math.abs(y) > STICK_DEAD ? Math.round(y * 100) / 100 : 0;
    // [VR SIMPLE 2026-11-03] Comfort in VR 3D Mode: walking only, never the sudden speed-up of running (the PC runs
    // past 0.9, wolfvrscreen.cpp kRun), and the edges of the view dim while moving (vignette below).
    if (config && config.mode === 'world') {
        const m = Math.max(Math.abs(x), Math.abs(y));
        if (m > 0.85) { x = x * 0.85 / m; y = y * 0.85 / m; x = Math.round(x * 100) / 100; y = Math.round(y * 100) / 100; }
    }
    vignette.visible = !!(config && config.mode === 'world' && (x || y));
    const now = performance.now();
    const changed = Math.abs(x - move.x) > 0.04 || Math.abs(y - move.y) > 0.04;
    // while held, every 250 ms as well: the PC lets go by itself after a second of silence (wolfvrscreen.cpp)
    if (!changed && !((x || y) && now - move.sentAt > 250)) return;
    move.x = x; move.y = y; move.sentAt = now;
    send({ t: 'in', e: 'stick', x, y });
}
function sendLift(d) {
    const now = performance.now();
    if (d === move.lift && !(d && now - move.liftAt > 250)) return;
    move.lift = d; move.liftAt = now;
    send({ t: 'in', e: 'lift', d });
}
function releaseMovement() { sendStick(0, 0); sendLift(0); }
function releaseKeys() { releaseMovement(); }
// [VR MODES 2026-11-04] Paul: "When I'm in VR and went to screen mode, the big VR box was kind of blocking things".
// Each mode shows only its own surfaces: on every "config" (a new mode, or the same one restarted) everything of the
// other mode is taken down at once - not when its next picture happens to arrive. Big Screen: the curved screen and
// its pointer only (the screen already shows the whole viewer, HUDs and all). 3D: the eye pictures, the HUDs layer,
// the action bar and its Menu tab, the world pointer.
function enterMode() {
    const big = BIG(), world = WORLD();
    if (!big) bigMesh.visible = false;
    if (!world) {
        screen.visible = false;
        eyeScreens[0].visible = eyeScreens[1].visible = false;
        depthEyes.hide();
        screen.userData.hdr = null;
        bar.hide();
        bar.setTabActive(false);
        checkCard.visible = false;
        vignette.visible = false;
        reticle.group.visible = false;
        for (const b of [0, 2]) { if (worldPointer.owner[b] === 'ptr') ptrButton(b, false); worldPointer.owner[b] = null; }
        worldPointer.trig = worldPointer.b = false;
        worldAim.k = 'none';
    }
    if (big) { buildBigScreen(); bigPlaced = false; statsOn = false; }
    else { for (const b of [0, 2]) pointerButton(b, false); }
    laser.visible = dot.visible = false;
    layer.setActive(world && renderer.xr.isPresenting);
}

// Left stick click switches between VR Big Screen Mode and VR 3D Mode without reconnecting.
function switchMode() {
    const want = BIG() ? 'world' : 'screen';
    if (want === 'world' && !(config && config.world)) { toast('VR 3D Mode is not ready yet'); return; }
    releaseMovement();
    for (const b of [0, 2]) pointerButton(b, false);
    send({ t: 'mode', m: want });
    toast(want === 'world' ? 'Switching to VR 3D Mode' : 'Switching to VR Big Screen Mode');
}
const axis = (gp, i, j) => (gp.axes.length >= 4 ? gp.axes[i] : (gp.axes[j] || 0)) || 0;
const _ro = new THREE.Vector3(), _rd = new THREE.Vector3(), _rq = new THREE.Quaternion();

let leftProfiles = '';
function pollLeft(gp, headPos, headQuat) {
    const pressed = (i) => !!(gp.buttons[i] && gp.buttons[i].pressed);
    const edge = (i) => { const p = pressed(i); const was = leftWas[i]; leftWas[i] = p; return p && !was; };
    sendStick(axis(gp, 2, 0), axis(gp, 3, 1));
    // [VR HUD CLICKS 2026-11-04] The menu button is index 7 on Meta's Touch controllers (WebXR input profiles registry,
    // meta-quest-touch-plus / oculus-touch-v3, left: trigger, squeeze, -, thumbstick, x, y, THUMBREST, menu). Index 6
    // is the thumb rest - a sensor the thumb lies on - so "any button from 6 up" toggled the bar whenever the thumb
    // moved on or off the rest. Other controllers without that layout: their last button past the face buttons.
    const MENU = gp.buttons.length >= 8 ? 7 : (gp.buttons.length === 7 && !/touch|quest|oculus/i.test(leftProfiles) ? 6 : -1);
    const menu = MENU >= 0 && pressed(MENU);
    const now = performance.now();
    if (menu && !leftWas.menu) { menuDownAt = now; menuLong = false; }
    if (menu && !menuLong && now - menuDownAt > 1000) { menuLong = true; if (config && config.debug) statsOn = !statsOn; }   // hold: numbers (debug)
    if (!menu && leftWas.menu && !menuLong && BIG()) recentre(headPos, headQuat);             // tap: recentre
    if (!menu && leftWas.menu && !menuLong && WORLD()) bar.toggle();                          // 3D: the action bar
    leftWas.menu = menu;
    if (edge(5) && (BIG() || WORLD())) openKeyboard();   // 3D too: typing into a chat or search window on the HUDs layer
    if (edge(3)) switchMode();
    return pressed(4) ? 1 : pressed(1) ? -1 : 0;   // X = up, grip = down
}

function pollRightStick(gp, aimed) {
    const x = axis(gp, 2, 0), y = axis(gp, 3, 1);
    if (WORLD() && aimed) {
        // [VR HUD LAYER] pointing at a window or HUD: the stick scrolls it (grip held: makes the layer bigger / smaller)
        const now = performance.now();
        if (Math.abs(x) < 0.3) snapArmed = false;
        if (aimed === 'layer' && Math.abs(y) > 0.5 && now - ptr.lastWheel > (gp.buttons[1] && gp.buttons[1].pressed ? 400 : 120)) {
            ptr.lastWheel = now;
            if (gp.buttons[1] && gp.buttons[1].pressed) layer.nudge(0, y < 0 ? 1 : -1);
            else layerWheel(Math.sign(y));
        }
        return 0;
    }
    // render_manager.js _updateVRInput snap turn: one step past 0.6, re-armed under 0.3; stick right = turn right.
    // The AVATAR turns in the viewer (LLAgent::yaw), so the world and the big screen's view both follow.
    if (Math.abs(x) > 0.6 && !snapArmed) { snapArmed = true; send({ t: 'in', e: 'turn', d: x > 0 ? -1 : 1 }); }
    else if (Math.abs(x) < 0.3) snapArmed = false;
    if (BIG()) {
        const now = performance.now();
        if (aimed) {
            if (Math.abs(y) > 0.5 && now - ptr.lastWheel > 120) { ptr.lastWheel = now; pointerWheel(Math.sign(y)); }
        } else if (Math.abs(y) > 0.6 && sizeArmed) { sizeArmed = false; setScreenSize(screenSizeIndex + (y < 0 ? 1 : -1)); }
        if (Math.abs(y) < 0.3) sizeArmed = true;
        return 0;
    }
    return y < -0.5 ? 1 : y > 0.5 ? -1 : 0;   // 3D: forward = fly up, back = down
}

// Big screen: the right controller is the mouse; returns whether it points at the screen.
function pollPointer(src, gp, frame) {
    let aimed = false;
    const rp = frame.getPose(src.targetRaySpace, renderer.xr.getReferenceSpace());
    if (rp) {
        dolly.updateMatrixWorld();
        _m.fromArray(rp.transform.matrix).premultiply(dolly.matrixWorld);
        _m.decompose(_ro, _rq, _scl);
        _rd.set(0, 0, -1).applyQuaternion(_rq);
        const h = hitBigScreen(_ro, _rd);
        laser.position.copy(_ro); laser.quaternion.copy(_rq); laser.scale.set(1, 1, h ? h.t : 3); laser.visible = true;
        if (h) {
            aimed = true;
            dot.position.copy(h.point).lerp(_ro, 0.004);
            dot.lookAt(_ro);
            pointerMove(h.u, h.v);
        }
    }
    const pressed = (i) => !!(gp.buttons[i] && gp.buttons[i].pressed);
    if (aimed || !pressed(0)) pointerButton(0, pressed(0));
    if (aimed || !(pressed(1) || pressed(5))) pointerButton(2, pressed(1) || pressed(5));
    dot.visible = aimed;
    return aimed;
}

// [VR CONTROLS / HUD LAYER 2026-11-04] VR 3D Mode pointer (vr_menu.js: ONE raycast, the bar first, then the surfaces):
// the right controller's ray, in HEAD space because the bar and the layer are docked to the head (the dock group).
// [VR WORLD POINTER 2026-11-04] Off the bar, the ray goes to the viewer ({"t":"in","e":"ray"}, with where it crosses
// the HUDs layer): the viewer decides whether it is on a window or HUD drawn there or on the world behind, and puts
// its mouse on that; the trigger and B are its left and right button there. [VR HUD CLICKS] A press belongs to what
// it was pressed on until it is let go: sliding off the bar while the trigger is held no longer clicks whatever the
// ray reaches on the layer behind it (on Paul's Quest 3 a window opened from the bar closed again a second later).
const worldPointer = { trig: false, b: false, grip: false, gripY: 0, owner: [null, null, null], ray: { at: 0, o: [0, 0, 0], d: [0, 0, 0], l: '' },
                       lastTrig: 0, lastDir: new THREE.Vector3() };
const lptr = { u: -1, v: -1, buttons: {} };
const _hq = new THREE.Quaternion(), _ho = new THREE.Vector3(), _hd = new THREE.Vector3();
function layerMove(u, v) {
    if (Math.abs(u - lptr.u) < 0.0003 && Math.abs(v - lptr.v) < 0.0003) return;
    lptr.u = u; lptr.v = v;
    send({ t: 'in', e: 'move', u, v, l: 1 });
}
function layerButton(b, down) {
    if (!!lptr.buttons[b] === down) return;
    lptr.buttons[b] = down;
    if (lptr.u < 0) return;
    send({ t: 'in', e: down ? 'down' : 'up', u: lptr.u, v: lptr.v, b, l: 1 });
}
function layerWheel(d) { if (lptr.u >= 0 && d) send({ t: 'in', e: 'wheel', u: lptr.u, v: lptr.v, d, l: 1 }); }
// world ray -> {kind:'bar'|'tab', hit} on the action bar, else {kind:'ptr', layer: {u, v, t} | null}
function aimAt(ro, rd, headPos, headQuat) {
    _hq.copy(headQuat).invert();
    _ho.copy(ro).sub(headPos).applyQuaternion(_hq);
    _hd.copy(rd).applyQuaternion(_hq);
    const b = bar.hit(_ho, _hd);
    if (b) return { kind: b.kind === 'tab' ? 'tab' : 'bar', hit: b, t: b.t };
    // [VR PANELS 2026-11-04] a window's panel first: the HUD band is mostly see-through (only the viewer knows where it
    // has something drawn), so its nearer surface must not take the ray from a window behind it
    const pan = layer.hitPanels(ro, rd);
    return { kind: 'ptr', layer: pan || layer.hit(_ho, _hd) };
}
function sendRay(ro, rd, lay) {
    const r = worldPointer.ray, now = performance.now();
    const o = [+ro.x.toFixed(4), +ro.y.toFixed(4), +ro.z.toFixed(4)], d = [+rd.x.toFixed(4), +rd.y.toFixed(4), +rd.z.toFixed(4)];
    const l = lay ? lay.u.toFixed(4) + ',' + lay.v.toFixed(4) : '';
    const moved = Math.abs(o[0] - r.o[0]) + Math.abs(o[1] - r.o[1]) + Math.abs(o[2] - r.o[2]) > 0.001 ||
                  Math.abs(d[0] - r.d[0]) + Math.abs(d[1] - r.d[1]) + Math.abs(d[2] - r.d[2]) > 0.0008 || l !== r.l;
    if (!moved && now - r.at < 200) return;   // still: every 200 ms (the viewer lets go of a ray it has not heard for 0.5 s)
    r.at = now; r.o = o; r.d = d; r.l = l;
    const m = { t: 'in', e: 'ray', o, d };
    if (lay) { m.lu = +lay.u.toFixed(4); m.lv = +lay.v.toFixed(4); }
    send(m);
}
function ptrButton(b, down) { send({ t: 'in', e: down ? 'down' : 'up', b, w: 1 }); if (down) reticle.pulseUntil = performance.now() + 160; }
function pollWorldPointer(src, gp, frame, headPos, headQuat) {
    const rp = frame.getPose(src.targetRaySpace, renderer.xr.getReferenceSpace());
    const pressed = (i) => !!(gp.buttons[i] && gp.buttons[i].pressed);
    let aim = null;
    if (rp) {
        dolly.updateMatrixWorld();
        _m.fromArray(rp.transform.matrix).premultiply(dolly.matrixWorld);
        _m.decompose(_ro, _rq, _scl);
        _rd.set(0, 0, -1).applyQuaternion(_rq);
        aim = aimAt(_ro, _rd, headPos, headQuat);
        if (aim.layer) { lptr.u = aim.layer.u; lptr.v = aim.layer.v; }   // where the stick scrolls (layerWheel)
        ptrLog.aim = aim.kind !== 'ptr' ? aim.kind : aim.layer ? (aim.layer.k ? 'panel ' + aim.layer.k.split(':')[0] : 'band') +
            ` ${aim.layer.u.toFixed(2)},${aim.layer.v.toFixed(2)}` : 'world';
    }
    const trig = pressed(0), bBtn = pressed(5), grip = pressed(1);
    const own = worldPointer.owner;
    // what the ray is on for the buttons: the owner of a held button, else what it points at now
    const onBar = aim && (aim.kind === 'bar' || aim.kind === 'tab');
    bar.setHover(onBar && !own[0] ? aim.hit : null);
    // the trigger (0) and B (2): press on the bar = activate it there; press elsewhere = the viewer's button
    for (const [b, now, was] of [[0, trig, worldPointer.trig], [2, bBtn, worldPointer.b]]) {
        if (now && !was) {
            const t = performance.now();
            // [VR TELEPORT 2026-11-04] the trigger twice within 0.45 s on the same spot of the world (the ray within
            // 3 degrees): go there, as the desktop's double click does; one press stays a click (touch, sit)
            const again = b === 0 && !onBar && aim && worldAim.k === 'world' && t - worldPointer.lastTrig < 450 &&
                          _rd.angleTo(worldPointer.lastDir) < 0.05;
            if (b === 0) { worldPointer.lastTrig = again ? 0 : t; worldPointer.lastDir.copy(_rd); }
            ptrLog.presses[onBar ? 'bar' : again ? 'tp' : aim ? 'ptr' : 'none']++;   // [VR DEVICE REPORT]
            if (onBar) { own[b] = 'bar'; if (b === 0) bar.activate(aim.hit); }
            else if (again) { own[b] = 'tp'; sendRay(_ro, _rd, aim.layer); send({ t: 'in', e: 'tp' }); reticle.pulseUntil = t + 300; }
            else if (aim) { own[b] = 'ptr'; sendRay(_ro, _rd, aim.layer); ptrButton(b, true); }
        } else if (!now && was) {
            if (own[b] === 'ptr') ptrButton(b, false);
            own[b] = null;
        }
    }
    // off the bar (or holding a press that began off it): the ray goes to the viewer
    if (aim && (!onBar || own[0] === 'ptr' || own[2] === 'ptr') && own[0] !== 'bar') {
        sendRay(_ro, _rd, aim.layer);
        if (grip && !own[0] && (layer.moving || (aim.layer && aim.layer.k && !worldPointer.grip))) {
            // [VR PANELS] grip held on a window's panel: it follows the controller round the arc (and up or down)
            layer.movePanel(aim.layer ? aim.layer.k : null, _ro, _rd, true);
        } else if (grip && aim.layer && !own[0] && !layer.moving) {
            // grip held on the HUD band: move it up or down with the controller (remembered); clicks still work
            if (!worldPointer.grip) worldPointer.gripY = _ro.y;
            else { layer.nudge(_ro.y - worldPointer.gripY, 0); worldPointer.gripY = _ro.y; }
        }
    }
    if (!grip && layer.moving) layer.movePanel(null, _ro, _rd, false);
    // the ray and the dot: on the bar where it hits; else where the viewer says it landed
    if (rp) {
        let t = -1, big = false;
        if (onBar) t = aim.t;
        else if (performance.now() - worldAim.at < 600) {
            if (worldAim.k === 'ui' && aim && aim.layer) t = aim.layer.t;
            else if (worldAim.k === 'world' && worldAim.d > 0) { t = worldAim.d; big = !!RETICLE_WORDS[worldAim.c]; }
        }
        laser.position.copy(_ro); laser.quaternion.copy(_rq); laser.scale.set(1, 1, t > 0 ? t : 4);
        laser.visible = true;
        if (t > 0) placeReticle(_v3.copy(_ro).addScaledVector(_rd, t * 0.998), _ro, big);
        else reticle.group.visible = false;
        if (!onBar && worldAim.k !== 'world') reticle.tag.visible = false;
        else if (!onBar) reticle.tag.visible = !!RETICLE_WORDS[reticle.label];
        else reticle.tag.visible = false;
    }
    worldPointer.trig = trig; worldPointer.b = bBtn; worldPointer.grip = grip;
    return onBar ? 'bar' : aim && aim.layer ? 'layer' : aim ? 'world' : null;
}
const _v3 = new THREE.Vector3();
// Push to talk (vr_hud.js _pollPushToTalk: A/X held; here A on the right, X is jump): speak on press, off on release.
let talkHeld = false;
function pollTalk(gp) {
    const p = !!(gp.buttons[4] && gp.buttons[4].pressed);
    if (p !== talkHeld) {
        talkHeld = p;
        if (p === bar.state.on.has('speak')) return;   // already as wanted (the Speak button latched it)
        send({ t: 'act', a: 'speak' });
    }
}
// Hands, and headsets without controllers (Apple Vision Pro's look-and-pinch, WebXR "transient-pointer"): a pinch is
// a "select" on the session; press what the ray points at.
function onSelect(ev) {
    if (!WORLD() || !ev.frame) return;
    const src = ev.inputSource;
    // [VR HUD CLICKS 2026-11-04] Controllers are polled every frame (pollWorldPointer). The browser ALSO fires "select"
    // when the trigger is let go; taken as a second press it toggled what the trigger had just opened. So a select
    // counts only from an input with no gamepad (hands, look-and-pinch) and only while no controller is in use.
    if (src.gamepad) return;
    for (const other of ev.frame.session.inputSources) if (other.gamepad && other.targetRayMode === 'tracked-pointer') return;
    const rp = ev.frame.getPose(src.targetRaySpace, renderer.xr.getReferenceSpace());
    const vp = ev.frame.getViewerPose(renderer.xr.getReferenceSpace());
    if (!rp || !vp) return;
    dolly.updateMatrixWorld();
    _m.fromArray(rp.transform.matrix).premultiply(dolly.matrixWorld);
    _m.decompose(_ro, _rq, _scl);
    _rd.set(0, 0, -1).applyQuaternion(_rq);
    const hp = new THREE.Vector3(), hq = new THREE.Quaternion(), hs = new THREE.Vector3();
    new THREE.Matrix4().fromArray(vp.transform.matrix).premultiply(dolly.matrixWorld).decompose(hp, hq, hs);
    const aim = aimAt(_ro, _rd, hp, hq);
    ptrLog.presses.select++;   // [VR DEVICE REPORT]
    if (aim.kind === 'bar' || aim.kind === 'tab') { bar.activate(aim.hit); return; }
    // anywhere else: a click there (the viewer finds what is under it: a window, a HUD, or the world)
    sendRay(_ro, _rd, aim.layer);
    ptrButton(0, true);
    ptrButton(0, false);
}

function pollControllers(session, frame, headPos, headQuat) {
    const ctl = [];
    let lift = 0;
    if (BIG() && !bigPlaced) recentre(headPos, headQuat);
    if (!BIG()) { laser.visible = dot.visible = false; reticle.group.visible = false; }
    // [VR HUD CLICKS 2026-11-04] one source per hand: a controller (a gamepad on a tracked pointer) before anything
    // else the browser lists for that hand (hands with "hand-tracking" can be listed beside the controllers)
    const seen = {};
    const sources = Array.from(session.inputSources).sort((a, b) => (b.gamepad ? 1 : 0) - (a.gamepad ? 1 : 0));
    const NO_PAD = { buttons: [], axes: [] };
    for (const src of sources) {
        const gp = src.gamepad;
        if (!gp) {
            // [VR HANDS 2026-11-04] a hand (Quest hand tracking) or a look-and-pinch pointer: no buttons to poll (a pinch
            // is the session's "select", onSelect), but its ray still aims - the laser, the dot, the viewer's hover -
            // exactly as the right controller's does, so pointing with a hand shows where a pinch will land
            const hand = src.handedness || 'none';
            if (WORLD() && src.targetRayMode === 'tracked-pointer' && hand !== 'left' && !seen.right && !seen[hand]) {
                seen[hand] = true;
                pollWorldPointer(src, NO_PAD, frame, headPos, headQuat);
            }
            continue;
        }
        const hand = src.handedness || 'none';
        if (seen[hand]) continue;
        seen[hand] = true;
        if (hand === 'left') leftProfiles = (src.profiles || []).join(' ');
        let bits = 0;
        gp.buttons.forEach((b, i) => { if (b && b.pressed && i < 31) bits |= (1 << i); });
        ctl.push({ h: hand, b: bits, a: Array.from(gp.axes, (x) => Math.round(x * 1000) / 1000) });
        if (!config || config.mode === 'test') {
            // the stage-1 test pattern: snap turn the page's own view, menu = numbers (as before)
            if (hand === 'right') {
                const sx = axis(gp, 2, 0);
                if (Math.abs(sx) > 0.6 && !snapArmed) { snapArmed = true; rigYaw += -Math.sign(sx) * 45 * Math.PI / 180; dolly.quaternion.setFromAxisAngle(new THREE.Vector3(0, 1, 0), rigYaw); }
                else if (Math.abs(sx) < 0.3) snapArmed = false;
            }
            for (let i = 6; i < gp.buttons.length; i++) {
                const key = hand + ':' + i, p = !!(gp.buttons[i] && gp.buttons[i].pressed);
                if (p && !menuWas[key]) statsOn = !statsOn;
                menuWas[key] = p;
            }
            continue;
        }
        if (hand === 'left') lift = lift || pollLeft(gp, headPos, headQuat);
        else if (hand === 'right') {
            const aimed = BIG() ? pollPointer(src, gp, frame) : WORLD() ? pollWorldPointer(src, gp, frame, headPos, headQuat) : false;
            pollTalk(gp);
            // [VR WORLD POINTER] the stick scrolls only over the bar or a window / HUD the viewer says the ray is on;
            // pointing at the world (or an empty part of the layer) it turns and flies as before
            const scrollable = aimed === 'bar' || (aimed === 'layer' && worldAim.k === 'ui') ? aimed : (BIG() ? aimed : null);
            lift = lift || pollRightStick(gp, scrollable);
        }
    }
    if (config && config.mode !== 'test') sendLift(lift);
    return ctl;
}

// ------------------------------------------------------------------------------------------------ frame loop
const _m = new THREE.Matrix4(), _pos = new THREE.Vector3(), _quat = new THREE.Quaternion(), _scl = new THREE.Vector3();
const _fwd = new THREE.Vector3();
let xrErrors = 0;
const pred = { t: 0, q: new THREE.Quaternion(), p: new THREE.Vector3(), w: new THREE.Vector3(), v: new THREE.Vector3(),
               dq: new THREE.Quaternion(), out: { p: new THREE.Vector3(), q: new THREE.Quaternion() }, ax: new THREE.Vector3() };
function predictPose(now, pos, quat) {
    const o = pred.out;
    o.p.copy(pos); o.q.copy(quat);
    if (!renderer.xr.isPresenting) { pred.t = 0; return o; }
    if (pred.t) {
        const dt = (now - pred.t) / 1000;
        if (dt > 0.002 && dt < 0.1) {
            // angular velocity from this frame's turn (q * prev^-1 as axis-angle), and velocity; smoothed
            pred.dq.copy(quat).multiply(pred.q.clone().invert());
            if (pred.dq.w < 0) { pred.dq.x = -pred.dq.x; pred.dq.y = -pred.dq.y; pred.dq.z = -pred.dq.z; pred.dq.w = -pred.dq.w; }
            const ang = 2 * Math.acos(Math.min(1, pred.dq.w)), sn = Math.sqrt(Math.max(0, 1 - pred.dq.w * pred.dq.w));
            pred.ax.set(pred.dq.x, pred.dq.y, pred.dq.z).multiplyScalar(sn > 1e-6 ? ang / sn / dt : 0);
            pred.w.lerp(pred.ax, 0.5);
            pred.v.lerp(pred.ax.copy(pos).sub(pred.p).divideScalar(dt), 0.5);
        }
    }
    pred.t = now; pred.q.copy(quat); pred.p.copy(pos);
    const lat = num.p2p.avg();
    const h = Number.isFinite(lat) ? Math.min(80, Math.max(0, lat * 0.8)) / 1000 : 0;
    if (h > 0) {
        const a = pred.w.length() * h;
        if (a > 1e-5) { pred.dq.setFromAxisAngle(pred.ax.copy(pred.w).normalize(), Math.min(a, 0.6)); o.q.premultiply(pred.dq); }
        o.p.addScaledVector(pred.v, h);
    }
    return o;
}
let testFreeze = false, testCards = false;   // test hooks only (e2e_parallax.mjs): hold the picture; the old flat cards

function animate(time, frame) {
    const now = performance.now();
    let ctl = [];
    let eyes = null;
    // Head pose in world space (dolly * reference-space pose).
    if (frame && renderer.xr.isPresenting) {
        const pose = frame.getViewerPose(renderer.xr.getReferenceSpace());
        if (pose) {
            eyes = eyesOf(pose);
            dolly.updateMatrixWorld();
            _m.fromArray(pose.transform.matrix).premultiply(dolly.matrixWorld);
            _m.decompose(_pos, _quat, _scl);
        }
        dock.position.copy(_pos); dock.quaternion.copy(_quat); dock.updateMatrixWorld(true);
        bar.setTabActive(WORLD());
        ctl = pollControllers(renderer.xr.getSession(), frame, _pos, _quat);
        num.xrFrames++;
    } else {
        _quat.setFromEuler(new THREE.Euler(-dragPitch, -dragYaw, 0, 'YXZ'));
        _pos.set(0, 1.6, 0);
        camera.quaternion.copy(_quat);
        dock.position.copy(_pos); dock.quaternion.copy(_quat); dock.updateMatrixWorld(true);
        num.xrFrames++;
    }
    // [VR PREDICT 2026-11-04] the pose the viewer draws for is where the head WILL be when that picture is shown here:
    // the head's own turning and moving rates (smoothed) carried forward by most of the measured head-pose-to-shown
    // time, at most 80 ms - as Oculus Link's runtime predicts over its measured end-to-end time ("How does Oculus Link
    // work", Meta). The depth meshes then only have to make up the small rest.
    const pp = predictPose(now, _pos, _quat);
    const pm = { t: 'pose', s: ++poseSeq, ct: now, p: [pp.p.x, pp.p.y, pp.p.z], q: [pp.q.x, pp.q.y, pp.q.z, pp.q.w], c: ctl };
    if (eyes) { pm.e = eyes; pm.f = eyeFov; if (eyeTan) pm.pt = eyeTan; }
    send(pm);

    // Newest picture onto the texture.
    let fresh = null;
    if (latest && !testFreeze) {
        fresh = latest;
        latest = null;
        const mat = BIG() ? bigMat : screenMat;
        if (fresh.hdr.codec === 'h264') { videoTex.setFrame(fresh.img); if (mat.map !== videoTex) { mat.map = videoTex; mat.needsUpdate = true; } }
        else { bitmapTex.image = fresh.img; bitmapTex.needsUpdate = true; if (mat.map !== bitmapTex) { mat.map = bitmapTex; mat.needsUpdate = true; } }
        if (BIG()) { bigMesh.visible = true; screen.visible = false; eyeScreens[0].visible = eyeScreens[1].visible = false; }
        else {
            screen.userData.hdr = fresh.hdr;
            screen.visible = !fresh.hdr.stereo;
            eyeScreens[0].visible = eyeScreens[1].visible = fresh.hdr.stereo;
            bigMesh.visible = false;
            // [VR DEPTH] the eyes as world-locked meshes when the picture has its depth (else the cards as before)
            if ((fresh.hdr.stereo || fresh.hdr.centre) && fresh.depth && !testCards) depthEyes.show(fresh.hdr, fresh.depth, mat.map, eyeLayer);
            else depthEyes.hide();
        }
    }
    if (!renderer.xr.isPresenting) { laser.visible = false; }
    // Reprojection: put the picture where it was rendered for, around where the head is NOW.
    const hdr = screen.userData.hdr;
    if (hdr && !BIG()) {
        const size = 2 * PICTURE_DISTANCE * Math.tan((hdr.fov * Math.PI / 180) / 2);
        _fwd.set(0, 0, -PICTURE_DISTANCE).applyQuaternion(hdr.q);
        if (!hdr.stereo && hdr.centre && hdr.tan) {
            // [VR CENTRE] the centre view's card: its own window, the far shell behind the mesh (or 10 m without depth)
            const D = depthEyes.active ? SHELL_DISTANCE : PICTURE_DISTANCE, w = hdr.tan;
            screen.scale.set(D * (w[1] - w[0]), D * (w[3] - w[2]), 1);
            screen.quaternion.copy(hdr.q);
            _ev.set(D * (w[0] + w[1]) / 2, D * (w[2] + w[3]) / 2, -D).applyQuaternion(hdr.q);
            screen.position.copy(_pos).add(_ev);
            screen.renderOrder = -10;
        } else if (!hdr.stereo) {
            screen.scale.set(size, size, 1);
            screen.quaternion.copy(hdr.q);
            screen.position.copy(_pos).add(_fwd);
        } else {
            // [VR STEREO] each half from the eye it was drawn for: that eye NOW (head position + its offset turned
            // with the head now), looking where the head looked when the picture was drawn
            for (let i = 0; i < 2; i++) {
                const m = eyeScreens[i];
                m.scale.set(size, size, 1);
                m.quaternion.copy(hdr.q);
                _ev.set(hdr.eyes[3 * i], hdr.eyes[3 * i + 1], hdr.eyes[3 * i + 2]).applyQuaternion(_quat);
                m.position.copy(_pos).add(_ev).add(_fwd);
            }
            // [VR 3D FIX 2026-11-04] each eye's own off-axis window: the half covers exactly the frustum it was drawn
            // for (tangents L, R, B, T), PICTURE_DISTANCE in front of that eye, along the head's direction then
            if (hdr.tan) {
                // [VR DEPTH] with the world-locked meshes in front, the cards are their far shell
                const D = depthEyes.active ? SHELL_DISTANCE : PICTURE_DISTANCE;
                for (let i = 0; i < 2; i++) {
                    const w = hdr.tan.slice(4 * i, 4 * i + 4), m = eyeScreens[i];
                    m.scale.set(D * (w[1] - w[0]), D * (w[3] - w[2]), 1);
                    _ev.set(D * (w[0] + w[1]) / 2, D * (w[2] + w[3]) / 2, -D).applyQuaternion(hdr.q);
                    const eo = new THREE.Vector3(hdr.eyes[3 * i], hdr.eyes[3 * i + 1], hdr.eyes[3 * i + 2]).applyQuaternion(_quat);
                    m.position.copy(_pos).add(eo).add(_ev);
                }
            }
            // each half seen only by its eye (eyeLayer, from XRView.eye); on the page (no headset) the camera sees
            // layer 0 only: show the left eye there
            eyeScreens[0].layers.set(renderer.xr.isPresenting ? eyeLayer[0] : 0);
            eyeScreens[1].layers.set(eyeLayer[1]);
            depthEyes.pageView(renderer.xr.isPresenting, eyeLayer);
        }
    }

    renderer.render(scene, camera);

    if (fresh) {
        const drawn = performance.now();
        if (fresh.hdr.poseClientMs >= 0) num.p2p.add(drawn - fresh.hdr.poseClientMs);
        if (Number.isFinite(clockOff)) {
            num.v2p.add(drawn + clockOff - fresh.hdr.captureMs);
            vrAudio.noteVideoDelay(drawn + clockOff - fresh.hdr.captureMs);   // [VR AUDIO] sound and picture together
        }
        num.decode.add(fresh.decodeMs);
        noteStages(fresh.hdr, drawn);   // [VR STAGES]
        num.newFrames++;
        if (shown) shown.img.close();   // the previous picture has been replaced on the GPU
        shown = fresh;
    }

    if (now - num.lastT >= 1000) {
        const dt = (now - num.lastT) / 1000;
        num.fps = num.xrFrames / dt;
        num.newFps = num.newFrames / dt;
        num.xrFrames = num.newFrames = 0;
        num.lastT = now;
        num.gotFps = num.got / dt;
        num.got = 0;
        num.decodedFps = num.decoded / dt;
        num.decoded = 0;
        if (config && config.debug) {
            send({ t: 'stats', fps: num.fps, newFps: num.newFps, decodeMs: num.decode.avg(), p2p: num.p2p.avg(),
                   p2p95: num.p2p.pct(0.95), v2p: num.v2p.avg() });
        }
        // [VR QUALITY 2026-11-04] ONE short line for the viewer's log every 30 s while in VR (Paul: no stats in hot
        // paths): what this headset really gets, so the next test shows its numbers
        if (renderer.xr.isPresenting && ++num.summaryTick >= 30) {
            num.summaryTick = 0;
            const eye = config ? (config.w === 2 * config.h ? config.h : config.w) : 0;
            send({ t: 'summary', s: deviceLine() });   // [VR DEVICE REPORT]
            send({ t: 'summary', s: `${HEADSETS[headsetKind()].label}: display ${fmt(num.fps, 0)} fps, pictures in ${fmt(num.gotFps, 0)}/s decoded ${fmt(num.decodedFps, 0)}/s, ` +
                `head-pose-to-shown ${fmt(num.p2p.avg(), 0)} ms (p95 ${fmt(num.p2p.pct(0.95), 0)}), PC-to-shown ${fmt(num.v2p.avg(), 0)} ms, decode ${fmt(num.decode.avg(), 1)} ms, ` +
                `eye ${eye} px, step ${quality.level} (top ${quality.top}, down ${quality.downs || 0} times, last because ${quality.lastWhy || '-'}), ` +
                `HUDs ${layer.on ? layer.frames + ' pictures' + (layer.errors ? `, ${layer.errors} failed` : '') : 'off'}; ` +
                `stages: ${stagesLine()}` });
        }
        adaptQuality(now);
    }
    // [VR CONTROLS / HUD LAYER] the docked surfaces (3D Mode in the headset only)
    layer.setActive(WORLD() && renderer.xr.isPresenting);
    layer.update(_pos, _quat);
    bar.setPageState('numbers', statsOn);
    bar.update(now);
    if (renderer.xr.isPresenting) statsMesh.visible = statsOn || now < sizeFlashUntil;
    if (config && config.debug) drawStatsPanel(now);
}

// WolfStorm render_manager.js _xrAnimateGuarded: an uncaught throw in the XR frame callback is permanently fatal
// (three re-arms the next frame AFTER the callback), so keep the loop alive and tell the viewer once.
renderer.setAnimationLoop((time, frame) => {
    try { animate(time, frame); }
    catch (e) {
        if (++xrErrors === 1) {
            console.error('[WolfViewer VR] frame callback threw (loop kept alive):', e);
            send({ t: 'log', msg: 'frame callback threw: ' + String(e && e.message || e), stack: String(e && e.stack || '').slice(0, 600) });
            showError('Something went wrong drawing the picture: ' + String(e && e.message || e));
        }
    }
});

// ------------------------------------------------------------------------------------------------ typing
// [VR BIG SCREEN] Text typed in the box (on the page, or with the headset's own keyboard in VR) goes to WolfViewer as
// typed characters; Enter, Backspace, Tab, Escape, the arrows and Ctrl/Alt shortcuts go as keys. The box keeps one
// space in it so a Backspace on an empty box is still seen (Android-style keyboards send it as an input event).
const SPECIAL = new Set(['Enter', 'Backspace', 'Tab', 'Escape', 'ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown',
    'Delete', 'Home', 'End', 'PageUp', 'PageDown', 'Insert']);
const typeBox = $('wvr-type');
function mods(e) { return (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0); }
function tapKey(k, m = 0) { send({ t: 'in', e: 'kd', k, m }); send({ t: 'in', e: 'ku', k, m }); }
function resetBox() { typeBox.value = ' '; typeBox.setSelectionRange(1, 1); }
typeBox.addEventListener('focus', resetBox);
typeBox.addEventListener('keydown', (e) => {
    if (SPECIAL.has(e.key) || ((e.ctrlKey || e.altKey) && e.key.length === 1)) {
        e.preventDefault();
        send({ t: 'in', e: 'kd', k: e.key, m: mods(e) });
    }
});
typeBox.addEventListener('keyup', (e) => {
    if (SPECIAL.has(e.key) || ((e.ctrlKey || e.altKey) && e.key.length === 1)) send({ t: 'in', e: 'ku', k: e.key, m: mods(e) });
});
typeBox.addEventListener('input', (e) => {
    if (e.inputType === 'insertText' || e.inputType === 'insertReplacementText' || e.inputType === 'insertCompositionText') {
        if (e.data) send({ t: 'in', e: 'text', s: e.data.slice(0, 64) });
    } else if (e.inputType === 'insertLineBreak') {
        tapKey('Enter');
    } else if (e.inputType === 'deleteContentBackward') {
        tapKey('Backspace');
    }
    resetBox();
});
function openKeyboard() {
    // The Quest Browser shows its system keyboard over an immersive session when a text box takes focus.
    typeBox.focus({ preventScroll: true });
    toast('Type on the keyboard; it goes to WolfViewer.');
}
$('wvr-bigger').addEventListener('click', () => setScreenSize(screenSizeIndex + 1));
$('wvr-smaller').addEventListener('click', () => setScreenSize(screenSizeIndex - 1));

window.__wolfvr = { resetStages: () => { for (const k of STAGES) stages[k].v.length = 0; },
    rates: () => ({ display: num.fps, gotFps: num.gotFps, decodedFps: num.decodedFps, newFps: num.newFps, p2p: num.p2p.avg(), v2p: num.v2p.avg() }),
    stages: () => Object.fromEntries(STAGES.map((k) => [k, { p50: stages[k].pct(0.5), p95: stages[k].pct(0.95), n: stages[k].v.length }])),
    numbers: () => ({ rows: numberRows(), p2p: num.p2p.v.slice(-20), v2p: num.v2p.v.slice(-20) }), toggleVR, config: () => config,
    big: () => ({ on: BIG(), size: SCREEN_SIZES[screenSizeIndex], visible: bigMesh.visible, ptr: { u: ptr.u, v: ptr.v } }),
    // test hooks (scripts/wolfvr/e2e_viewer.mjs): the picture as received, and a click at a point of it
    grab: () => {
        if (!shown || !config) return null;
        const c = document.createElement('canvas');
        c.width = config.w; c.height = config.h;
        c.getContext('2d').drawImage(shown.img, 0, 0, config.w, config.h);
        return c.toDataURL('image/jpeg', 0.85);
    },
    clickAt: (u, v, b = 0) => { pointerMove(u, v); pointerButton(b, true); pointerButton(b, false); },
    sendText: (t) => send({ t: 'in', e: 'text', s: String(t).slice(0, 64) }),
    sendKey: (k, m = 0) => tapKey(k, m),
    // [VR CONTROLS / HUD LAYER] test hooks (scripts/wolfvr/e2e_controls.mjs)
    bar: () => ({ open: bar.open, ids: bar.slots.map((x) => x.action.id), on: [...bar.state.on], off: [...bar.state.off],
                  sit: bar.state.sit, hide: bar.state.hide, region: bar.state.region, fav: bar.state.fav.length, hover: bar.hover }),
    layer: () => ({ want: layer.want, on: layer.on, frames: layer.frames, cfg: layer.cfg, visible: layer.mesh.visible }),
    // [VR PANELS] the window panels: name, window rectangle, where it stands (yaw degrees, height), shown
    panels: () => [...layer.panels.values()].map((p) => ({ k: p.k, n: p.n, r: p.r, yaw: +(p.yaw * 180 / Math.PI).toFixed(1),
        y: +p.c.y.toFixed(3), arc: +(p.arc * 180 / Math.PI).toFixed(1), visible: p.mesh.visible })),
    // [VR PANELS] where a point of the viewer's window (u, v from its top-left) is in the headset: on its window's
    // panel when it is inside one, else on the HUD band (test hook)
    windowPoint: (u, v) => {
        for (const p of layer.panels.values()) {
            const [x0, y0, x1, y1] = p.r;
            if (p.mesh.visible && u >= x0 && u <= x1 && v >= y0 && v <= y1) return window.__wolfvr.panelPoint(p.k, (u - x0) / (x1 - x0), (v - y0) / (y1 - y0));
        }
        return surfacePoint(1.25, layer.arc, layer.y, layer.height, u, 1 - v);
    },
    // the world point at (u, v) of a panel (0..1 across it, from its top-left), for aiming the fake controller
    panelPoint: (k, u, v) => {
        const p = layer.panels.get(k);
        if (!p) return null;
        const a = -p.arc / 2 + u * p.arc - p.yaw;
        return [p.c.x + 1.3 * Math.sin(a), p.c.y + (0.5 - v) * p.h, p.c.z - 1.3 * Math.cos(a)];
    },
    quality: () => ({ level: quality.level, headset: headsetKind(), auto: headsetAuto() }),
    audio: () => ({ buffered: vrAudio.bufferedMs, target: vrAudio.targetMs, underruns: vrAudio.underruns, gaps: vrAudio.gaps,
                    delay: vrAudio.soundDelayMs, skew: vrAudio.skewMs, mic: vrAudio.micWanted, blocks: vrAudio.blocks,
                    started: vrAudio.started, ctx: vrAudio.ctx ? vrAudio.ctx.state : 'none', on: vrAudio.on,
                    error: $('wvr-error').textContent }),
    // the world point of a bar slot's centre (or a layer point u, v from the top-left), for aiming the fake controller
    slotPoint: (id) => {
        const i = bar.slots.findIndex((x) => x.action.id === id);
        if (i < 0) return null;
        const sl = bar.slots[i];
        const u = (sl.x + sl.w / 2) / 1024, v = 1 - (sl.y + sl.h / 2) / 448;
        return surfacePoint(1.0, bar.arc, -0.385, bar.height, u, v);
    },
    layerPoint: (u, v) => surfacePoint(1.25, layer.arc, layer.y, layer.height, u, 1 - v),
    // [VR PANELS] the HUD band's own hit for a ray from the head through its point (u, v) (null where a panel was cut out)
    bandHit: (u, v) => {
        const a = -layer.arc / 2 + u * layer.arc, o = new THREE.Vector3(0, 0, 0);
        const d = new THREE.Vector3(1.25 * Math.sin(a), layer.y + (0.5 - v) * layer.height, -1.25 * Math.cos(a)).normalize();
        return layer.hit(o, d);
    },
    // [VR MENU FIX] the bar's Close button, and what the Menu tab says now
    closePoint: () => surfacePoint(1.0, bar.arc, -0.385, bar.height, (1024 - 196 + 91) / 1024, 1 - (10 + 24) / 448),
    setQuality: (l) => { quality.pinned = true; setQuality(l, ''); },   // held there: no adapting while a test measures it   // test hook: a headset's quality step
    deviceLine: () => deviceLine(),
    tabText: () => (performance.now() < bar.tabMsgUntil ? bar.tabMsg : 'Menu'),
    // [VR 3D FIX / WORLD POINTER] test hooks: which view is which eye and the layer each half is on (and each XR
    // camera's layer mask), the eye windows sent, the last picture's header, where the viewer said the ray landed
    eyes: () => {
        const xc = renderer.xr.isPresenting ? renderer.xr.getCamera().cameras : [];
        const views = lastViews ? Array.from(lastViews, (v) => ({ eye: v.eye, proj: Array.from(v.projectionMatrix, (x) => +x.toFixed(5)) })) : null;
        return { views, eyeLayer: eyeLayer.slice(), halfLayers: eyeScreens.map((m) => m.layers.mask),
                 cameraMasks: xc.map((c) => c.layers.mask), tan: eyeTan, fov: eyeFov,
                 hdr: screen.userData.hdr ? { stereo: screen.userData.hdr.stereo, tan: screen.userData.hdr.tan, eyes: screen.userData.hdr.eyes } : null,
                 visible: eyeScreens.map((m) => m.visible), mono: screen.visible };
    },
    xr: () => ({ inVR: inVR(), said: xrSaid, pictures: num.gotTotal }),   // [VR IMMERSIVE] test hook
    freeze: (on) => { testFreeze = !!on; }, cards: (on) => { testCards = !!on; if (on) depthEyes.hide(); },
    // test hook (e2e_parallax.mjs): what the LEFT eye sees with the head moved dx metres sideways, as a PNG - the scene
    // drawn off-screen by a camera with the left XRView's projection and pose (plus dx), its own layers
    renderEye: (dx) => {
        if (!lastViews) return null;
        const li = Math.max(0, lastViews.findIndex((v) => v.eye === 'left'));
        const v = lastViews[li];
        const cam = new THREE.PerspectiveCamera();
        cam.projectionMatrix.fromArray(v.projectionMatrix);
        cam.projectionMatrixInverse.copy(cam.projectionMatrix).invert();
        new THREE.Matrix4().fromArray(v.transform.matrix).decompose(cam.position, cam.quaternion, cam.scale);
        cam.position.x += dx;
        cam.updateMatrixWorld(true);
        cam.layers.set(0); cam.layers.enable(eyeLayer[0]);
        const W = 480, H = 432, rt = new THREE.WebGLRenderTarget(W, H, { colorSpace: THREE.SRGBColorSpace });
        const xr = renderer.xr.enabled; renderer.xr.enabled = false;
        renderer.setRenderTarget(rt); renderer.render(scene, cam); renderer.setRenderTarget(null);
        renderer.xr.enabled = xr;
        const px = new Uint8Array(W * H * 4);
        renderer.readRenderTargetPixels(rt, 0, 0, W, H, px);
        rt.dispose();
        const c = document.createElement('canvas'); c.width = W; c.height = H;
        const img = c.getContext('2d').createImageData(W, H);
        for (let y = 0; y < H; y++) img.data.set(px.subarray((H - 1 - y) * W * 4, (H - y) * W * 4), y * W * 4);   // GL rows are bottom first
        c.getContext('2d').putImageData(img, 0, 0);
        return c.toDataURL('image/png');
    },
    aim: () => ({ ...worldAim, reticle: reticle.group.visible, tag: reticle.tag.visible, laser: laser.visible, owner: worldPointer.owner.slice() }),
    depth: () => ({ active: depthEyes.active, frames: depthEyes.frames, grid: [depthEyes.gw, depthEyes.gh],
                    hdr: screen.userData.hdr ? { pos: screen.userData.hdr.pos, dw: screen.userData.hdr.dw, dh: screen.userData.hdr.dh,
                                                 near: screen.userData.hdr.near, far: screen.userData.hdr.far } : null,
                    // metres at a few grid points of the left eye (centre, bottom middle, top middle), for the test
                    sample: depthEyes.active ? [[0.5, 0.5], [0.5, 0.95], [0.5, 0.05]].map(([u, v]) =>
                        +depthEyes.data[Math.min(depthEyes.gh - 1, Math.floor(v * depthEyes.gh)) * depthEyes.gw * depthEyes.halves + Math.floor(u * depthEyes.gw)].toFixed(3)) : null }),
    surfaces: () => ({ big: bigMesh.visible, eyes: eyeScreens.map((m) => m.visible), mono: screen.visible, layer: layer.mesh.visible,
                       bar: bar.mesh.visible, tab: bar.tab.visible, reticle: reticle.group.visible, check: checkCard.visible }),
    tabPoint: () => surfacePoint(1.0, bar.tabArc, -0.52, bar.tabH, 0.5, 0.5),
    grabLayer: () => {
        if (!layer.shown || !layer.cfg) return null;
        const c = document.createElement('canvas');
        c.width = layer.cfg.w; c.height = layer.cfg.h * 2;
        c.getContext('2d').drawImage(layer.shown, 0, 0, c.width, c.height);
        return c.toDataURL('image/png');
    } };
// head-space point on a docked cylinder section (vrbar.js curvedGeometry: u from the left, v from the bottom) -> world
function surfacePoint(R, arc, cy, h, u, v) {
    const a = -arc / 2 + u * arc;
    const p = new THREE.Vector3(R * Math.sin(a), cy - h / 2 + v * h, -R * Math.cos(a)).applyQuaternion(_quat).add(_pos);
    return [p.x, p.y, p.z];
}
connect();
