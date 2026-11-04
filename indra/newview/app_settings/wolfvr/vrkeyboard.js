/*
 * WolfViewer VR in the headset browser - the headset keyboard (VR 3D Mode).
 * Copyright (C) 2026 IntelligentWolf Ltd. (LGPL 2.1, as the viewer)
 *
 * [VR KEYBOARD 2026-11-04] Paul: "NO KEYBOARD" - and "keys need to match the country the user is in". A keyboard drawn
 * by this page, standing in the room like the windows (world-locked, in front of where you look when it opens), with
 * big keys for older and disabled residents. It opens by itself when the viewer's typing goes to a text field (the
 * viewer's "ui" message says "typing") and with the Keyboard button on the bar; its Close key closes it.
 * Every key sends what it MEANS on that layout - a typed character ({"t":"in","e":"text"}, the viewer's
 * handleUnicodeChar), never a physical key code - and Backspace, Enter, Tab and the arrows as the viewer's own keys.
 * The layout follows the headset's own language (navigator.languages), then the viewer's language, else US with an
 * accents page; the Layout key changes it and the choice is remembered in this browser.
 */
import * as THREE from './three/three.module.js';
import { curvedGeometry, hitCylinder } from './vrbar.js';

// Letter rows per layout (lower case; Shift gives upper case). Sources: the national layouts' letter positions
// (UK/US QWERTY, German QWERTZ DIN 2137, French AZERTY NF Z71-300's base layer, Spanish/Italian/Nordic QWERTY with
// their extra letters, Belgian AZERTY). Punctuation lives on the symbols page, which each layout fills with its own
// currency first (UK £, euro countries €, US $).
export const LAYOUTS = {
    gb: { label: 'UK', rows: ['qwertyuiop', 'asdfghjkl', 'zxcvbnm'], money: '£€$', extra: "'" },
    us: { label: 'US', rows: ['qwertyuiop', 'asdfghjkl', 'zxcvbnm'], money: '$€£', extra: "'" },
    de: { label: 'Deutsch', rows: ['qwertzuiopü', 'asdfghjklöä', 'yxcvbnmß'], money: '€$£', extra: '' },
    fr: { label: 'Français', rows: ['azertyuiop', 'qsdfghjklm', 'wxcvbnéèàç'], money: '€$£', extra: 'ù' },
    be: { label: 'Belgique', rows: ['azertyuiop', 'qsdfghjklm', 'wxcvbnéèàç'], money: '€$£', extra: 'ù' },
    es: { label: 'Español', rows: ['qwertyuiop', 'asdfghjklñ', 'zxcvbnmç'], money: '€$£', extra: '¿¡' },
    it: { label: 'Italiano', rows: ['qwertyuiop', 'asdfghjkl', 'zxcvbnmèéòàùì'], money: '€$£', extra: '' },
    da: { label: 'Dansk', rows: ['qwertyuiopå', 'asdfghjklæø', 'zxcvbnm'], money: '€$£', extra: '' },
    no: { label: 'Norsk', rows: ['qwertyuiopå', 'asdfghjkløæ', 'zxcvbnm'], money: '€$£', extra: '' },
    sv: { label: 'Svenska', rows: ['qwertyuiopå', 'asdfghjklöä', 'zxcvbnm'], money: '€$£', extra: '' },
    nl: { label: 'Nederlands', rows: ['qwertyuiop', 'asdfghjkl', 'zxcvbnm'], money: '€$£', extra: "'" },
};
const ORDER = ['gb', 'us', 'de', 'fr', 'be', 'es', 'it', 'da', 'no', 'sv', 'nl'];
const SYMBOLS = ['1234567890', '@#&*-+=()/', '!?,.:;"%_'];
// The accents page: for any language without its own layout here (and for everyone: a long word in another language)
const ACCENTS = ['àáâäãåæ', 'èéêëìíîï', 'òóôöõøùúûü', 'çñßýÿœ'];

// BCP 47 tag -> layout: language, then the region where it matters (en-GB vs en-US, fr-BE, nl-BE, nb/nn = Norwegian)
export function layoutFor(tag) {
    const t = String(tag || '').toLowerCase();
    const [lang, region] = t.split(/[-_]/);
    if (lang === 'en') return region === 'us' || region === 'ca' || region === 'ph' ? 'us' : region ? 'gb' : 'gb';
    if (lang === 'fr') return region === 'be' ? 'be' : 'fr';
    if (lang === 'nl') return region === 'be' ? 'be' : 'nl';
    if (lang === 'nb' || lang === 'nn' || lang === 'no') return 'no';
    if (lang === 'da') return 'da';
    if (lang === 'sv' || lang === 'fi') return 'sv';
    if (LAYOUTS[lang]) return lang;
    return '';
}
const KEY = 'wolfvr.keyboard';
const W = 1280, H = 520, SS = 2;   // canvas, CSS px (drawn at 2x)
const RADIUS = 0.95, ARC_DEG = 64;

