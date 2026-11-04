/*
 * WolfViewer VR in the headset browser - the in-VR action bar (VR 3D Mode).
 * Copyright (C) 2026 IntelligentWolf Ltd. (LGPL 2.1, as the viewer)
 *
 * [VR CONTROLS 2026-11-04] Paul: "there's no controls in full 3D mode; in WolfStorm there are lots of controls that
 * work". This is WolfStorm's VR action bar (wolfstorm/js/vr/vr_hud.js VRHud: _curvedGeometry, _glass, _roundRect,
 * _layoutBar, _drawBar, _drawSlot, _drawFavourites, _dock, the constants), drawn the same way and docked the same way
 * (a cylinder section parented to the camera, identity rotation: rev 3/4 of vr_hud.js, "screen-locked, no keystone"),
 * with WolfStorm's action list (wolfstorm/js/ui/hud_model.js WsHudModel.ACTIONS: same order, labels, icons, groups).
 * The difference is what a button does: here it runs the WolfViewer toolbar command of the same name over the pipe
 * (wolfvrui.cpp), and floaters it opens appear on the UI layer (vrlayer.js). The bar's lit / greyed state comes back
 * from the viewer ("ui" message), so the bar and the desktop toolbar cannot disagree (hud_model.js state()).
 * Summoned, not permanent (vr_hud.js rev 4, Paul: "don't bring up the menu unless the user clicks the menu button").
 */
import * as THREE from './three/three.module.js';

// Source: hud_model.js ACTIONS (order, labels, icons, groups); `cmd` = commands.xml name (wolfvrui.cpp kCommands),
// `page` = done here on the headset page. `wolf` = Wolf Territories only (hud_model.js wolfOnly: removed elsewhere).
// `why` = shown greyed with this reason (hud_model.js vr:null), never left out, so the slots never move.
export const ACTIONS = [
    // communication
    { id: 'chat', label: 'Chat', icon: 'chat', cmd: 'chat' },
    { id: 'speak', label: 'Speak', icon: 'mic', cmd: 'speak', hint: 'Speak: on until you press it again' },
    { id: 'voice', label: 'Voice', icon: 'volume_up', cmd: 'voice' },
    { id: 'dictate', label: 'Dictate', icon: 'keyboard_voice', cmd: 'wolf_dictate' },
    { id: 'screen', label: 'Screen', icon: 'screen_share', why: 'Pick a window on the desktop first' },
    // movement / view
    { id: 'move', label: 'Move', icon: 'directions_walk', cmd: 'move' },
    { id: 'view', label: 'View', icon: 'videocam', cmd: 'view' },
    { id: 'fly', label: 'Fly', icon: 'flight', cmd: 'fly' },
    { id: 'sit', label: 'Sit', icon: 'airline_seat_recline_normal', cmd: 'sit', vrLabelSat: 'Stand' },
    // world
    { id: 'people', label: 'People', icon: 'people', cmd: 'people' },
    { id: 'radar', label: 'Radar', icon: 'radar', cmd: 'radar' },
    { id: 'search', label: 'Search', icon: 'search', cmd: 'search' },
    { id: 'map', label: 'Map', icon: 'public', cmd: 'map' },
    { id: 'minimap', label: 'Mini-Map', icon: 'map', cmd: 'minimap' },
    { id: 'gamemode', label: 'Game Mode', icon: 'sports_esports', cmd: 'wolf_gamemode', wolf: true },
    { id: 'experiences', label: 'Experiences', icon: 'explore', cmd: 'wolf_experiences', wolf: true },
    { id: 'home', label: 'Home', icon: 'home', cmd: 'home' },
    // self
    { id: 'inventory', label: 'Inventory', icon: 'inventory_2', cmd: 'inventory' },
    { id: 'appearance', label: 'Appearance', icon: 'accessibility_new', cmd: 'appearance' },
    { id: 'ao', label: 'AO', icon: 'directions_run', cmd: 'animationoverride' },
    { id: 'gestures', label: 'Gestures', icon: 'waving_hand', cmd: 'gestures' },
    // system
    { id: 'snapshot', label: 'Snapshot', icon: 'photo_camera', cmd: 'snapshot' },
    { id: 'quickprefs', label: 'Prefs', icon: 'tune', cmd: 'quickprefs' },
    { id: 'eep', label: 'EEP', icon: 'wb_sunny', cmd: 'wolf_eep' },
    { id: 'weather', label: 'Weather', icon: 'rainy', cmd: 'wolf_weather' },
    // Wolf vehicles: the ship's helm and the car's dashboard (commands.xml wolf_helm / wolf_dashboard)
    { id: 'helm', label: 'Helm', icon: 'sailing', cmd: 'wolf_helm', wolf: true },
    { id: 'dashboard', label: 'Dashboard', icon: 'speed', cmd: 'wolf_dashboard', wolf: true },
    // VR-only (hud_model.js vrOnly): the HUD layer switch (hud_model.js 'huds'), the big screen, leaving VR
    { id: 'huds', label: 'HUDs', icon: 'dashboard', page: 'huds' },
    // [VR VIEW 2026-11-04] see yourself from behind (the default) or from your own eyes with your avatar hidden
    { id: 'hideme', label: 'Hide Me', icon: 'visibility_off', cmd: 'hideavatar' },
    { id: 'bigscreen', label: 'Big Screen', icon: 'tv', page: 'bigscreen' },
    { id: 'numbers', label: 'Numbers', icon: 'speed', page: 'numbers', debug: true },   // only with WolfVRDebugStats
    { id: 'vr', label: 'Exit VR', icon: 'vrpano', page: 'exit' },
];

