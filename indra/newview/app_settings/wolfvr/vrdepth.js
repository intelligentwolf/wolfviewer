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
 *
 * [Wolf 2026-11-06] That cut left every outline as a band two grid cells wide (16 picture pixels at 1/8) showing the
 * far shell 900 m away: the edges of near things carried no depth at all and shimmered against them. Now a grid point
 * on an edge is put on the FARTHER side of the jump (the most distant of itself and its four neighbours) and nothing is
 * discarded: the cell beyond the edge lies on the background at the background's own depth, and only the one cell
 * between the object and its edge stretches (the background stretching, never a hole). EDGE_MODE_CUT keeps the old
 * behaviour for the before/after measurement only (wolfvr.js __wolfvr.edgeProbe).
 */
import * as THREE from './three/three.module.js';

const SHELL = 800;          // metres: far points and the sky go no further than this; the shell card is behind it
const CUT_RATIO = 1.25;     // a grid point whose neighbours are more than 25% nearer / further is on an edge
const EDGE_MODE_CUT = 0, EDGE_MODE_FAR = 1;   // [Wolf 2026-11-06] see the top of the file

// [VR EDGE FILL 2026-11-09] Paul (Quest 3): "When I turn my head to the left and right there's a tiny bit of lag, where
// there is black". Each picture covers the window the viewer drew it for, and it reaches the eyes 140-190 ms after that
// pose (his device lines, 11-09 15:32-15:34Z): in a turn the side the head turns towards was past the newest picture's
// edge until the next one came, and nothing was drawn there - the scene's black. The first answer (r51) carried the
// edge's own pixels on outwards as streaks, and Paul: "when I turned my head it was a bit worse". Now:
//  - the viewer draws each eye wider than the headset shows, at the same sharpness in the middle (wolfvrscreen.cpp
//    widenEyeWindows, WolfVROverscan), so a turn first shows real picture;
//  - past even that, a frame round the picture fades within EDGE_FADE_DEG from the picture's edge to the average colour
//    of that side of the picture (the sky above, the ground below, sideColours, sampled from the pictures), a soft
//    haze in place of the hard black or the streaks; it reaches EDGE_FILL_DEG out, placed by angle (edgeDir) so it
//    goes past 90 degrees from the middle. Used by the world-locked eyes (skirt below) and the flat cards (wolfvr.js
//    placeCard, Paul's own setting: WolfVRDepth off). Inside the picture nothing changes.
const EDGE_FILL_DEG = 40, EDGE_FADE_DEG = 2;   // 5 left a stretched corner wedge in the emulator captures (os-gl1)
const EDGE_FILL = EDGE_FILL_DEG * Math.PI / 180;
// Each eye's side colours (linear, three's working colour space): left, right, bottom, top edge averages of its half of
// the newest pictures (wolfvr.js sampleSides keeps them up to date); shared by the skirt's and the cards' frames.
const sideColours = [0, 1].map(() => ({ L: new THREE.Color(0, 0, 0), R: new THREE.Color(0, 0, 0), B: new THREE.Color(0, 0, 0), T: new THREE.Color(0, 0, 0) }));
// the fade's GLSL, shared with the cards' frame (wolfvr.js): the side colour for a frame point at vSxy (-1..1 across,
// -1..1 up: how far it is towards each side's outer ring), and the edge picture colour faded into it EDGE_FADE_DEG out
const FADE_GLSL = `
uniform vec3 sideL; uniform vec3 sideR; uniform vec3 sideB; uniform vec3 sideT;
vec3 edgeFade(vec3 picture, vec2 sxy, float fade) {
    float wx = abs(sxy.x), wy = abs(sxy.y);
    vec3 side = ((sxy.x < 0.0 ? sideL : sideR) * wx + (sxy.y < 0.0 ? sideB : sideT) * wy) / max(wx + wy, 1e-4);
    return mix(picture, side, smoothstep(0.0, ${(EDGE_FADE_DEG / EDGE_FILL_DEG).toFixed(4)}, fade));
}`;
// The fill's ring point for the picture's edge point at tangents (tx, ty) (eye space: -Z ahead, +X right, +Y up), turned
// sx * fill sideways and sy * fill up (each -1, 0 or +1), at the same distance from the eye as that edge point at depth
// z: its angles across (atan tx) and up (measured as the direction (tx, ty, -1) has it), moved, back to a direction.
// The same formula as VERT's ring (keep them together).
function edgeDir(tx, ty, z, sx, sy, fill, out) {
    const r = z * Math.hypot(tx, ty, 1), ax = Math.atan(tx) + sx * fill;
    const ay = Math.max(-1.55, Math.min(1.55, Math.atan2(ty, Math.hypot(tx, 1)) + sy * fill));
    out[0] = r * Math.sin(ax) * Math.cos(ay); out[1] = r * Math.sin(ay); out[2] = -r * Math.cos(ax) * Math.cos(ay);
    return out;
}

