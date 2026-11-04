/*
 * WolfViewer VR in the headset browser - VR 3D Mode's world-locked eyes (depth reprojection).
 * Copyright (C) 2026 IntelligentWolf Ltd. (LGPL 2.1, as the viewer)
 *
 * [VR DEPTH 2026-11-04] Paul: "it just doesn't look 3d in 3d mode ... its like its rendering the big screen and
 * thinking its 3d". The two eye pictures were right (true stereo pairs), but each was shown as a flat card 10 m away
 * that followed the head: between pictures, and over the whole stream delay, the world moved rigidly with the head -
 * no motion parallax, nothing world-locked. Rotational timewarp ignores head movement; holding the world still under a
 * moving head needs the picture's depth (UploadVR, "Reprojection explained"; Meta's compositor: positional timewarp
 * uses the app's depth; NVIDIA CloudXR: streamed frames need depth for a stable world).
 *
 * So the viewer now sends each picture with a depth grid (1/8 of the picture each way, wolfvrpipe.h "DEPTH"), and here
 * each eye's picture is a MESH in the world: every grid point is put back where it was in the world - through the
 * head pose and that eye's own frustum it was drawn for - and the mesh is drawn by the headset for where the eyes are
 * NOW, every display frame. Turning or moving the head shows real parallax and a still world, even at 20-30 pictures a
 * second. Triangles across a depth jump (an object's edge against what is behind it) are cut, so the gap shows the far
 * shell (the same picture far away) rather than a rubber sheet. Sky and far points sit on that shell.
 */
import * as THREE from './three/three.module.js';

const SHELL = 800;          // metres: far points and the sky go no further than this; the shell card is behind it
const CUT_RATIO = 1.25;     // a grid point whose neighbours are more than 25% nearer / further is on an edge

const VERT = `
uniform sampler2D depthTex;   // metres, both eyes side by side, top row first
uniform vec4 win;             // this eye's frustum: tangents L, R, B, T
uniform float eyeIndex;       // 0 left half, 1 right half
uniform float halves;         // 2: two eyes side by side; 1: one centre view for both eyes
uniform vec2 grid;            // one eye's grid: columns, rows
uniform mat4 renderEye;       // eye space -> world, for the head pose and the eye the picture was drawn for
varying vec2 vUv;
varying float vCut;
attribute vec2 gxy;
float zAt(vec2 c) {
    c = clamp(c, vec2(0.0), grid - 1.0);
    return min(texelFetch(depthTex, ivec2(int(c.x + eyeIndex * grid.x), int(c.y)), 0).r, ${SHELL.toFixed(1)});
}
void main() {
    float z = zAt(gxy);
    float a = zAt(gxy + vec2(-1.0, 0.0)), b = zAt(gxy + vec2(1.0, 0.0)), c = zAt(gxy + vec2(0.0, -1.0)), d = zAt(gxy + vec2(0.0, 1.0));
    float zmin = min(min(min(a, b), min(c, d)), z), zmax = max(max(max(a, b), max(c, d)), z);
    vCut = zmax > zmin * ${CUT_RATIO.toFixed(2)} ? 1.0 : 0.0;
    float u = (gxy.x + 0.5) / grid.x, v = (gxy.y + 0.5) / grid.y;   // v from the top
    float tx = mix(win.x, win.y, u), ty = mix(win.w, win.z, v);
    vec4 world = renderEye * vec4(tx * z, ty * z, -z, 1.0);
    vUv = vec2((eyeIndex + u) / halves, 1.0 - v);
    gl_Position = projectionMatrix * viewMatrix * world;
}`;
// GLSL 3 (texelFetch above): three's GLSL3 prefix gives "varying" and "texture2D" but not gl_FragColor - declared here
const FRAG = `
layout(location = 0) out highp vec4 pc_fragColor;
#define gl_FragColor pc_fragColor
uniform sampler2D map;
uniform float decodeVideo;    // a video frame is sampled as stored (sRGB): decode it, as three's map_fragment does
varying vec2 vUv;
varying float vCut;
void main() {
    if (vCut > 0.001) discard;
    vec4 c = texture2D(map, vUv);
    gl_FragColor = decodeVideo > 0.5 ? sRGBTransferEOTF(c) : c;
    #include <colorspace_fragment>
}`;

export class DepthEyes {
    constructor(scene) {
        this.scene = scene;
        this.meshes = [];
        this.gw = 0; this.gh = 0;      // one eye's grid
        this.tex = null;
        this.lut = null; this.lutKey = '';
        this.active = false;
        this.frames = 0;
        this._m = new THREE.Matrix4();
        this._p = new THREE.Vector3();
        this._e = new THREE.Vector3();
        this._s = new THREE.Vector3(1, 1, 1);
    }

