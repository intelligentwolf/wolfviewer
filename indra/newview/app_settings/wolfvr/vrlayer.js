/*
 * WolfViewer VR in the headset browser - the UI layer (VR 3D Mode): the viewer's HUDs and floaters in the headset.
 * Copyright (C) 2026 IntelligentWolf Ltd. (LGPL 2.1, as the viewer)
 *
 * [VR HUD LAYER 2026-11-04] Paul: "how are we going to implement the different HUDs for different things like ships?"
 * The viewer draws its UI layer a second time on its own (wolfvrscreen.cpp captureLayer): HUD attachments (ship helms,
 * the flight deck, the dogfight radar, game HUDs, residents' own HUDs), floaters and notices, transparent everywhere
 * else, and streams it as a second H.264 picture with the coverage stacked under the colour (wolfvrpipe.cpp
 * layerLoop). Here it is put back together and shown on a curved band docked to the head, the way WolfStorm docks
 * its HUD attachments in VR (vr_hud.js: "SL HUD attachments dock the same way - they are screen-space by
 * definition"). So EVERY HUD works without per-HUD code. Pointing at it with the right controller is the mouse on
 * that HUD or floater, exactly as Big Screen Mode is.
 *
 * Where it sits: standing, a band in front at eye height; seated on something (a ship's wheel, a plane, a car) a lower
 * "dashboard" band, like looking down at instruments. Each is remembered separately in this browser (grip + move
 * the controller up or down moves it; grip + right stick up / down makes it bigger or smaller).
 */
import * as THREE from './three/three.module.js';
import { curvedGeometry, hitCylinder } from './vrbar.js';

const RADIUS = 1.25;
const PLACES = {
    stand: { arcDeg: 72, y: -0.02 },   // centred a little below the eye line
    seat: { arcDeg: 64, y: -0.36 },    // the dashboard: lower, as instruments are
};
const KEY = 'wolfvr.layer.';
// [VR PANELS 2026-11-04] each viewer window as its own panel: radius, the angle a whole window's width would span (a
// little bigger than on the band, for reading), the gap kept between panels, and how many cut-outs the band shader takes
const PANEL_R = 1.3, PANEL_SCALE = 1.25, PANEL_GAP = 3 * Math.PI / 180, MAX_CUTS = 12;
// One shader for the band and the panels: `sub` picks the part of the picture (x, y from the bottom-left, w, h), and
// the band leaves out the windows that are panels of their own (`cut`, window rectangles x0, y0 from the top-left, x1, y1).
const VERT = 'varying vec2 vUv; void main() { vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }';
const FRAG = `uniform sampler2D map; uniform vec4 sub; uniform vec4 cut[${MAX_CUTS}]; uniform int cutN; varying vec2 vUv;
    void main() {
        vec2 w = sub.xy + vUv * sub.zw;
        for (int i = 0; i < ${MAX_CUTS}; i++) {
            if (i >= cutN) break;
            vec4 r = cut[i];
            if (w.x >= r.x && w.x <= r.z && 1.0 - w.y >= r.y && 1.0 - w.y <= r.w) discard;
        }
        vec3 c = texture2D(map, vec2(w.x, 0.5 + w.y * 0.5)).rgb;
        // the texture is sampled as sRGB (decoded to linear); the coverage was stored as plain bytes,
        // so encode it back (the sRGB transfer function, IEC 61966-2-1) before using it
        float s = clamp(texture2D(map, vec2(w.x, w.y * 0.5)).r, 0.0, 1.0);
        s = s <= 0.0031308 ? s * 12.92 : 1.055 * pow(s, 1.0 / 2.4) - 0.055;
        float a = sqrt(s);
        if (a < 0.02) discard;
        gl_FragColor = vec4(c, a);
        #include <colorspace_fragment>
    }`;