const VERT = `
uniform sampler2D depthTex;   // metres, both eyes side by side, top row first
uniform vec4 win;             // this eye's frustum: tangents L, R, B, T
uniform float fill;           // [VR EDGE FILL 2026-11-09] how far the edge fill's ring is turned out past the edge (radians)
uniform float skirt;          // [VR EDGE FILL] 1: this mesh is the edge fill round the grid (never cut), 0: the grid
uniform float eyeIndex;       // 0 left half, 1 right half
uniform float halves;         // 2: two eyes side by side; 1: one centre view for both eyes
uniform vec2 grid;            // one eye's grid: columns, rows
uniform mat4 renderEye;       // eye space -> world, for the head pose and the eye the picture was drawn for
uniform float edgeMode;       // [Wolf 2026-11-06] 1: an edge point goes to the far side of its jump; 0: cut (the old way)
varying vec2 vUv;
varying float vCut;
varying vec2 vSxy;            // [VR EDGE FILL] towards which side's outer ring this point is (0 on the picture itself)
attribute vec2 gxy;           // the grid point; the edge fill's outer ring is one point beyond the grid (-1 or grid)
float zAt(vec2 c) {
    c = clamp(c, vec2(0.0), grid - 1.0);
    return min(texelFetch(depthTex, ivec2(int(c.x + eyeIndex * grid.x), int(c.y)), 0).r, ${SHELL.toFixed(1)});
}
void main() {
    // [VR EDGE FILL] an outer ring point takes the depth and the colour of the grid's edge point next to it (g)
    vec2 g = clamp(gxy, vec2(0.0), grid - 1.0);
    float z = zAt(g);
    float a = zAt(g + vec2(-1.0, 0.0)), b = zAt(g + vec2(1.0, 0.0)), c = zAt(g + vec2(0.0, -1.0)), d = zAt(g + vec2(0.0, 1.0));
    float zmin = min(min(min(a, b), min(c, d)), z), zmax = max(max(max(a, b), max(c, d)), z);
    bool edge = zmax > zmin * ${CUT_RATIO.toFixed(2)};
    vCut = edge && edgeMode < 0.5 && skirt < 0.5 ? 1.0 : 0.0;
    if (edge && edgeMode > 0.5) z = zmax;   // [Wolf 2026-11-06] on the background, along this point's own view ray
    float u = (g.x + 0.5) / grid.x, v = (g.y + 0.5) / grid.y;   // v from the top
    float tx = mix(win.x, win.y, u), ty = mix(win.w, win.z, v);
    vec3 p = vec3(tx * z, ty * z, -z);
    // [VR EDGE FILL] the outer ring: the edge point's direction turned out by fill (edgeDir in this file, the same
    // formula), at the edge point's own distance (v from the top: row -1 is above the top edge)
    float sx = gxy.x < -0.5 ? -1.0 : (gxy.x > grid.x - 0.5 ? 1.0 : 0.0), sy = gxy.y < -0.5 ? 1.0 : (gxy.y > grid.y - 0.5 ? -1.0 : 0.0);
    vSxy = vec2(sx, sy);
    if (sx != 0.0 || sy != 0.0) {
        float r = z * length(vec3(tx, ty, 1.0)), ax = atan(tx) + sx * fill;
        float ay = clamp(atan(ty, length(vec2(tx, 1.0))) + sy * fill, -1.55, 1.55);
        p = r * vec3(sin(ax) * cos(ay), sin(ay), -cos(ax) * cos(ay));
    }
    vec4 world = renderEye * vec4(p, 1.0);
    vUv = vec2((eyeIndex + u) / halves, 1.0 - v);
    gl_Position = projectionMatrix * viewMatrix * world;
}`;
// GLSL 3 (texelFetch above): three's GLSL3 prefix gives "varying" and "texture2D" but not gl_FragColor - declared here
const FRAG = `
layout(location = 0) out highp vec4 pc_fragColor;
#define gl_FragColor pc_fragColor
uniform sampler2D map;
uniform float decodeVideo;    // a video frame is sampled as stored (sRGB): decode it, as three's map_fragment does
uniform float skirt;          // [VR EDGE FILL] the frame round the grid: faded into the side colours (the grid: never)
varying vec2 vUv;
varying float vCut;
varying vec2 vSxy;
${FADE_GLSL}
void main() {
    if (vCut > 0.001) discard;
    vec4 c = texture2D(map, vUv);
    gl_FragColor = decodeVideo > 0.5 ? sRGBTransferEOTF(c) : c;
    if (skirt > 0.5) gl_FragColor.rgb = edgeFade(gl_FragColor.rgb, vSxy, max(abs(vSxy.x), abs(vSxy.y)));
    #include <colorspace_fragment>
}`;

