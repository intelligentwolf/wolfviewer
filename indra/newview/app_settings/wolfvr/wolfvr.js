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
const HEADER_SIZE = 72;           // wolfvrpipe.cpp kHeaderSize
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
    setTimeout(() => t.remove(), 6000);
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
    fps: NaN, newFps: NaN, viewer: {}, rtt: NaN,
};

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

function send(obj) {
    if (ws && ws.readyState === WebSocket.OPEN && ws.bufferedAmount < 65536) ws.send(JSON.stringify(obj));
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
        showWaiting(false);
        if (ev.code === 4000) {
            showError('Another headset or browser tab is now connected to WolfViewer, so this one stopped. Reload this page to take it back.');
            setStatus('Not connected.');
            return;
        }
        if (ev.code === 4003) {
            showError('Your PC said no to this headset. To try again, switch VR off and on again in WolfViewer, then reload this page.');
            setStatus('Not allowed.');
            return;
        }
        if (ev.code === 4029) {
            showError('Too many headsets are asking your PC at once. Wait a moment, then reload this page.');
            setStatus('Not connected.');
            return;
        }
        if (approvedReconnect) { approvedReconnect = false; connect(); return; }
        setStatus(ev.code === 1001 ? 'WolfViewer stopped the headset stream. Trying again every few seconds…'
                  : ev.code === 4001 ? 'Your PC forgot its headsets. Connecting again…'
                                     : 'Lost the connection to WolfViewer. Trying again every few seconds…');
        retryTimer = setTimeout(connect, 3000);
    };
}

let approvedReconnect = false;
function showWaiting(on) {
    $('wvr-waiting').hidden = !on;
}

// The PC allowed this headset: swap the one-time ticket for the cookie, then connect again with it.
async function claim(ticket) {
    try {
        const r = await fetch('/claim?c=' + encodeURIComponent(ticket), { cache: 'no-store', credentials: 'same-origin' });
        if (r.status !== 204) throw new Error('the PC answered ' + r.status);
        approvedReconnect = true;
        setStatus('Allowed. Connecting…');
        showWaiting(false);
        ws.close(1000);
    } catch (e) {
        showError('Your PC allowed this headset, but saving that failed (' + (e && e.message ? e.message : e) + '). Reload this page to try again.');
    }
}

// The PC knows this headset (the socket said "welcome"): start the picture and the clock sync.
function onAllowed() {
    showWaiting(false);
    setStatus('Connected to WolfViewer. Waiting for the picture…');
    send({ t: 'hello', webcodecs: typeof VideoDecoder !== 'undefined', ua: navigator.userAgent });
    pings.length = 0;
    let n = 0;
    clearInterval(pingTimer);
    pingTimer = setInterval(() => { send({ t: 'ping', ct: performance.now() }); if (++n === 10) { clearInterval(pingTimer); pingTimer = setInterval(() => send({ t: 'ping', ct: performance.now() }), 2000); } }, 250);
}

function onText(text) {
    let m;
    try { m = JSON.parse(text); } catch (e) { return; }
    if (m.t === 'waiting') {
        setStatus('Found your PC.');
        showWaiting(true);
        return;
    }
    if (m.t === 'approved') { claim(m.claim); return; }
    if (m.t === 'denied') return;   // the close (4003) that follows says it
    if (m.t === 'welcome') { onAllowed(); return; }
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
        setupDecoder(m);
        $('wvr-big-controls').hidden = !BIG();
        $('wvr-preview-hint').textContent = BIG()
            ? 'This is your WolfViewer window. Click, right-click and scroll on it here; in VR, point with the right controller.'
            : 'Drag the picture to turn your head here on the page; in the headset, just look around.';
        if (BIG()) { buildBigScreen(); bigPlaced = false; statsOn = false; }
        else { bigMesh.visible = false; laser.visible = dot.visible = false; }
        releaseKeys();
        setStatus(`Connected. Picture ${m.w}×${m.h}, ${m.codec === 'h264' ? 'H.264 video' : 'JPEG pictures'} from ${m.encoder}.`);
    } else if (m.t === 'stats') {
        num.viewer = m;
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
            // This browser cannot decode the stream: ask for JPEG pictures instead.
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
    if (buf.byteLength < HEADER_SIZE || d.getUint32(0, true) !== MAGIC) return null;
    return {
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
    };
}