export class VRKeyboard {
    constructor(world, send) {
        this.world = world;
        this.send = send;
        this.open = false;
        this.page = 'letters';      // letters | symbols | accents
        this.shift = false;
        this.hover = -1;
        this.flash = -1; this.flashUntil = 0;
        this.viewerLang = '';
        this.chosen = '';
        try { this.chosen = localStorage.getItem(KEY) || ''; } catch (e) { /* not remembered */ }
        this.layout = this._pick();
        const c = document.createElement('canvas');
        c.width = W * SS; c.height = H * SS;
        this.ctx = c.getContext('2d');
        this.ctx.scale(SS, SS);
        this.tex = new THREE.CanvasTexture(c);
        this.tex.colorSpace = THREE.SRGBColorSpace;
        this.tex.generateMipmaps = false;
        this.tex.minFilter = THREE.LinearFilter;
        this.arc = ARC_DEG * Math.PI / 180;
        this.height = RADIUS * this.arc * (H / W);
        this.mesh = new THREE.Mesh(curvedGeometry(RADIUS, this.arc, this.height, 32),
            new THREE.MeshBasicMaterial({ map: this.tex, transparent: true, depthTest: false }));
        this.mesh.name = 'WolfVRKeyboard';
        this.mesh.frustumCulled = false;
        this.mesh.renderOrder = 9100;   // over the windows and the bar: it is what the resident is using now
        this.mesh.visible = false;
        world.add(this.mesh);
        this.c = new THREE.Vector3();
        this.yaw = 0;
        this.cy = 0;
        this._layoutKeys();
    }

