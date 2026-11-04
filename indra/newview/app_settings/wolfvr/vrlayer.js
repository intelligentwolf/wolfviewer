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
        // already premultiplied, and sqrt() gives the coverage back.
        this.mat = new THREE.ShaderMaterial({
            uniforms: { map: { value: this.tex } },
            vertexShader: 'varying vec2 vUv; void main() { vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }',
            fragmentShader: `uniform sampler2D map; varying vec2 vUv;
                void main() {
                    vec3 c = texture2D(map, vec2(vUv.x, 0.5 + vUv.y * 0.5)).rgb;
                    // the texture is sampled as sRGB (decoded to linear); the coverage was stored as plain bytes,
                    // so encode it back (the sRGB transfer function, IEC 61966-2-1) before using it
                    float s = clamp(texture2D(map, vec2(vUv.x, vUv.y * 0.5)).r, 0.0, 1.0);
                    s = s <= 0.0031308 ? s * 12.92 : 1.055 * pow(s, 1.0 / 2.4) - 0.055;
                    float a = sqrt(s);
                    if (a < 0.02) discard;
                    gl_FragColor = vec4(c, a);
                    #include <colorspace_fragment>
                }`,
            transparent: true, depthTest: false, depthWrite: false,
            blending: THREE.CustomBlending, blendSrc: THREE.OneFactor, blendDst: THREE.OneMinusSrcAlphaFactor,
            premultipliedAlpha: true,
        });
        this.mesh = new THREE.Mesh(new THREE.BufferGeometry(), this.mat);
        this.mesh.name = 'WolfVRLayer';
        this.mesh.frustumCulled = false;
        this.mesh.renderOrder = 8000;   // under the action bar (9000) and the pointer
        this.mesh.visible = false;
        camera.add(this.mesh);
        this._built = '';
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

    update() {
        if (!this.on) { this.mesh.visible = false; return; }
        this._geometry();
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
            if (this.shown) this.shown.close();
            this.shown = img;
            this.frames++;
        }
        this.mesh.visible = !!this.shown;
    }

    // Head-space ray -> {u, v (0 at the TOP, as the viewer's pointer wants), t} or null
    hit(o, d) {
        if (!this.on || !this.mesh.visible || !this.arc) return null;
        const h = hitCylinder(o, d, RADIUS, this.arc, 0, this.y, this.height);
        return h ? { u: h.u, v: 1 - h.v, t: h.t } : null;
    }
}