function offerPicture(img, hdr, decodeMs) {
    // Newest wins: a picture that was never drawn is dropped for a newer one.
    if (latest) latest.img.close();
    latest = { img, hdr, decodeMs };
}

function onPicture(buf) {
    const hdr = parseHeader(buf);
    if (!hdr) return;
    const payload = new Uint8Array(buf, HEADER_SIZE, hdr.bytes);
    if (hdr.codec === 'jpeg') {
        const t0 = performance.now();
        // WebGL ignores UNPACK_FLIP_Y for ImageBitmap, so flip while decoding (texture.flipY = false below).
        createImageBitmap(new Blob([payload], { type: 'image/jpeg' }), { imageOrientation: 'flipY' })
            .then((bmp) => offerPicture(bmp, hdr, performance.now() - t0))
            .catch(() => { /* a bad picture is skipped */ });
        return;
    }
    if (!decoder || decoder.state !== 'configured') return;
    if (waitingKey) {
        if (!hdr.key) { askKey(false); return; }
        waitingKey = false;
    }
    if (decoder.decodeQueueSize > 4) { waitingKey = true; askKey(false); return; }   // decoder far behind: restart clean
    pendingMeta.set(hdr.frame, { hdr, t0: performance.now() });
    decoder.decode(new EncodedVideoChunk({ type: hdr.key ? 'key' : 'delta', timestamp: hdr.frame, data: payload }));
}

function onDecoded(vf) {
    const meta = pendingMeta.get(vf.timestamp);
    pendingMeta.delete(vf.timestamp);
    if (!meta) { vf.close(); return; }
    offerPicture(vf, meta.hdr, performance.now() - meta.t0);
}

// ------------------------------------------------------------------------------------------------ scene
const wrap = $('wvr-canvas-wrap');
const renderer = new THREE.WebGLRenderer({ antialias: false, alpha: false });
renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
renderer.outputColorSpace = THREE.SRGBColorSpace;
wrap.appendChild(renderer.domElement);
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x000000);
const dolly = new THREE.Group();            // the XR reference-space origin (threejs-webxr-contract: view = camera.parent)
scene.add(dolly);
const camera = new THREE.PerspectiveCamera(100, 1, 0.05, 100);
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
let statsOn = false, statsLastDraw = 0;   // numbers panel: off until asked for (left menu held 1 s)

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
    ].concat(BIG() ? [['screen', `${SCREEN_SIZES[screenSizeIndex].toFixed(1)} m wide`, 'left stick up/down']] : []);
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
            await renderer.xr.setSession(session);
            // As WolfStorm's updateTargetFrameRate: the supported display rate nearest the viewer's frame rate.
            const want = (config && config.fps) || 72;
            if (session.supportedFrameRates && session.updateTargetFrameRate) {
                const rates = Array.from(session.supportedFrameRates);
                if (rates.length) {
                    const best = rates.reduce((a, b) => (Math.abs(b - want) < Math.abs(a - want) ? b : a));
                    session.updateTargetFrameRate(best).catch(() => {});
                }
            }
            statsMesh.visible = statsOn;
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

function pollLeft(gp, headPos, headQuat) {
    const pressed = (i) => !!(gp.buttons[i] && gp.buttons[i].pressed);
    const edge = (i) => { const p = pressed(i); const was = leftWas[i]; leftWas[i] = p; return p && !was; };
    sendStick(axis(gp, 2, 0), axis(gp, 3, 1));
    let menu = false;
    for (let i = 6; i < gp.buttons.length; i++) menu = menu || pressed(i);
    const now = performance.now();
    if (menu && !leftWas.menu) { menuDownAt = now; menuLong = false; }
    if (menu && !menuLong && now - menuDownAt > 1000) { menuLong = true; statsOn = !statsOn; }   // hold: numbers
    if (!menu && leftWas.menu && !menuLong && BIG()) recentre(headPos, headQuat);             // tap: recentre
    leftWas.menu = menu;
    if (edge(5) && BIG()) openKeyboard();
    if (edge(3)) switchMode();
    return pressed(4) ? 1 : pressed(1) ? -1 : 0;   // X = up, grip = down
}

