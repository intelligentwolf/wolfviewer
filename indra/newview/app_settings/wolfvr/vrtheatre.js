/**
 * [VR THEATRE 2026-11-05] Paul: "add some theater seats or being in a theatre ... in big screen mode like virtual desktop
 * does", and before W96 "we didnt build the theatre or stretching for vr screen mode". A dark cinema room around the
 * resident in VR Big Screen Mode: the big screen hangs on the front wall between red curtains, the resident sits in the
 * FRONT row (nothing between them and the screen), rows of red seats beside and stepped up behind them, carpet, dim wall
 * lights and step lights. Built here from simple shapes (no third-party models - memory
 * paul-build-our-own-no-third-party-models); cheap for a headset: one instanced mesh per seat part, Lambert materials,
 * two lights.
 *
 * Frame: the theatre is a child of the big screen's group (wolfvr.js bigGroup: origin at the resident's head when the
 * screen was placed, -Z towards the screen), so recentring the screen moves the whole room with it. The floor is put
 * where the headset says the floor is (local-floor reference space: world y = 0), or 1.2 m under the head when the
 * browser gave no floor.
 */
import * as THREE from './three/three.module.js';

const SEAT_RED = 0x8c1a2c, ARM = 0x2e2420, CARPET = 0x3a1620, WALL = 0x2a2230, PANEL = 0x3a2e3c, CEIL = 0x120f16, CURTAIN = 0x7d1424;
const ROW_GAP = 1.1;          // metres between rows (front to front)
const ROW_RISE = 0.35;        // each row behind is this much higher (a stepped cinema floor)
const SEAT_PITCH = 0.62;      // seat centre to seat centre along a row
const SEATS_PER_SIDE = 6;     // in each row, either side of the centre line
const ROWS_BEHIND = 4;

// the light a wall sconce throws on the wall: a soft warm fan, brightest just under the lamp, fading to nothing
function washTexture() {
    const c = document.createElement('canvas');
    c.width = 128; c.height = 192;
    const g = c.getContext('2d');
    // an ellipse (the circle stretched 1.5 x down the wall) that is fully clear before every edge of the texture
    g.scale(1, 1.5);
    const grad = g.createRadialGradient(64, 36, 2, 64, 62, 60);
    grad.addColorStop(0, 'rgba(255,190,120,0.55)');
    grad.addColorStop(0.4, 'rgba(255,160,90,0.2)');
    grad.addColorStop(1, 'rgba(255,140,80,0)');
    g.fillStyle = grad;
    g.fillRect(0, 0, 128, 128);
    const t = new THREE.CanvasTexture(c);
    t.colorSpace = THREE.SRGBColorSpace;
    return t;
}

export class VRTheatre {
    /** @param {THREE.Object3D} parent the big screen's group */
    constructor(parent) {
        this.group = new THREE.Group();
        this.group.visible = false;
        parent.add(this.group);
        this.room = new THREE.Group();   // rebuilt when the screen's distance or size changes
        this.group.add(this.room);
        this.lights = new THREE.Group();
        this.group.add(this.lights);
        // cinema-dim, not black (a headset shows near-black as nothing): warm from above, the floor darker
        const hemi = new THREE.HemisphereLight(0xffe2c4, 0x1a0c10, 0.75);
        // the screen's own glow on the room: a soft cool light from the screen's direction
        const glow = new THREE.DirectionalLight(0xcfd8ff, 0.9);
        glow.position.set(0, 0.3, -1);
        this.lights.add(hemi, glow, glow.target);
        this.key = '';
    }