function layerMaterial(map, panel) {
    const cuts = [];
    for (let i = 0; i < MAX_CUTS; i++) cuts.push(new THREE.Vector4());
    return new THREE.ShaderMaterial({
        uniforms: { map: { value: map }, sub: { value: new THREE.Vector4(0, 0, 1, 1) }, cut: { value: cuts }, cutN: { value: 0 } },
        vertexShader: VERT, fragmentShader: FRAG,
        transparent: true, depthTest: false, depthWrite: false,
        blending: THREE.CustomBlending, blendSrc: THREE.OneFactor, blendDst: THREE.OneMinusSrcAlphaFactor,
        premultipliedAlpha: true,
        // a panel: its backing is drawn too (a window is never see-through where the viewer drew it half-transparent)
        side: panel ? THREE.DoubleSide : THREE.FrontSide,
    });
}

function loadPlace(name) {
    const d = { ...PLACES[name] };
    try {
        const raw = localStorage.getItem(KEY + name);
        if (raw) {
            const v = JSON.parse(raw);
            if (Number.isFinite(v.arcDeg) && v.arcDeg >= 30 && v.arcDeg <= 120) d.arcDeg = v.arcDeg;
            if (Number.isFinite(v.y) && v.y >= -0.9 && v.y <= 0.5) d.y = v.y;
        }
    } catch (e) { /* storage blocked: the defaults */ }
    return d;
}

export class VRLayer {
    constructor(camera, send, onError) {
        this.send = send;
        this.onError = onError;
        this.want = true;             // shown (the HUDs button); remembered
        try { this.want = localStorage.getItem(KEY + 'on') !== '0'; } catch (e) { /* default on */ }
        this.active = false;          // VR 3D Mode and presenting
        this.cfg = null;              // layerconfig {codec, codecString, w, h}
        this.decoder = null;
        this.decoderCodec = '';
        this.waitingKey = true;
        this.lastKeyAsk = 0;
        this.latest = null;           // newest decoded picture not yet on the texture
        this.shown = null;
        this.frames = 0;
        this.errors = 0;              // pictures that could not be decoded
        this.place = 'stand';
        this.places = { stand: loadPlace('stand'), seat: loadPlace('seat') };
        this.tex = new THREE.VideoFrameTexture();
        this.tex.colorSpace = THREE.SRGBColorSpace;
        this.bmp = new THREE.Texture();
        this.bmp.colorSpace = THREE.SRGBColorSpace;
        this.bmp.flipY = false;
        this.bmp.generateMipmaps = false;
        this.bmp.minFilter = THREE.LinearFilter;
        // Colour is the top half of the picture, coverage (as grey) the bottom half. The viewer's UI blends its alpha
        // channel with the same factors as its colour (LLRender blendFunc SRC_ALPHA / ONE_MINUS_SRC_ALPHA), so over
        // a cleared, transparent target a pixel of coverage a ends up with colour c*a and alpha a*a: the colour is
        // already premultiplied, and sqrt() gives the coverage back (FRAG above).
        this.mat = layerMaterial(this.tex, false);
        this.mesh = new THREE.Mesh(new THREE.BufferGeometry(), this.mat);
        this.mesh.name = 'WolfVRLayer';
        this.mesh.frustumCulled = false;
        this.mesh.renderOrder = 8000;   // under the action bar (9000) and the pointer
        this.mesh.visible = false;
        camera.add(this.mesh);
        this._built = '';
        // [VR PANELS] the viewer's windows, each its own panel in the room (world), keyed by the viewer's window name
        this.world = null;            // the group the panels live in (attachWorld)
        this.panels = new Map();      // k -> { mesh, mat, r: [x0, y0, x1, y1], yaw, arc, h, c: Vector3, n }
        this.placed = new Map();      // k -> { yaw, y } where a window was last (this session), so it comes back there
        this.rects = [];              // the list now shown
        this.nextRects = null;        // the list that goes with the next picture
        this.nextAt = 0;
        this.moving = null;           // { k, dyaw, y0, ry0 } while the grip moves a panel
        this._head = { p: new THREE.Vector3(), q: new THREE.Quaternion() };
    }