export class DepthEyes {
    constructor(scene) {
        this.scene = scene;
        this.meshes = [];
        this.skirts = [];              // [VR EDGE FILL 2026-11-09] each eye's edge fill (same order as meshes)
        this.edgeFill = true;          // [VR EDGE FILL] test hook only (wolfvr.js __wolfvr.edgeFill): off = the old black
        this.gw = 0; this.gh = 0;      // one eye's grid
        this.tex = null;
        this.lut = null; this.lutKey = '';
        this.active = false;
        this.frames = 0;
        // [Wolf 2026-11-06] CUT is the default again: Paul's Quest test of the far-side mode the same day was "horrible
        // wobbling" (edge points moving to the far side jump as each picture's depth changes). FAR stays for measurement only.
        this.edgeMode = EDGE_MODE_CUT;
        this._m = new THREE.Matrix4();
        this._p = new THREE.Vector3();
        this._e = new THREE.Vector3();
        this._s = new THREE.Vector3(1, 1, 1);
    }

    _build(gw, gh, halves) {
        for (const m of this.meshes.concat(this.skirts)) { this.scene.remove(m); m.geometry.dispose(); m.material.dispose(); }
        this.meshes = [];
        this.skirts = [];
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
        // [VR EDGE FILL 2026-11-09] the edge fill: the grid's points with one more ring round them (columns -1 .. gw,
        // rows -1 .. gh), and only the cells that touch that ring - a frame of quads from the grid's edge out to the
        // ring turned EDGE_FILL out past the edge (VERT, the uniform fill), at the depth and colour of the edge point next to it)
        const sw = gw + 2, sh = gh + 2, sxy = new Float32Array(sw * sh * 2), sidx = [];
        for (let y = 0; y < sh; y++) for (let x = 0; x < sw; x++) { sxy[2 * (y * sw + x)] = x - 1; sxy[2 * (y * sw + x) + 1] = y - 1; }
        for (let y = 0; y < sh - 1; y++) for (let x = 0; x < sw - 1; x++) {
            if (x !== 0 && x !== sw - 2 && y !== 0 && y !== sh - 2) continue;
            const k = y * sw + x;
            sidx.push(k, k + sw, k + 1, k + 1, k + sw, k + sw + 1);
        }
        for (let e = 0; e < halves; e++) {
            const g = new THREE.BufferGeometry();
            g.setAttribute('gxy', new THREE.Float32BufferAttribute(gxy, 2));
            // a position attribute three wants for its draw count; the real positions come from the depth
            g.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(gw * gh * 3), 3));
            g.setIndex(idx);
            const uniforms = { depthTex: { value: this.tex }, map: { value: null }, decodeVideo: { value: 0 }, win: { value: new THREE.Vector4() },
                               fill: { value: EDGE_FILL }, skirt: { value: 0 },
                               sideL: { value: sideColours[e].L }, sideR: { value: sideColours[e].R },
                               sideB: { value: sideColours[e].B }, sideT: { value: sideColours[e].T },
                               eyeIndex: { value: e }, halves: { value: halves }, grid: { value: new THREE.Vector2(gw, gh) },
                               renderEye: { value: new THREE.Matrix4() }, edgeMode: { value: this.edgeMode } };
            const mat = new THREE.ShaderMaterial({
                glslVersion: THREE.GLSL3,
                uniforms,
                vertexShader: VERT, fragmentShader: FRAG, side: THREE.DoubleSide, toneMapped: false,
            });
            const m = new THREE.Mesh(g, mat);
            m.frustumCulled = false;
            m.visible = false;
            m.name = 'WolfVRDepthEye' + e;
            this.scene.add(m);
            this.meshes.push(m);
            // [VR EDGE FILL] the eye's edge fill: the same uniforms (one update in show() moves both), skirt on. Drawn
            // before the grid (renderOrder) and without writing depth, so it can never cover any of the picture itself,
            // only the black outside it; after the far shell's cards (renderOrder -10), which it lies in front of.
            const sg = new THREE.BufferGeometry();
            sg.setAttribute('gxy', new THREE.Float32BufferAttribute(sxy, 2));
            sg.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(sw * sh * 3), 3));
            sg.setIndex(sidx);
            const smat = new THREE.ShaderMaterial({
                glslVersion: THREE.GLSL3,
                uniforms: { ...uniforms, skirt: { value: 1 } },
                vertexShader: VERT, fragmentShader: FRAG, side: THREE.DoubleSide, toneMapped: false, depthWrite: false,
            });
            const s = new THREE.Mesh(sg, smat);
            s.frustumCulled = false;
            s.visible = false;
            s.renderOrder = -5;
            s.name = 'WolfVRDepthEdge' + e;
            this.scene.add(s);
            this.skirts.push(s);
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
            this.skirts[e].layers.mask = this.meshes[e].layers.mask;   // [VR EDGE FILL] seen by the same eye
            this.skirts[e].visible = this.edgeFill;
        }
        this.active = true;
        this.frames++;
        return true;
    }

    // on the 2D page (no headset): the left eye's mesh on the page camera's layer
    pageView(presenting, eyeLayer) {
        if (!this.active || this.halves === 1) return;
        this.meshes[0].layers.set(presenting ? eyeLayer[0] : 0);
        this.skirts[0].layers.mask = this.meshes[0].layers.mask;   // [VR EDGE FILL]
    }

    // [Wolf 2026-11-06] test only (the before/after edge measurement): EDGE_MODE_CUT (the default) or EDGE_MODE_FAR
    setEdgeMode(m) { this.edgeMode = m; for (const x of this.meshes.concat(this.skirts)) x.material.uniforms.edgeMode.value = m; }
    // the share of the grid's points that are on an edge (a depth jump of more than CUT_RATIO to a neighbour)
    edgeShare() {
        if (!this.active) return null;
        // as VERT's zAt: each eye's own grid, clamped at its edges
        const gw = this.gw, W = gw * this.halves, H = this.gh;
        const z = (e, x, y) => Math.min(this.data[Math.min(H - 1, Math.max(0, y)) * W + e * gw + Math.min(gw - 1, Math.max(0, x))], SHELL);
        let n = 0;
        for (let e = 0; e < this.halves; e++) for (let y = 0; y < H; y++) for (let x = 0; x < gw; x++) {
            const v = [z(e, x, y), z(e, x - 1, y), z(e, x + 1, y), z(e, x, y - 1), z(e, x, y + 1)];
            if (Math.max(...v) > Math.min(...v) * CUT_RATIO) n++;
        }
        return n / (W * H);
    }

    hide() {
        for (const m of this.meshes.concat(this.skirts)) m.visible = false;
        this.active = false;
    }
}

// the deflated (zlib) depth bytes -> Uint16Array, in the browser's own decompressor
export async function inflateDepth(bytes) {
    const ds = new DecompressionStream('deflate');
    const buf = await new Response(new Blob([bytes]).stream().pipeThrough(ds)).arrayBuffer();
    return new Uint16Array(buf);
}

export { SHELL, EDGE_MODE_CUT, EDGE_MODE_FAR, EDGE_FILL, edgeDir, sideColours, FADE_GLSL };