// vr_hud.js constants
const BAR_W = 1024, BAR_H = 448, SS = 2;
const DOCK_RADIUS = 1.0, BAR_ARC_DEG = 64, BAR_Y = -0.385;
const TAB_ARC_DEG = 9, TAB_Y = -0.52;     // [VR HUD CLICKS] the "Menu" tab: small, low, centred
const ACCENT = '#84cc16';
// [VR MENU FIX 2026-11-04] Paul on the Quest 3: "i click [the Menu tab] but then i cant turn off the menu". The bar could
// only be closed by the left controller's menu button, which the Quest Browser may keep for itself, and the bar had
// no close of its own. Now it has a big Close button (top right), and it closes by itself after a button has been
// used, so what was opened can be seen; the Menu tab then says what happened.
const CLOSE = { x: BAR_W - 196, y: 10, w: 182, h: 48 };

// vr_hud.js _curvedGeometry: u = 0 at the LEFT end, v = 0 at the BOTTOM, wound counter-clockwise from the eye.
export function curvedGeometry(radius, arcRad, height, segments) {
    const pos = [], uv = [], nrm = [], idx = [];
    const segs = Math.max(8, segments | 0);
    for (let i = 0; i <= segs; i++) {
        const u = i / segs;
        const a = -arcRad / 2 + u * arcRad;
        const sx = Math.sin(a), cz = Math.cos(a);
        const x = radius * sx, z = -radius * cz;
        for (let j = 0; j < 2; j++) {
            pos.push(x, j === 0 ? -height / 2 : height / 2, z);
            uv.push(u, j);
            nrm.push(-sx, 0, cz);
        }
    }
    for (let i = 0; i < segs; i++) {
        const a0 = i * 2, a1 = a0 + 1, b0 = a0 + 2, b1 = a0 + 3;
        idx.push(a0, b0, b1, a0, b1, a1);
    }
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
    g.setAttribute('uv', new THREE.Float32BufferAttribute(uv, 2));
    g.setAttribute('normal', new THREE.Float32BufferAttribute(nrm, 3));
    g.setIndex(idx);
    return g;
}

// A ray in HEAD space (origin, direction) against a cylinder section centred on the eye: radius R, arc (radians)
// centred on -Z turned `yaw` (radians, + = left as vr_hud.js _dock's rotation.y), centre height cy, height h.
// Returns {u, v (0 at the bottom), t} or null. The ray starts inside the cylinder, so the far root is the hit.
export function hitCylinder(o, d, R, arc, yaw, cy, h) {
    const a = d.x * d.x + d.z * d.z;
    if (a < 1e-9) return null;
    const b = 2 * (o.x * d.x + o.z * d.z), c = o.x * o.x + o.z * o.z - R * R;
    const disc = b * b - 4 * a * c;
    if (disc < 0) return null;
    const t = (-b + Math.sqrt(disc)) / (2 * a);
    if (t <= 0) return null;
    const px = o.x + d.x * t, py = o.y + d.y * t, pz = o.z + d.z * t;
    // angle measured like _curvedGeometry: a = atan2(x, -z), then undo the surface's own yaw
    let ang = Math.atan2(px, -pz) + yaw;
    const u = (ang + arc / 2) / arc, v = (py - (cy - h / 2)) / h;
    if (u < 0 || u > 1 || v < 0 || v > 1) return null;
    return { u, v, t };
}