    _pick() {
        if (this.chosen && LAYOUTS[this.chosen]) return this.chosen;
        const langs = (navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language]) || [];
        for (const l of langs) { const k = layoutFor(l); if (k) return k; }
        return layoutFor(this.viewerLang) || 'us';
    }
    setViewerLanguage(lang) {
        if (lang === this.viewerLang) return;
        this.viewerLang = String(lang || '');
        if (!this.chosen) { this.layout = this._pick(); this._layoutKeys(); }
    }
    setLayout(k) {
        if (!LAYOUTS[k]) return;
        this.layout = k; this.chosen = k;
        try { localStorage.setItem(KEY, k); } catch (e) { /* not remembered */ }
        this._layoutKeys();
    }

    // the keys of the page shown now: {x, y, w, h, label, send: {text} | {key} | {act}}
    _layoutKeys() {
        const L = LAYOUTS[this.layout];
        const rows = this.page === 'symbols' ? [SYMBOLS[0], SYMBOLS[1], L.money + SYMBOLS[2]]
                   : this.page === 'accents' ? ACCENTS
                   : L.rows.map((r, i) => (i === 2 && L.extra ? r + L.extra : r));
        const keys = [];
        const top = 74, rowH = 92, gap = 8;
        rows.forEach((r, j) => {
            const chars = Array.from(r);
            const n = Math.max(10, chars.length);
            const kw = (W - 40) / (n + (j === 2 ? 3 : 0)) ;
            let x = 20 + (j === 2 ? kw * 1.5 : (n - chars.length) * kw / 2);
            for (const ch of chars) {
                const out = this.page === 'letters' && this.shift ? ch.toLocaleUpperCase() : ch;
                keys.push({ x: x + gap / 2, y: top + j * rowH + gap / 2, w: kw - gap, h: rowH - gap, label: out, send: { text: out } });
                x += kw;
            }
            if (j === 2) {
                keys.push({ x: 20 + gap / 2, y: top + j * rowH + gap / 2, w: kw * 1.5 - gap, h: rowH - gap, label: '⇧ Shift', send: { act: 'shift' }, lit: this.shift });
                keys.push({ x: x + gap / 2, y: top + j * rowH + gap / 2, w: W - 20 - x - gap, h: rowH - gap, label: '⌫', send: { key: 'Backspace' } });
            }
        });
        const y = top + 3 * rowH + gap / 2, h = rowH - gap;
        const bottom = [
            [130, this.page === 'letters' ? '?123' : 'ABC', { act: this.page === 'letters' ? 'symbols' : 'letters' }],
            [130, 'àéü', { act: 'accents' }],
            [150, L.label, { act: 'layout' }],
            [430, 'Space', { text: ' ' }],
            [80, '←', { key: 'ArrowLeft' }],
            [80, '→', { key: 'ArrowRight' }],
            [210, 'Enter ↵', { key: 'Enter' }],
        ];
        let x = 20;
        const total = bottom.reduce((a, b) => a + b[0], 0), scale = (W - 40) / total;
        for (const [w, label, send] of bottom) {
            keys.push({ x: x + gap / 2, y, w: w * scale - gap, h, label, send });
            x += w * scale;
        }
        // the Close key: big, top right, always in the same place (as the bar's)
        keys.push({ x: W - 200, y: 10, w: 186, h: 54, label: '✕  Close', send: { act: 'close' }, close: true });
        this.keys = keys;
        this._dirty = true;
    }

    show(headPos, headQuat) {
        // in front of where the resident looks, a little below the eyes, then it stays put (as the windows do)
        const f = new THREE.Vector3(0, 0, -1).applyQuaternion(headQuat);
        this.yaw = Math.atan2(-f.x, -f.z);
        this.c.copy(headPos);
        this.cy = -0.42;
        this.mesh.position.set(this.c.x, this.c.y + this.cy, this.c.z);
        this.mesh.rotation.set(0, this.yaw, 0);
        this.open = true;
        this.mesh.visible = true;
        this._dirty = true;
    }
    hide() { this.open = false; this.mesh.visible = false; this.hover = -1; }

    // world ray -> {i (key), t} or null
    hit(o, d) {
        if (!this.open) return null;
        const lo = new THREE.Vector3().copy(o).sub(this.c);
        const h = hitCylinder(lo, d, RADIUS, this.arc, this.yaw, this.cy, this.height);
        if (!h) return null;
        const px = h.u * W, py = (1 - h.v) * H;
        const i = this.keys.findIndex((k) => px >= k.x && px < k.x + k.w && py >= k.y && py < k.y + k.h);
        return { i, t: h.t };
    }
    setHover(i) { if (i !== this.hover) { this.hover = i; this._dirty = true; } }

    // a press on key i: what it means on this layout goes to the viewer
    press(i) {
        const k = this.keys[i];
        if (!k) return;
        this.flash = i; this.flashUntil = performance.now() + 180; this._dirty = true;
        const s = k.send;
        if (s.text) {
            this.send({ t: 'in', e: 'text', s: s.text });
            if (this.shift && this.page === 'letters') { this.shift = false; this._layoutKeys(); }   // one capital, as a phone does
        } else if (s.key) {
            this.send({ t: 'in', e: 'kd', k: s.key, m: 0 });
            this.send({ t: 'in', e: 'ku', k: s.key, m: 0 });
        } else if (s.act === 'shift') { this.shift = !this.shift; this._layoutKeys(); }
        else if (s.act === 'symbols' || s.act === 'letters' || s.act === 'accents') { this.page = s.act === 'accents' && this.page === 'accents' ? 'letters' : s.act; this._layoutKeys(); }
        else if (s.act === 'layout') { this.setLayout(ORDER[(ORDER.indexOf(this.layout) + 1) % ORDER.length]); }
        else if (s.act === 'close') { this.hide(); }
    }
    // where a key is in the room (test hook): the key whose label is `label` (or index), its centre
    keyPoint(label) {
        const i = typeof label === 'number' ? label : this.keys.findIndex((k) => k.label === label);
        const k = this.keys[i];
        if (!k) return null;
        const u = (k.x + k.w / 2) / W, v = (k.y + k.h / 2) / H;
        const a = -this.arc / 2 + u * this.arc - this.yaw;
        return [this.c.x + RADIUS * Math.sin(a), this.c.y + this.cy + (0.5 - v) * this.height, this.c.z - RADIUS * Math.cos(a)];
    }

    update(now) {
        if (this.flash >= 0 && now > this.flashUntil) { this.flash = -1; this._dirty = true; }
        if (this.open && this._dirty) this._draw();
    }
    _round(ctx, x, y, w, h, r) {
        ctx.beginPath(); ctx.moveTo(x + r, y); ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r);
        ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath();
    }
    _draw() {
        this._dirty = false;
        const ctx = this.ctx;
        ctx.clearRect(0, 0, W, H);
        this._round(ctx, 4, 4, W - 8, H - 8, 26);
        ctx.fillStyle = 'rgba(15, 23, 42, 0.92)'; ctx.fill();
        ctx.strokeStyle = 'rgba(132, 204, 22, 0.6)'; ctx.lineWidth = 3; ctx.stroke();
        ctx.fillStyle = '#84cc16'; ctx.font = 'bold 26px sans-serif'; ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
        ctx.fillText('KEYBOARD', 26, 38);
        ctx.fillStyle = '#7f93b5'; ctx.font = '22px sans-serif';
        ctx.fillText(`${LAYOUTS[this.layout].label} layout · point and pull the trigger`, 190, 38);
        this.keys.forEach((k, i) => {
            const hot = i === this.hover, fl = i === this.flash;
            this._round(ctx, k.x, k.y, k.w, k.h, 12);
            ctx.fillStyle = fl ? 'rgba(220, 255, 160, 0.9)' : k.lit ? 'rgba(132, 204, 22, 0.55)' : hot ? 'rgba(132, 204, 22, 0.32)' : k.close ? 'rgba(255, 255, 255, 0.12)' : 'rgba(255, 255, 255, 0.08)';
            ctx.fill();
            if (hot || k.close) { ctx.strokeStyle = hot ? 'rgba(220, 255, 160, 0.95)' : 'rgba(226, 240, 255, 0.55)'; ctx.lineWidth = 2; ctx.stroke(); }
            ctx.fillStyle = fl ? '#0f172a' : '#ffffff';
            ctx.textAlign = 'center';
            ctx.font = `bold ${k.label.length > 2 ? 26 : 40}px sans-serif`;
            ctx.fillText(k.label, k.x + k.w / 2, k.y + k.h / 2 + 2);
        });
        this.tex.needsUpdate = true;
    }
}
