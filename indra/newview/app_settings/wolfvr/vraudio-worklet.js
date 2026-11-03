/*
 * WolfViewer VR in the headset browser - the sound worklets (vraudio.js): the player's ring buffer and the microphone.
 * Copyright (C) 2026 IntelligentWolf Ltd. (LGPL 2.1, as the viewer)
 * A file of its own, not a Blob URL: the page's Content-Security-Policy (wolfvrpipe.cpp) allows scripts from 'self' only.
 */

class WvrPlayer extends AudioWorkletProcessor {
    constructor() {
        super();
        this.inRate = 48000;
        this.cap = 48000 * 2;
        this.L = new Float32Array(this.cap);
        this.R = new Float32Array(this.cap);
        this.w = 0;              // frames written (input rate)
        this.r = 0;              // read position (input frames, fractional)
        this.meta = [];          // { at: frame index, mixMs } for the delay readout
        this.primed = false;
        this.underruns = 0;
        this.jitterMs = 40;
        this.syncMs = 0;
        this.calmFrames = 0;     // output frames since the last gap
        this.lastStats = 0;
        this.port.onmessage = (e) => {
            const d = e.data;
            if (d.pcm) this.push(d);
            else if (typeof d.syncMs === 'number') this.syncMs = Math.max(0, Math.min(150, d.syncMs));
            else if (d.reset) { this.w = 0; this.r = 0; this.meta = []; this.primed = false; }
        };
    }
    targetFrames() { return Math.max(this.jitterMs, this.syncMs) * this.inRate / 1000; }
    push(d) {
        const pcm = new Int16Array(d.pcm);
        const ch = d.channels;
        const frames = Math.floor(pcm.length / ch);
        if (d.rate !== this.inRate) { this.inRate = d.rate; this.w = 0; this.r = 0; this.meta = []; this.primed = false; }
        if (this.w - this.r + frames > this.cap) return;   // full (never in practice: see the drop below)
        for (let i = 0; i < frames; i++) {
            const k = (this.w + i) % this.cap;
            const l = pcm[i * ch] / 32768;
            this.L[k] = l;
            this.R[k] = ch > 1 ? pcm[i * ch + 1] / 32768 : l;
        }
        this.meta.push({ at: this.w, mixMs: d.mixMs });
        if (this.meta.length > 64) this.meta.shift();
        this.w += frames;
        // far too much in hand (a burst after a network stall): drop the oldest at once, keep the target
        const target = this.targetFrames();
        if (this.w - this.r > target + 0.06 * this.inRate) this.r = this.w - target;
    }
    process(inputs, outputs) {
        const out = outputs[0];
        const oL = out[0], oR = out[1] || out[0];
        const n = oL.length;
        const target = this.targetFrames();
        let have = this.w - this.r;
        if (!this.primed) {
            if (have >= target) this.primed = true;
            else { oL.fill(0); if (oR !== oL) oR.fill(0); this.stats(have); return true; }
        }
        // gentle catch-up / slow-down around the target: at most 0.5% faster or slower
        const err = (have - target) / Math.max(1, target);
        const step = (this.inRate / sampleRate) * (1 + Math.max(-0.005, Math.min(0.005, err * 0.01)));
        for (let i = 0; i < n; i++) {
            if (this.w - this.r < 2) {
                this.primed = false;
                this.underruns++;
                this.calmFrames = 0;
                this.jitterMs = Math.min(120, this.jitterMs + 10);
                for (let j = i; j < n; j++) { oL[j] = 0; if (oR !== oL) oR[j] = 0; }
                break;
            }
            const idx = Math.floor(this.r);
            const f = this.r - idx;
            const a = idx % this.cap, b = (idx + 1) % this.cap;
            oL[i] = this.L[a] + (this.L[b] - this.L[a]) * f;
            if (oR !== oL) oR[i] = this.R[a] + (this.R[b] - this.R[a]) * f;
            this.r += step;
        }
        this.calmFrames += n;
        if (this.calmFrames > sampleRate * 10 && this.jitterMs > 40) { this.jitterMs -= 5; this.calmFrames = 0; }
        this.stats(this.w - this.r);
        return true;
    }
    stats(have) {
        if (currentTime - this.lastStats < 0.25) return;
        this.lastStats = currentTime;
        while (this.meta.length > 1 && this.meta[1].at <= this.r) this.meta.shift();
        const m = this.meta[0];
        const headMixMs = m && m.at <= this.r ? m.mixMs + (this.r - m.at) * 1000 / this.inRate : NaN;
        this.port.postMessage({ bufferedMs: have * 1000 / this.inRate, underruns: this.underruns,
                                targetMs: Math.max(this.jitterMs, this.syncMs), headMixMs });
    }
}
registerProcessor('wvr-player', WvrPlayer);

class WvrMic extends AudioWorkletProcessor {
    constructor() {
        super();
        this.size = Math.round(sampleRate / 100);   // 10 ms
        this.buf = new Int16Array(this.size);
        this.n = 0;
        this.on = false;
        this.port.onmessage = (e) => { this.on = !!e.data.on; this.n = 0; };
    }
    process(inputs, outputs) {
        if (outputs[0] && outputs[0][0]) outputs[0][0].fill(0);
        const ch = inputs[0] && inputs[0][0];
        if (!ch || !this.on) return true;
        for (let i = 0; i < ch.length; i++) {
            const s = Math.max(-1, Math.min(1, ch[i]));
            this.buf[this.n++] = Math.round(s * 32767);
            if (this.n === this.size) {
                this.port.postMessage(this.buf.buffer, [this.buf.buffer]);
                this.buf = new Int16Array(this.size);
                this.n = 0;
            }
        }
        return true;
    }
}
registerProcessor('wvr-mic', WvrMic);