export class VRBar {
    constructor(camera, send) {
        this.send = send;               // (obj) => void, the pipe
        this.open = false;
        this.hover = null;              // {kind:'action'|'fav', i}
        this.state = { on: new Set(), off: new Set(), wolf: false, region: '', sit: 0, fly: false, fav: [] };
        this.pageState = {};            // id -> bool, for the page's own actions (huds on, numbers on)
        this.onPage = null;             // (id) => void, the page's own actions
        this._toast = null;
        this._dirty = true;
        this._sig = '';
        const c = document.createElement('canvas');
        c.width = BAR_W * SS; c.height = BAR_H * SS;
        this.ctx = c.getContext('2d');
        this.ctx.scale(SS, SS);
        this.tex = new THREE.CanvasTexture(c);
        this.tex.colorSpace = THREE.SRGBColorSpace;
        this.tex.generateMipmaps = false;
        this.tex.minFilter = THREE.LinearFilter;
        this.arc = BAR_ARC_DEG * Math.PI / 180;
        this.height = (DOCK_RADIUS * this.arc) * (BAR_H / BAR_W);
        this.mesh = new THREE.Mesh(curvedGeometry(DOCK_RADIUS, this.arc, this.height, Math.round(BAR_ARC_DEG / 2)),
            new THREE.MeshBasicMaterial({ map: this.tex, transparent: true, depthTest: false }));
        this.mesh.name = 'WolfVRBar';
        this.mesh.frustumCulled = false;
        this.mesh.renderOrder = 9000;
        this.mesh.visible = false;
        this.mesh.position.set(0, BAR_Y, 0);   // vr_hud.js _dock(bar, 0, BAR_Y)
        camera.add(this.mesh);
        // [VR HUD CLICKS 2026-11-04] Paul on the Quest 3: "the VR HUD - nothing I clicked worked". A small "Menu" tab
        // always shows low in front of you in VR 3D Mode while the bar is closed: point at it and pull the trigger
        // (the left menu button does the same). Nobody has to find a button they cannot see.
        const tc = document.createElement('canvas');
        tc.width = 256; tc.height = 72;
        this.tabCtx = tc.getContext('2d');
        this.tabTex = new THREE.CanvasTexture(tc);
        this.tabTex.colorSpace = THREE.SRGBColorSpace;
        this.tabArc = TAB_ARC_DEG * Math.PI / 180;
        this.tabH = DOCK_RADIUS * this.tabArc * (72 / 256);
        this.tab = new THREE.Mesh(curvedGeometry(DOCK_RADIUS, this.tabArc, this.tabH, 8),
            new THREE.MeshBasicMaterial({ map: this.tabTex, transparent: true, depthTest: false }));
        this.tab.name = 'WolfVRBarTab';
        this.tab.frustumCulled = false;
        this.tab.renderOrder = 9000;
        this.tab.visible = false;
        this.tab.position.set(0, TAB_Y, 0);
        camera.add(this.tab);
        this.tabActive = false;      // VR 3D Mode and presenting (wolfvr.js)
        this.tabHover = false;
        this.flashUntil = 0;         // a press landed: the slot (or the tab) lights up briefly
        this.flashKey = '';
        this.tabMsg = '';            // [VR MENU FIX] what the last button did, shown on the tab for a few seconds
        this.tabMsgUntil = 0;
        this.pending = null;         // [VR MENU FIX] the command sent, waiting for the viewer's "acted"
        this._drawTab();
        this._iconOk = false;
        try {
            document.fonts.load('48px "Material Icons"', 'map').then(() => {
                this._iconOk = document.fonts.check('48px "Material Icons"');
                this._dirty = true;
            }).catch(() => {});
        } catch (e) { /* no FontFaceSet: two-letter fallback (vr_hud.js _icon) */ }
        this._layout();
    }

    actions() { return ACTIONS.filter((a) => (!a.wolf || this.state.wolf) && (!a.debug || this.debug)); }
    setDebug(on) { if (this.debug !== on) { this.debug = on; this._layout(); } }