    _build(gw, gh, halves) {
        for (const m of this.meshes) { this.scene.remove(m); m.geometry.dispose(); m.material.dispose(); }
        this.meshes = [];
        if (this.tex) this.tex.dispose();
        this.gw = gw; this.gh = gh; this.halves = halves;
        this.data = new Float32Array(gw * halves * gh);
        this.tex = new THREE.DataTexture(this.data, gw * halves, gh, THREE.RedFormat, THREE.FloatType);
        this.tex.minFilter = this.tex.magFilter = THREE.NearestFilter;
        this.tex.generateMipmaps = false;
        this.tex.needsUpdate = true;
        // one grid per eye: points (column, row), two triangles per cell
        const gxy = new Float32Array(gw * gh * 2), idx = [];
        for (let y = 0; y < gh; y++) for (let x = 0; x < gw; x++) { gxy[2 * (y * gw + x)] = x; gxy[2 * (y * gw + x) + 1] = y; }
        for (let y = 0; y < gh - 1; y++) for (let x = 0; x < gw - 1; x++) {
            const k = y * gw + x;
            idx.push(k, k + gw, k + 1, k + 1, k + gw, k + gw + 1);
        }
        for (let e = 0; e < halves; e++) {
            const g = new THREE.BufferGeometry();
            g.setAttribute('gxy', new THREE.Float32BufferAttribute(gxy, 2));
            // a position attribute three wants for its draw count; the real positions come from the depth
            g.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(gw * gh * 3), 3));
            g.setIndex(idx);
            const mat = new THREE.ShaderMaterial({
                glslVersion: THREE.GLSL3,
                uniforms: { depthTex: { value: this.tex }, map: { value: null }, decodeVideo: { value: 0 }, win: { value: new THREE.Vector4() },
                            eyeIndex: { value: e }, halves: { value: halves }, grid: { value: new THREE.Vector2(gw, gh) },
                            renderEye: { value: new THREE.Matrix4() } },
                vertexShader: VERT, fragmentShader: FRAG, side: THREE.DoubleSide, toneMapped: false,
            });
            const m = new THREE.Mesh(g, mat);
            m.frustumCulled = false;
            m.visible = false;
            m.name = 'WolfVRDepthEye' + e;
            this.scene.add(m);
            this.meshes.push(m);
        }
    }

    // the u16 log depth -> metres, through a table made once per near / far (65536 entries)
    _table(near, far) {
        const key = near + ':' + far;
        if (key === this.lutKey) return this.lut;
        const lr = Math.log(far / near), t = new Float32Array(65536);
        for (let v = 0; v < 65536; v++) t[v] = near * Math.exp(v / 65535 * lr);
        this.lut = t; this.lutKey = key;
        return t;
    }

    // a new picture with its depth: the grid's depths, and for each eye where it was drawn from
    // [VR CENTRE] hdr.centre: one view for both eyes - one mesh, seen by both of them (each from where it is)
    show(hdr, depth, texture, eyeLayer) {
        const halves = hdr.centre ? 1 : 2;
        const gw = hdr.dw / halves, gh = hdr.dh;
        if (!depth || !gw || !gh || depth.length !== hdr.dw * hdr.dh || !hdr.tan) { this.hide(); return false; }
        if (gw !== this.gw || gh !== this.gh || halves !== this.halves) this._build(gw, gh, halves);
        const t = this._table(hdr.near, hdr.far);
        for (let i = 0; i < depth.length; i++) this.data[i] = t[depth[i]];
        this.tex.needsUpdate = true;
        for (let e = 0; e < halves; e++) {
            const u = this.meshes[e].material.uniforms;
            u.map.value = texture;
            // three decodes a video texture in its own shaders (map_fragment DECODE_VIDEO_TEXTURE: a VideoFrame cannot be
            // stored as sRGB); an image texture is decoded by the GPU when sampled
            u.decodeVideo.value = texture && texture.isVideoTexture && texture.colorSpace === THREE.SRGBColorSpace ? 1 : 0;
            const w = hdr.tan.slice(4 * e, 4 * e + 4);
            u.win.value.set(w[0], w[1], w[2], w[3]);
            // eye -> world: the head position it was drawn for (reference space), plus this eye's offset turned with
            // the head, then the head's orientation then
            if (hdr.centre) this._e.set(0, 0, 0);   // drawn from the head's centre
            else this._e.set(hdr.eyes[3 * e], hdr.eyes[3 * e + 1], hdr.eyes[3 * e + 2]).applyQuaternion(hdr.q);
            this._p.set(hdr.pos[0], hdr.pos[1], hdr.pos[2]).add(this._e);
            u.renderEye.value.compose(this._p, hdr.q, this._s);
            this.meshes[e].layers.set(hdr.centre ? 0 : eyeLayer[e]);   // the centre view: both eyes (layer 0)
            this.meshes[e].visible = true;
        }
        this.active = true;
        this.frames++;
        return true;
    }

    // on the 2D page (no headset): the left eye's mesh on the page camera's layer
    pageView(presenting, eyeLayer) {
        if (!this.active || this.halves === 1) return;
        this.meshes[0].layers.set(presenting ? eyeLayer[0] : 0);
    }

    hide() {
        for (const m of this.meshes) m.visible = false;
        this.active = false;
    }
}

// the deflated (zlib) depth bytes -> Uint16Array, in the browser's own decompressor
export async function inflateDepth(bytes) {
    const ds = new DecompressionStream('deflate');
    const buf = await new Response(new Blob([bytes]).stream().pipeThrough(ds)).arrayBuffer();
    return new Uint16Array(buf);
}

export { SHELL };