    /**
     * Places the room for a screen of `width` x `height` metres at `radius` from the head, the floor `floorY` metres
     * from the head (negative = below). Rebuilds only when something changed.
     */
    layout(radius, width, height, floorY) {
        const key = [radius, width, height, floorY].map((x) => x.toFixed(2)).join(',');
        if (key === this.key) return;
        this.key = key;
        for (const c of [...this.room.children]) { this.room.remove(c); c.traverse((o) => { if (o.geometry) o.geometry.dispose(); }); }
        const mats = (this.mats = this.mats || {
            seat: new THREE.MeshLambertMaterial({ color: SEAT_RED }),
            arm: new THREE.MeshLambertMaterial({ color: ARM }),
            carpet: new THREE.MeshLambertMaterial({ color: CARPET }),
            wall: new THREE.MeshLambertMaterial({ color: WALL }),
            panel: new THREE.MeshLambertMaterial({ color: PANEL }),
            wash: new THREE.MeshBasicMaterial({ map: washTexture(), transparent: true, depthWrite: false, blending: THREE.AdditiveBlending }),
            ceil: new THREE.MeshLambertMaterial({ color: CEIL }),
            curtain: new THREE.MeshLambertMaterial({ color: CURTAIN }),
            lamp: new THREE.MeshBasicMaterial({ color: 0xffc98a }),
            step: new THREE.MeshBasicMaterial({ color: 0x5a86ff }),
        });
        const front = -(radius + 0.35);                        // the front wall, just behind the screen
        const back = ROWS_BEHIND * ROW_GAP + 1.4;
        const half = Math.max(SEATS_PER_SIDE * SEAT_PITCH + 1.6, width / 2 + 2.2);
        const top = Math.max(floorY + 4.2, height / 2 + 1.2);
        // [Wolf 2026-11-06] the room's inside, in the screen group's frame (the e2e test checks a curved and a flat screen,
        // near and far, stand inside it)
        this.bounds = { front, back, half, floor: floorY, top };
        const add = (geo, mat, x, y, z) => { const m = new THREE.Mesh(geo, mat); m.position.set(x, y, z); this.room.add(m); return m; };

        // floor: flat at the front (the resident's row and the space before the screen), then a step per row behind
        add(new THREE.BoxGeometry(half * 2, 0.05, -front + 0.55), mats.carpet, 0, floorY - 0.025, (front + 0.55) / 2);
        for (let r = 1; r <= ROWS_BEHIND; r++) {
            const z0 = r * ROW_GAP - 0.55, y = floorY + r * ROW_RISE;
            add(new THREE.BoxGeometry(half * 2, ROW_RISE * r, ROW_GAP), mats.carpet, 0, floorY + (ROW_RISE * r) / 2, z0 + ROW_GAP / 2);
            // step lights along each riser's edge (the aisle glow of a dark cinema)
            add(new THREE.BoxGeometry(half * 2 - 0.4, 0.015, 0.015), mats.step, 0, y - 0.03, z0 - 0.005);
        }
        const roomD = back - front;
        // walls and ceiling
        add(new THREE.BoxGeometry(half * 2, top - floorY, 0.1), mats.wall, 0, (top + floorY) / 2, front - 0.05);          // front
        add(new THREE.BoxGeometry(half * 2, top - floorY, 0.1), mats.wall, 0, (top + floorY) / 2, back + 0.05);            // back
        for (const s of [-1, 1]) add(new THREE.BoxGeometry(0.1, top - floorY, roomD), mats.wall, s * (half + 0.05), (top + floorY) / 2, (front + back) / 2);
        add(new THREE.BoxGeometry(half * 2, 0.1, roomD), mats.ceil, 0, top + 0.05, (front + back) / 2);

        // curtains either side of the screen: a run of round folds from floor to the top of the screen and above
        const cTop = Math.min(top, height / 2 + 0.6), cH = cTop - floorY;
        const foldGeo = new THREE.CylinderGeometry(0.07, 0.07, cH, 10);
        for (const s of [-1, 1]) {
            const x0 = s * (width / 2 + 0.15);
            for (let k = 0; k < 9; k++) add(foldGeo, mats.curtain, x0 + s * k * 0.12, floorY + cH / 2, front + 0.12 + (k % 2) * 0.05);
        }
        // a pelmet across the top of the screen
        add(new THREE.BoxGeometry(width + 2.6, 0.35, 0.2), mats.curtain, 0, cTop + 0.1, front + 0.15);

        // side walls: tall fabric panels with a warm sconce on each, its light washing down the wall
        const lampGeo = new THREE.BoxGeometry(0.06, 0.18, 0.12);
        const panelGeo = new THREE.BoxGeometry(0.04, 2.6, 1.6);
        const washGeo = new THREE.PlaneGeometry(1.2, 1.8);
        for (const s of [-1, 1]) {
            for (let z = front + 1.5; z < back; z += 2.2) {
                add(panelGeo, mats.panel, s * (half - 0.03), floorY + 1.6, z);
                add(lampGeo, mats.lamp, s * (half - 0.07), floorY + 2.3, z);
                const w = add(washGeo, mats.wash, s * (half - 0.08), floorY + 1.55, z);
                w.rotation.y = -s * Math.PI / 2;
            }
        }

        // seats: the resident's row (none where they sit), and the rows behind, all instanced
        const spots = [];
        for (let r = 0; r <= ROWS_BEHIND; r++) {
            for (let i = -SEATS_PER_SIDE; i <= SEATS_PER_SIDE; i++) {
                if (r === 0 && i === 0) continue;   // the resident's own seat stays empty (they are in it)
                spots.push([i * SEAT_PITCH, floorY + r * ROW_RISE, r * ROW_GAP]);
            }
        }
        const parts = [
            [new THREE.BoxGeometry(0.5, 0.12, 0.48), mats.seat, 0, 0.44, 0],        // cushion
            [new THREE.BoxGeometry(0.52, 0.62, 0.1), mats.seat, 0, 0.78, 0.25],     // back
            [new THREE.BoxGeometry(0.06, 0.24, 0.5), mats.arm, -0.29, 0.6, 0.02],   // left arm
            [new THREE.BoxGeometry(0.06, 0.24, 0.5), mats.arm, 0.29, 0.6, 0.02],    // right arm
            [new THREE.BoxGeometry(0.4, 0.38, 0.06), mats.arm, 0, 0.19, 0.12],      // stand
        ];
        const m4 = new THREE.Matrix4();
        for (const [geo, mat, dx, dy, dz] of parts) {
            const inst = new THREE.InstancedMesh(geo, mat, spots.length);
            spots.forEach(([x, y, z], k) => { m4.makeTranslation(x + dx, y + dy, z + dz); inst.setMatrixAt(k, m4); });
            inst.instanceMatrix.needsUpdate = true;
            this.room.add(inst);
        }
    }

    setVisible(on) { this.group.visible = on; }
}