    // vr_hud.js _layoutBar: 12 columns, rows derived, a fixed grid band that leaves room for the favourites strip
    _layout() {
        const list = this.actions();
        const COLS = 12, ROWS = Math.max(2, Math.ceil(list.length / COLS));
        const gridTop = 62, gridH = 196, cellW = BAR_W / COLS, cellH = gridH / ROWS;
        this.slots = list.map((a, i) => ({ x: (i % COLS) * cellW, y: gridTop + Math.floor(i / COLS) * cellH, w: cellW, h: cellH, action: a }));
        this.favSlots = [];
        this._dirty = true;
    }

    setState(m) {
        const wolf = !!m.wolf;
        this.state = { on: new Set(m.on || []), off: new Set(m.off || []), wolf, region: String(m.region || ''),
                       sit: m.sit | 0, fly: !!m.fly, fav: Array.isArray(m.fav) ? m.fav.slice(0, 6) : [], hide: !!m.hide };
        if (this.slots.length !== this.actions().length) this._layout();
        this._dirty = true;
    }
    setPageState(id, on) { if (this.pageState[id] !== on) { this.pageState[id] = on; this._dirty = true; } }

    toggle() { this.open = !this.open; this.mesh.visible = this.open; this.hover = null; this._dirty = true; this._showTab(); }
    // [VR MENU FIX] a few words on the Menu tab (what the last button did), for `ms`
    tabSay(text, ms = 4000) { this.tabMsg = text; this.tabMsgUntil = performance.now() + ms; this._drawTab(); }
    // [VR MENU FIX] the viewer's answer to a command: done (the bar is already closed: say so on the tab) or not done
    // (the bar opens again with the reason, which stays until the next press - a failure is never only flashed)
    onActed(m) {
        const a = ACTIONS.find((x) => x.cmd === m.a);
        const label = a ? this.labelFor(a) : String(m.a || '');
        const mine = this.pending && this.pending.cmd === m.a;
        if (mine) this.pending = null;
        if (m.ok) { if (mine) this.tabSay(label + ' \u2713'); return; }   // only the bar's own presses are confirmed here
        const why = String(m.why || 'it cannot be used here just now');
        if (!this.open) this.toggle();
        this.toast(label + ': ' + why, 600000);
    }
    hide() { this.open = false; this.mesh.visible = false; this.hover = null; this._showTab(); }
    setTabActive(on) { if (this.tabActive !== on) { this.tabActive = on; this._showTab(); } }
    _showTab() { this.tab.visible = this.tabActive && !this.open; }
    _drawTab() {
        const ctx = this.tabCtx, W = 256, H = 72;
        ctx.clearRect(0, 0, W, H);
        const now = performance.now();
        const lit = this.tabHover || now < this.flashUntil;
        const msg = now < this.tabMsgUntil ? this.tabMsg : '';
        this._roundRect(ctx, 4, 4, W - 8, H - 8, 22);
        ctx.fillStyle = lit ? 'rgba(132, 204, 22, 0.55)' : 'rgba(15, 23, 42, 0.82)';
        ctx.fill();
        ctx.strokeStyle = lit ? 'rgba(220, 255, 160, 0.95)' : 'rgba(132, 204, 22, 0.6)';
        ctx.lineWidth = 3;
        ctx.stroke();
        ctx.fillStyle = '#ffffff';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.font = msg ? 'bold 24px sans-serif' : 'bold 30px sans-serif';
        ctx.fillText(msg ? msg.slice(0, 18) : 'Menu', W / 2, H / 2 + 1);
        this.tabTex.needsUpdate = true;
    }
    // a press landed on `key` (a slot, a favourite or the tab): light it for a moment
    flash(key) { this.flashKey = key; this.flashUntil = performance.now() + 220; this._dirty = true; if (key === 'tab') this._drawTab(); }

    toast(text, ms = 3000) { this._toast = { text, until: performance.now() + ms }; this._dirty = true; }

    // state of one slot (hud_model.js state(): active / enabled / available)
    slotState(a) {
        if (a.page) return { active: !!this.pageState[a.page], dead: false };
        if (a.why) return { active: false, dead: true };
        if (a.cmd === 'sit') return { active: this.state.sit > 0, dead: false };
        if (a.cmd === 'home') return { active: false, dead: false };
        if (a.cmd === 'hideavatar') return { active: !!this.state.hide, dead: false };
        return { active: this.state.on.has(a.cmd), dead: this.state.off.has(a.cmd) };
    }