    attachWorld(group) { this.world = group; }
    // the viewer's window list ({"t":"panels"}): applied with the picture that follows it (or after 300 ms anyway)
    setPanels(list) {
        if (!Array.isArray(list)) return;
        this.nextRects = list.filter((p) => p && typeof p.k === 'string' && Array.isArray(p.r) && p.r.length === 4 &&
            p.r.every(Number.isFinite) && p.r[2] > p.r[0] && p.r[3] > p.r[1]).slice(0, MAX_CUTS);
        this.nextAt = performance.now();
    }
    _yawOf(q) { const f = new THREE.Vector3(0, 0, -1).applyQuaternion(q); return Math.atan2(-f.x, -f.z); }
    _applyRects() {
        this.rects = this.nextRects;
        this.nextRects = null;
        const keep = new Set(this.rects.map((p) => p.k));
        for (const [k, p] of this.panels) {
            if (keep.has(k)) continue;
            this.placed.set(k, { yaw: p.yaw, y: p.c.y });
            if (p.mesh.parent) p.mesh.parent.remove(p.mesh);
            p.mesh.geometry.dispose(); p.mat.dispose();
            this.panels.delete(k);
            if (this.moving && this.moving.k === k) this.moving = null;
        }
        const aspect = this.cfg && this.cfg.w && this.cfg.h ? this.cfg.w / this.cfg.h : 16 / 9;
        for (const r of this.rects) {
            const [x0, y0, x1, y1] = r.r;
            const arc = Math.min(Math.PI * 0.8, (x1 - x0) * (this.arc || 72 * Math.PI / 180) * PANEL_SCALE);
            const h = PANEL_R * arc * ((y1 - y0) / (x1 - x0)) / aspect;
            let p = this.panels.get(r.k);
            if (p && (Math.abs(p.arc - arc) > 1e-3 || Math.abs(p.h - h) > 1e-3)) {
                p.mesh.geometry.dispose();
                p.mesh.geometry = curvedGeometry(PANEL_R, arc, h, Math.max(8, Math.round(arc * 180 / Math.PI / 3)));
                p.arc = arc; p.h = h;
            }
            if (!p) {
                const mat = layerMaterial(this.mat.uniforms.map.value, true);
                const mesh = new THREE.Mesh(curvedGeometry(PANEL_R, arc, h, Math.max(8, Math.round(arc * 180 / Math.PI / 3))), mat);
                mesh.name = 'WolfVRPanel:' + r.k;
                mesh.frustumCulled = false;
                mesh.renderOrder = 8500;   // over the HUD band, under the action bar
                p = { mesh, mat, arc, h, c: new THREE.Vector3(), yaw: 0, k: r.k };
                const was = this.placed.get(r.k);
                const hy = this._yawOf(this._head.q);
                p.c.copy(this._head.p);
                if (was) { p.yaw = was.yaw; p.c.y = was.y; }
                else { p.yaw = this._freeYaw(hy, arc); p.c.y = this._head.p.y - 0.08 + (0.5 - (y0 + y1) / 2) * 0.5; }
                mesh.position.copy(p.c);
                mesh.rotation.set(0, p.yaw, 0);
                if (this.world) this.world.add(mesh);
                this.panels.set(r.k, p);
            }
            p.r = r.r; p.n = r.n;
            p.mat.uniforms.sub.value.set(x0, 1 - y1, x1 - x0, y1 - y0);
        }
        // the band leaves the panels' windows out
        const cuts = this.mat.uniforms.cut.value;
        this.rects.forEach((r, i) => cuts[i].set(r.r[0], r.r[1], r.r[2], r.r[3]));
        this.mat.uniforms.cutN.value = this.rects.length;
    }
    // the free direction nearest to where the resident looks: panels side by side in an arc, never on top of each other
    _freeYaw(centre, arc) {
        const taken = [...this.panels.values()].map((p) => [p.yaw - p.arc / 2 - PANEL_GAP, p.yaw + p.arc / 2 + PANEL_GAP]);
        const step = 4 * Math.PI / 180;
        for (let k = 0; k < 90; k++) {
            const off = (k % 2 ? 1 : -1) * Math.ceil(k / 2) * step;   // 0, +4, -4, +8 ... degrees (+ = left)
            const y = centre + off, a = y - arc / 2, b = y + arc / 2;
            if (!taken.some(([lo, hi]) => a < hi && b > lo)) return y;
        }
        return centre;
    }
    // world ray -> the nearest panel it points at: {u, v (window, from the top-left), t, k} or null
    hitPanels(o, d) {
        if (!this.on) return null;
        let best = null;
        const lo = new THREE.Vector3();
        for (const p of this.panels.values()) {
            if (!p.mesh.visible) continue;
            lo.copy(o).sub(p.c);
            const h = hitCylinder(lo, d, PANEL_R, p.arc, p.yaw, 0, p.h);
            if (!h || (best && best.t <= h.t)) continue;
            const [x0, y0, x1, y1] = p.r;
            best = { u: x0 + h.u * (x1 - x0), v: y0 + (1 - h.v) * (y1 - y0), t: h.t, k: p.k };
        }
        return best;
    }
    // grip held on a panel: it follows the controller round the arc and up or down; let go: it stays there
    movePanel(k, o, d, held) {
        if (!held) { this.moving = null; return; }
        const yaw = Math.atan2(-d.x, -d.z);
        if (!this.moving) {
            const p = this.panels.get(k);
            if (!p) return;
            this.moving = { k, dyaw: p.yaw - yaw, y0: p.c.y, ry0: o.y + d.y };
            return;
        }
        const p = this.panels.get(this.moving.k);
        if (!p) { this.moving = null; return; }
        p.yaw = yaw + this.moving.dyaw;
        p.c.y = Math.max(this._head.p.y - 1.0, Math.min(this._head.p.y + 0.8, this.moving.y0 + (o.y + d.y - this.moving.ry0)));
        p.mesh.position.copy(p.c);
        p.mesh.rotation.set(0, p.yaw, 0);
        this.placed.set(p.k, { yaw: p.yaw, y: p.c.y });
    }