function pollRightStick(gp, aimed) {
    const x = axis(gp, 2, 0), y = axis(gp, 3, 1);
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

function pollControllers(session, frame, headPos, headQuat) {
    const ctl = [];
    let lift = 0;
    if (BIG() && !bigPlaced) recentre(headPos, headQuat);
    if (!BIG()) laser.visible = dot.visible = false;
    for (const src of session.inputSources) {
        const gp = src.gamepad;
        if (!gp) continue;
        const hand = src.handedness || 'none';
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
            const aimed = BIG() ? pollPointer(src, gp, frame) : false;
            lift = lift || pollRightStick(gp, aimed);
        }
    }
    if (config && config.mode !== 'test') sendLift(lift);
    return ctl;
}

// ------------------------------------------------------------------------------------------------ frame loop
const _m = new THREE.Matrix4(), _pos = new THREE.Vector3(), _quat = new THREE.Quaternion(), _scl = new THREE.Vector3();
const _fwd = new THREE.Vector3();
let xrErrors = 0;

function animate(time, frame) {
    const now = performance.now();
    let ctl = [];
    // Head pose in world space (dolly * reference-space pose).
    if (frame && renderer.xr.isPresenting) {
        const pose = frame.getViewerPose(renderer.xr.getReferenceSpace());
        if (pose) {
            dolly.updateMatrixWorld();
            _m.fromArray(pose.transform.matrix).premultiply(dolly.matrixWorld);
            _m.decompose(_pos, _quat, _scl);
        }
        ctl = pollControllers(renderer.xr.getSession(), frame, _pos, _quat);
        num.xrFrames++;
    } else {
        _quat.setFromEuler(new THREE.Euler(-dragPitch, -dragYaw, 0, 'YXZ'));
        _pos.set(0, 1.6, 0);
        camera.quaternion.copy(_quat);
        num.xrFrames++;
    }
    send({ t: 'pose', s: ++poseSeq, ct: now, p: [_pos.x, _pos.y, _pos.z], q: [_quat.x, _quat.y, _quat.z, _quat.w], c: ctl });

    // Newest picture onto the texture.
    let fresh = null;
    if (latest) {
        fresh = latest;
        latest = null;
        const mat = BIG() ? bigMat : screenMat;
        if (fresh.hdr.codec === 'h264') { videoTex.setFrame(fresh.img); if (mat.map !== videoTex) { mat.map = videoTex; mat.needsUpdate = true; } }
        else { bitmapTex.image = fresh.img; bitmapTex.needsUpdate = true; if (mat.map !== bitmapTex) { mat.map = bitmapTex; mat.needsUpdate = true; } }
        if (BIG()) { bigMesh.visible = true; screen.visible = false; }
        else { screen.userData.hdr = fresh.hdr; screen.visible = true; bigMesh.visible = false; }
    }
    if (!renderer.xr.isPresenting) { laser.visible = false; }
    // Reprojection: put the picture where it was rendered for, around where the head is NOW.
    const hdr = screen.userData.hdr;
    if (hdr && !BIG()) {
        const size = 2 * PICTURE_DISTANCE * Math.tan((hdr.fov * Math.PI / 180) / 2);
        screen.scale.set(size, size, 1);
        screen.quaternion.copy(hdr.q);
        _fwd.set(0, 0, -PICTURE_DISTANCE).applyQuaternion(hdr.q);
        screen.position.copy(_pos).add(_fwd);
    }

    renderer.render(scene, camera);

    if (fresh) {
        const drawn = performance.now();
        if (fresh.hdr.poseClientMs >= 0) num.p2p.add(drawn - fresh.hdr.poseClientMs);
        if (Number.isFinite(clockOff)) num.v2p.add(drawn + clockOff - fresh.hdr.captureMs);
        num.decode.add(fresh.decodeMs);
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
        send({ t: 'stats', fps: num.fps, newFps: num.newFps, decodeMs: num.decode.avg(), p2p: num.p2p.avg(),
               p2p95: num.p2p.pct(0.95), v2p: num.v2p.avg() });
    }
    if (renderer.xr.isPresenting) statsMesh.visible = statsOn || now < sizeFlashUntil;
    drawStatsPanel(now);
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

window.__wolfvr = { numbers: () => ({ rows: numberRows(), p2p: num.p2p.v.slice(-20), v2p: num.v2p.v.slice(-20) }), toggleVR, config: () => config,
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
    sendKey: (k, m = 0) => tapKey(k, m) };
connect();