    update(now) {
        if (this._toast && now > this._toast.until) { this._toast = null; this._dirty = true; }
        if (this.tabMsg && now > this.tabMsgUntil) { this.tabMsg = ''; this._drawTab(); }
        if (this.flashKey && now > this.flashUntil) { const k = this.flashKey; this.flashKey = ''; this._dirty = true; if (k === 'tab') this._drawTab(); }
        if (this.open && this._dirty) this._draw();
    }

    // Head-space ray -> what it points at on the bar, or null. vr_hud.js resolve(): canvas px from the UV.
    hit(o, d) {
        if (!this.open) {
            if (!this.tab.visible) return null;
            const t = hitCylinder(o, d, DOCK_RADIUS, this.tabArc, 0, TAB_Y, this.tabH);
            return t ? { kind: 'tab', i: -1, t: t.t } : null;
        }
        const h = hitCylinder(o, d, DOCK_RADIUS, this.arc, 0, BAR_Y, this.height);
        if (!h) return null;
        const px = h.u * BAR_W, py = (1 - h.v) * BAR_H;
        if (px >= CLOSE.x && px < CLOSE.x + CLOSE.w && py >= CLOSE.y && py < CLOSE.y + CLOSE.h) return { kind: 'close', i: -1, t: h.t };
        for (let i = 0; i < this.slots.length; i++) {
            const s = this.slots[i];
            if (px >= s.x && px < s.x + s.w && py >= s.y && py < s.y + s.h) return { kind: 'action', i, t: h.t };
        }
        for (let i = 0; i < this.favSlots.length; i++) {
            const s = this.favSlots[i];
            if (px >= s.x && px < s.x + s.w && py >= s.y && py < s.y + s.h) return { kind: 'fav', i, t: h.t };
        }
        return { kind: 'bar', i: -1, t: h.t };   // on the bar, between buttons: still the bar's, not the world's
    }

    setHover(target) {
        const th = !!(target && target.kind === 'tab');
        if (th !== this.tabHover) { this.tabHover = th; this._drawTab(); }
        if (th) target = null;
        const k = target && target.kind !== 'bar' ? target.kind + target.i : '';
        const was = this.hover ? this.hover.kind + this.hover.i : '';
        if (k !== was) { this.hover = target && target.kind !== 'bar' ? target : null; this._dirty = true; }
    }

    // vr_hud.js activate()
    activate(target) {
        if (!target) return;
        if (target.kind === 'tab') { this.flash('tab'); this.toggle(); return; }
        if (target.kind === 'close') { this.hide(); return; }
        if (this._toast && this._toast.until - performance.now() > 60000) this._toast = null;   // a reason shown: read, now gone
        if (target.kind === 'action' || target.kind === 'fav') this.flash(target.kind + target.i);
        if (target.kind === 'fav') {
            const f = this.favSlots[target.i] && this.favSlots[target.i].fav;
            if (f) { this.send({ t: 'act', a: 'fav', x: f.id }); this.toast('Teleporting to ' + f.n); }
            return;
        }
        if (target.kind !== 'action') return;
        const a = this.slots[target.i] && this.slots[target.i].action;
        if (!a) return;
        const st = this.slotState(a);
        if (a.why || st.dead) { this.toast(a.why || (a.label + ' cannot be used here just now')); return; }
        if (a.page) { if (this.onPage) this.onPage(a.page); return; }
        this.send({ t: 'act', a: a.cmd });
        if (this.onCommand) this.onCommand(a);
        // [VR MENU FIX] out of the way at once, so the window it opens is seen; the tab says it is on its way
        this.pending = { cmd: a.cmd, at: performance.now() };
        this.hide();
        this.tabSay(this.labelFor(a) + '\u2026', 6000);
    }

    labelFor(a) {
        if (a.cmd === 'sit' && this.state.sit > 0) return 'Stand';
        if (a.cmd === 'hideavatar' && this.state.hide) return 'Show Me';
        return a.label;
    }