    // ---- what is shown ----
    setWant(on) {
        this.want = on;
        try { localStorage.setItem(KEY + 'on', on ? '1' : '0'); } catch (e) { /* not remembered */ }
        this._tell();
    }
    setActive(on) { if (this.active !== on) { this.active = on; this._tell(); } }
    _tell() {
        const on = this.want && this.active;
        this.send({ t: 'layer', on });
        if (on) { this.waitingKey = true; this.askKey(true); }
        if (!on) this.mesh.visible = false;
    }
    // seated on an object (viewer "ui" sit = 2): the dashboard place
    setSeated(seated) {
        const p = seated ? 'seat' : 'stand';
        if (p !== this.place) { this.place = p; this._built = ''; }
    }
    get on() { return this.want && this.active; }

    _geometry() {
        const pl = this.places[this.place];
        const aspect = this.cfg && this.cfg.w && this.cfg.h ? this.cfg.w / this.cfg.h : 16 / 9;
        const key = `${this.place}:${pl.arcDeg}:${pl.y}:${aspect.toFixed(3)}`;
        if (key === this._built) return;
        this._built = key;
        this.arc = pl.arcDeg * Math.PI / 180;
        this.height = RADIUS * this.arc / aspect;
        this.y = pl.y;
        this.mesh.geometry.dispose();
        this.mesh.geometry = curvedGeometry(RADIUS, this.arc, this.height, Math.round(pl.arcDeg / 2));
        this.mesh.position.set(0, this.y, 0);
    }
    // grip held while pointing: move it up or down (dy metres), or resize (+1 / -1 step)
    nudge(dy, size) {
        const pl = this.places[this.place];
        if (dy) pl.y = Math.max(-0.9, Math.min(0.5, pl.y + dy));
        if (size) pl.arcDeg = Math.max(30, Math.min(120, pl.arcDeg + size * 6));
        try { localStorage.setItem(KEY + this.place, JSON.stringify(pl)); } catch (e) { /* not remembered */ }
        this._built = '';
    }