    // ---- drawing: vr_hud.js _glass / _roundRect / _drawBar / _drawSlot / _drawFavourites ----
    _roundRect(ctx, x, y, w, h, r) {
        ctx.beginPath();
        ctx.moveTo(x + r, y);
        ctx.arcTo(x + w, y, x + w, y + h, r);
        ctx.arcTo(x + w, y + h, x, y + h, r);
        ctx.arcTo(x, y + h, x, y, r);
        ctx.arcTo(x, y, x + w, y, r);
        ctx.closePath();
    }
    _glass(ctx, x, y, w, h, r, opts = {}) {
        const a = opts.alpha ?? 1;
        this._roundRect(ctx, x, y, w, h, r);
        const g = ctx.createLinearGradient(0, y, 0, y + h);
        g.addColorStop(0, `rgba(30, 41, 59, ${0.72 * a})`);
        g.addColorStop(0.45, `rgba(21, 31, 50, ${0.78 * a})`);
        g.addColorStop(1, `rgba(15, 23, 42, ${0.84 * a})`);
        ctx.fillStyle = g;
        ctx.fill();
        ctx.save();
        this._roundRect(ctx, x, y, w, h, r);
        ctx.clip();
        const sh = ctx.createLinearGradient(0, y, 0, y + h * 0.35);
        sh.addColorStop(0, `rgba(255, 255, 255, ${0.14 * a})`);
        sh.addColorStop(1, 'rgba(255, 255, 255, 0)');
        ctx.fillStyle = sh;
        ctx.fillRect(x, y, w, h * 0.35);
        ctx.restore();
        this._roundRect(ctx, x, y, w, h, r);
        ctx.strokeStyle = `rgba(132, 204, 22, ${0.34 * a})`;
        ctx.lineWidth = opts.rim ?? 2;
        ctx.stroke();
    }
    _icon(ctx, name, x, y, px) {
        if (this._iconOk) { ctx.font = `${px}px "Material Icons"`; ctx.fillText(name, x, y); }
        else { ctx.font = `bold ${Math.round(px * 0.5)}px sans-serif`; ctx.fillText(name.slice(0, 2).toUpperCase(), x, y); }
    }
    _draw() {
        this._dirty = false;
        const ctx = this.ctx, W = BAR_W, H = BAR_H;
        ctx.clearRect(0, 0, W, H);
        this._glass(ctx, 4, 4, W - 8, H - 8, 26, { rim: 3 });
        ctx.textAlign = 'left';
        ctx.textBaseline = 'middle';
        ctx.fillStyle = ACCENT;
        ctx.font = 'bold 26px sans-serif';
        ctx.fillText('WOLFVIEWER', 26, 34);
        if (this.state.region) {
            ctx.textAlign = 'right';
            ctx.fillStyle = '#7f93b5';
            ctx.font = '24px sans-serif';
            ctx.fillText(this.state.region.slice(0, 28), CLOSE.x - 18, 34);
        }
        // [VR MENU FIX] Close: big, always in the same place, lit while pointed at
        const ch = this.hover && this.hover.kind === 'close';
        this._roundRect(ctx, CLOSE.x, CLOSE.y, CLOSE.w, CLOSE.h, 14);
        ctx.fillStyle = ch ? 'rgba(132, 204, 22, 0.55)' : 'rgba(255, 255, 255, 0.10)';
        ctx.fill();
        ctx.strokeStyle = ch ? 'rgba(220, 255, 160, 0.95)' : 'rgba(226, 240, 255, 0.55)';
        ctx.lineWidth = 2;
        ctx.stroke();
        ctx.textAlign = 'center';
        ctx.fillStyle = '#ffffff';
        ctx.font = 'bold 26px sans-serif';
        ctx.fillText('\u2715  Close', CLOSE.x + CLOSE.w / 2, CLOSE.y + CLOSE.h / 2 + 1);
        for (let i = 0; i < this.slots.length; i++) this._drawSlot(ctx, this.slots[i], i);
        this._drawFavourites(ctx);
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        if (this._toast) {
            ctx.fillStyle = '#ffd08a';
            ctx.font = 'bold 24px sans-serif';
            ctx.fillText(this._toast.text.slice(0, 70), W / 2, H - 26);
        } else {
            let hint = 'Point at a button and pull the trigger \u00b7 Close is top right';
            const h = this.hover;
            if (h && h.kind === 'action') {
                const a = this.slots[h.i] && this.slots[h.i].action;
                if (a) hint = a.hint || a.why || this.labelFor(a);
            }
            ctx.fillStyle = '#7f93b5';
            ctx.font = '22px sans-serif';
            ctx.fillText(hint, W / 2, H - 26);
        }
        this.tex.needsUpdate = true;
    }
    _drawSlot(ctx, slot, i) {
        const a = slot.action;
        const st = this.slotState(a);
        const hovered = this.hover && this.hover.kind === 'action' && this.hover.i === i;
        const flashing = this.flashKey === 'action' + i;
        const pad = 6;
        const x = slot.x + pad, y = slot.y + pad, w = slot.w - pad * 2, h = slot.h - pad * 2;
        this._roundRect(ctx, x, y, w, h, 12);
        if (flashing) {
            ctx.fillStyle = 'rgba(220, 255, 160, 0.85)';   // the press landed
        } else if (st.active) {
            const g = ctx.createLinearGradient(0, y, 0, y + h);
            g.addColorStop(0, 'rgba(132, 204, 22, 0.52)');
            g.addColorStop(1, 'rgba(101, 163, 13, 0.36)');
            ctx.fillStyle = g;
        } else if (hovered) {
            ctx.fillStyle = 'rgba(132, 204, 22, 0.20)';
        } else {
            ctx.fillStyle = 'rgba(255, 255, 255, 0.055)';
        }
        ctx.fill();
        if (st.active || hovered) {
            ctx.strokeStyle = st.active ? 'rgba(178, 240, 90, 0.95)' : 'rgba(132, 204, 22, 0.55)';
            ctx.lineWidth = 2;
            ctx.stroke();
        }
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillStyle = st.dead ? 'rgba(200, 214, 236, 0.26)' : (st.active ? '#ffffff' : 'rgba(226, 240, 255, 0.94)');
        // three rows of slots are shorter than WolfStorm's two: the glyph shrinks to fit, the label stays 17 px
        const glyph = Math.min(42, Math.round(h * 0.55));
        this._icon(ctx, a.icon, x + w / 2, y + h / 2 - 10, glyph);
        ctx.font = '17px sans-serif';
        const label = this.labelFor(a);
        ctx.fillText(label.length > 11 ? label.slice(0, 10) + '…' : label, x + w / 2, y + h - 11);
    }
    _drawFavourites(ctx) {
        const favs = this.state.fav;
        const W = BAR_W, stripY = 286;
        ctx.textAlign = 'left';
        ctx.textBaseline = 'middle';
        ctx.fillStyle = '#7f93b5';
        ctx.font = 'bold 20px sans-serif';
        ctx.fillText('FAVOURITES', 26, stripY + 4);
        this.favSlots = [];
        if (!favs.length) {
            ctx.fillStyle = 'rgba(200, 214, 236, 0.35)';
            ctx.font = '20px sans-serif';
            ctx.fillText('No landmarks in your favourites bar', 170, stripY + 4);
            return;
        }
        let x = 170;
        const h = 46, y = stripY - 19;
        for (let i = 0; i < favs.length; i++) {
            const name = String(favs[i].n || 'Landmark');
            ctx.font = '20px sans-serif';
            const w = Math.min(190, ctx.measureText(name).width + 34);
            if (x + w > W - 26) break;
            const hovered = (this.hover && this.hover.kind === 'fav' && this.hover.i === i) || this.flashKey === 'fav' + i;
            this._roundRect(ctx, x, y, w, h, 8);
            ctx.fillStyle = hovered ? 'rgba(132, 204, 22, 0.30)' : 'rgba(255, 255, 255, 0.06)';
            ctx.fill();
            ctx.strokeStyle = hovered ? 'rgba(132, 204, 22, 0.45)' : 'rgba(255, 255, 255, 0.12)';
            ctx.lineWidth = 1.5;
            ctx.stroke();
            ctx.fillStyle = hovered ? '#ffffff' : '#cfd8ea';
            ctx.textAlign = 'center';
            const fit = ctx.measureText(name).width > w - 20 ? name.slice(0, Math.max(3, Math.floor((w - 20) / 10))) + '…' : name;
            ctx.fillText(fit, x + w / 2, y + h / 2);
            ctx.textAlign = 'left';
            this.favSlots.push({ x, y, w, h, fav: favs[i] });
            x += w + 10;
        }
    }
}