    // ---- the stream ----
    onConfig(m) {
        this.cfg = m;
        this._built = '';
        this.waitingKey = true;
        if (m.codec !== 'h264') { this._closeDecoder(); return; }
        if (this.decoder && this.decoderCodec === m.codecString && this.decoder.state === 'configured') return;
        this._closeDecoder();
        const conf = { codec: m.codecString, optimizeForLatency: true };
        VideoDecoder.isConfigSupported(conf).then((r) => {
            if (!r.supported) { this.onError('This headset cannot show the HUDs picture (its video decoder said no).'); return; }
            this.decoder = new VideoDecoder({
                output: (vf) => { if (this.latest) this.latest.close(); this.latest = vf; },
                error: () => { this._closeDecoder(); this.askKey(true); },
            });
            this.decoder.configure(conf);
            this.decoderCodec = m.codecString;
        }).catch((e) => this.onError('The HUDs picture could not start: ' + e.message));
    }
    _closeDecoder() {
        if (this.decoder) { try { this.decoder.close(); } catch (e) { /* closed */ } }
        this.decoder = null;
        this.decoderCodec = '';
    }
    askKey(force) {
        const now = performance.now();
        if (force || now - this.lastKeyAsk > 1000) { this.lastKeyAsk = now; this.send({ t: 'key', s: 1 }); }
    }
    onPicture(buf, hdr) {
        if (!this.on) return;
        const payload = new Uint8Array(buf, hdr.size, hdr.bytes);
        if (hdr.codec === 'jpeg') {
            createImageBitmap(new Blob([payload], { type: 'image/jpeg' }), { imageOrientation: 'flipY' })
                .then((bmp) => { if (this.latest) this.latest.close(); this.latest = bmp; })
                .catch(() => {
                    // a bad picture is skipped; many in a row is a problem the resident is told about
                    if (++this.errors === 5) this.onError('Your HUDs and windows could not be shown in the headset (its browser could not read the pictures).');
                });
            return;
        }
        if (!this.decoder || this.decoder.state !== 'configured') return;
        if (this.waitingKey) { if (!hdr.key) { this.askKey(false); return; } this.waitingKey = false; }
        if (this.decoder.decodeQueueSize > 4) { this.waitingKey = true; this.askKey(false); return; }
        this.decoder.decode(new EncodedVideoChunk({ type: hdr.key ? 'key' : 'delta', timestamp: hdr.frame, data: payload }));
    }

    update(headPos, headQuat) {
        if (headPos) { this._head.p.copy(headPos); this._head.q.copy(headQuat); }
        if (!this.on) { this.mesh.visible = false; for (const p of this.panels.values()) p.mesh.visible = false; return; }
        this._geometry();
        if (this.nextRects && this.shown && performance.now() - this.nextAt > 300) this._applyRects();   // [VR PANELS] no picture came
        if (this.latest) {
            const img = this.latest;
            this.latest = null;
            if (typeof VideoFrame !== 'undefined' && img instanceof VideoFrame) {
                this.tex.setFrame(img);
                if (this.mat.uniforms.map.value !== this.tex) this.mat.uniforms.map.value = this.tex;
            } else {
                this.bmp.image = img;
                this.bmp.needsUpdate = true;
                if (this.mat.uniforms.map.value !== this.bmp) this.mat.uniforms.map.value = this.bmp;
            }
            if (this.nextRects) this._applyRects();   // [VR PANELS] the window list that came with this picture
            if (this.shown) this.shown.close();
            this.shown = img;
            this.frames++;
        }
        this.mesh.visible = !!this.shown;
        for (const p of this.panels.values()) {
            p.mesh.visible = !!this.shown;
            if (p.mat.uniforms.map.value !== this.mat.uniforms.map.value) p.mat.uniforms.map.value = this.mat.uniforms.map.value;
        }
    }

    // Head-space ray -> {u, v (0 at the TOP, as the viewer's pointer wants), t} or null
    hit(o, d) {
        if (!this.on || !this.mesh.visible || !this.arc) return null;
        const h = hitCylinder(o, d, RADIUS, this.arc, 0, this.y, this.height);
        if (!h) return null;
        const u = h.u, v = 1 - h.v;
        // [VR PANELS] a window that is a panel of its own is not on the band (it is cut out there)
        if (this.rects.some((r) => u >= r.r[0] && u <= r.r[2] && v >= r.r[1] && v <= r.r[3])) return null;
        return { u, v, t: h.t };
    }
}
