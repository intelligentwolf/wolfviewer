/*
 * WolfViewer VR in the headset browser - the viewer's sound, played in the headset, and the headset's microphone.
 * Copyright (C) 2026 IntelligentWolf Ltd. (LGPL 2.1, as the viewer)
 *
 * [VR AUDIO 2026-11-04] Paul: "don't forget audio, what do we do with it? move it to the headset?"
 * The viewer sends its whole sound mix (world, UI, wind, voice chat) as binary messages with header byte 67 = 2
 * (wolfvrpipe.h): bytes 8..15 the first sample's number, 16..23 the viewer clock (ms) when the block was mixed,
 * 52 the sample rate (float), 64 the channels, 68 the payload bytes; then little-endian signed 16-bit PCM,
 * interleaved, 10 ms a block. It plays here through an AudioWorklet ring buffer with a small jitter buffer:
 *   - target 40 ms to start; +10 ms after each gap (at most 120 ms), -5 ms after 10 s without one (back to 40);
 *   - never grows: past the target the playback runs up to 0.5% fast (not audible) to catch up, and more than
 *     60 ms past it the oldest sound is dropped at once;
 *   - in step with the pictures: the pictures carry their capture time on the same viewer clock (bytes 16..23),
 *     wolfvr.js passes each picture's delay to noteVideoDelay(), and when the pictures are later than the sound the
 *     buffer is allowed to hold up to 150 ms more so both arrive together. skewMs is sound delay minus picture
 *     delay (positive: the sound is late).
 * The microphone ("Use the headset microphone for voice", off by default in WolfViewer): asked for once, in 2D, from
 * a press (prepareMic, from its own button in 2D; never in VR), then 10 ms of 16-bit mono at a time goes up the socket in a
 * binary message with its own 16-byte header (wolfvrpipe.h "page -> viewer, binary"), only while the viewer asks
 * for it ({"t":"micwant","on":true}).
 * No external downloads; the two worklets are in vraudio-worklet.js (the page's CSP allows scripts from 'self' only).
 */

const MAGIC = 0x31525657;   // "WVR1" (wolfvrpipe.cpp kMagic)


export class VRAudio {
    /**
     * @param {object} o
     * @param {function(object)} o.send        a JSON message to the viewer (wolfvr.js send)
     * @param {function(ArrayBuffer)} o.sendBinary  a binary message to the viewer
     * @param {function(): number} o.clockOff  viewer clock minus page clock (ms), NaN until the ping sync has one
     * @param {function(string)} o.onError     shows a plain sentence to the resident ('' clears it)
     * @param {function(boolean)} [o.onMicWanted]  the viewer started / stopped wanting the microphone
     */
    constructor(o) {
        this.o = o;
        this.ctx = null;
        this.player = null;
        this.started = false;
        this.wantOn = true;            // the resident's choice on the page (setOn)
        this.on = false;               // what the viewer was last told
        this.micWanted = false;        // the viewer asked for the microphone (WolfVRHeadsetMic)
        this.micAsked = false;         // getUserMedia was asked (once)
        this.micStream = null;
        this.micNode = null;
        this.micSending = false;
        // readout
        this.bufferedMs = 0;
        this.underruns = 0;
        this.targetMs = 0;
        this.soundDelayMs = NaN;       // viewer mix -> heard here
        this.videoDelayMs = NaN;       // viewer capture -> drawn here (from noteVideoDelay)
        this.skewMs = NaN;             // soundDelayMs - videoDelayMs
        this.syncMs = NaN;             // the buffer the sync asks for (smoothed)
        this.blocks = 0;
        this.lastSample = -1;
        this.gaps = 0;                 // blocks the viewer or the network dropped
        this._starting = null;
        document.addEventListener('visibilitychange', () => this._tell());
    }

    /** Creates / resumes the AudioContext. Call from a press (Quest Browser plays sound only after one). */
    start() {
        if (this._starting) return this._starting;
        this._starting = (async () => {
            try {
                if (!this.ctx) {
                    const AC = window.AudioContext || window.webkitAudioContext;
                    if (!AC) throw new Error('no AudioContext');
                    try { this.ctx = new AC({ sampleRate: 48000, latencyHint: 'interactive' }); }
                    catch (e) { this.ctx = new AC({ latencyHint: 'interactive' }); }   // a browser that will not do 48 kHz
                    await this.ctx.audioWorklet.addModule('vraudio-worklet.js');   // 'self' (the page's CSP)
                    this.player = new AudioWorkletNode(this.ctx, 'wvr-player', { numberOfInputs: 0, numberOfOutputs: 1,
                                                                                  outputChannelCount: [2] });
                    this.player.port.onmessage = (e) => this._onStats(e.data);
                    this.player.connect(this.ctx.destination);
                }
                if (this.ctx.state !== 'running') await this.ctx.resume();
                this.started = true;
                this._tell();
            } catch (e) {
                this.o.onError('The headset could not start playing WolfViewer\'s sound, so it stays on your PC. ' +
                               'Press Enter VR again to try once more.');
            } finally {
                this._starting = null;
            }
        })();
        return this._starting;
    }

    /** The resident's choice on the page: play the viewer's sound here (true) or not. */
    setOn(on) {
        this.wantOn = !!on;
        this._tell();
    }

    /** The socket (re)connected and the viewer said welcome: say again whether this page plays the sound. */
    onConnected() {
        this.on = false;
        if (this.player) this.player.port.postMessage({ reset: true });
        this.lastSample = -1;
        this._tell(true);
    }

    /** A text message from the viewer; returns true when it was one of ours. */
    onText(m) {
        if (!m || m.t !== 'micwant') return false;
        const was = this.micWanted;
        this.micWanted = !!m.on;
        this._micState();
        if (was !== this.micWanted && this.o.onMicWanted) this.o.onMicWanted(this.micWanted);
        return true;
    }

    /** True when the viewer wants the headset microphone and it has not been asked for yet (show a button). */
    get micPending() { return this.micWanted && !this.micAsked; }

    /** One sound block (header byte 67 = 2). */
    onBlock(buf, hdr) {
        if (!this.player || !this.started) return;
        const d = new DataView(buf);
        const size = d.getUint32(4, true);
        const channels = d.getUint8(64);
        const bytes = d.getUint32(68, true);
        const rate = Math.round(d.getFloat32(52, true));
        if (!channels || channels > 8 || rate < 8000 || size + bytes > buf.byteLength || (bytes & 1)) return;
        const first = Number(d.getBigUint64(8, true));
        const frames = bytes / 2 / channels;
        if (this.lastSample >= 0 && first > this.lastSample) this.gaps++;
        this.lastSample = first + frames;
        this.blocks++;
        const pcm = buf.slice(size, size + bytes);   // its own buffer, handed to the worklet
        this.player.port.postMessage({ pcm, channels, rate, mixMs: d.getFloat64(16, true) }, [pcm]);
    }

    /** Each drawn picture's delay (page draw time + clockOff - captureMs), as wolfvr.js num.v2p. */
    noteVideoDelay(ms) {
        if (!Number.isFinite(ms) || ms < 0 || ms > 2000) return;
        this.videoDelayMs = Number.isFinite(this.videoDelayMs) ? this.videoDelayMs * 0.95 + ms * 0.05 : ms;
    }

    /**
     * Asks for the headset microphone once, if the viewer wants it. Call from a press in 2D (a "Use the headset
     * microphone" button shown while micPending): never from the Enter VR click (the browser's question could take
     * longer than requestSession's press allows) and never in VR. Resolves when done (granted or not).
     */
    async prepareMic() {
        if (!this.micWanted || this.micAsked) { this._micState(); return; }
        this.micAsked = true;
        await this.start();
        if (!this.ctx) return;
        try {
            if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) throw new Error('none');
            this.micStream = await navigator.mediaDevices.getUserMedia({
                audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true }
            });
            const src = this.ctx.createMediaStreamSource(this.micStream);
            this.micNode = new AudioWorkletNode(this.ctx, 'wvr-mic', { numberOfInputs: 1, numberOfOutputs: 1,
                                                                       outputChannelCount: [1] });
            this.micNode.port.onmessage = (e) => this._sendMic(e.data);
            const sink = this.ctx.createGain();
            sink.gain.value = 0;   // the mic node is pulled through a silent path; nothing of it is heard
            src.connect(this.micNode);
            this.micNode.connect(sink);
            sink.connect(this.ctx.destination);
            this.o.onError('');
        } catch (e) {
            this.micStream = null;
            this.o.onError('The headset did not allow its microphone, so voice chat uses your PC\'s microphone. ' +
                           'To change this, allow the microphone for this page in the headset browser\'s settings, ' +
                           'then reload the page.');
        }
        this._micState();
    }

    // ---- inside ----
    _tell(force) {
        const on = this.started && this.wantOn && document.visibilityState !== 'hidden';
        if (on !== this.on || force) {
            this.on = on;
            this.o.send({ t: 'audio', on });
        }
        this._micState();
    }

    _micState() {
        const send = !!(this.micWanted && this.micStream && this.on);
        if (this.micNode && send !== this.micSending) this.micNode.port.postMessage({ on: send });
        this.micSending = send;
        if (this.micStream) for (const t of this.micStream.getAudioTracks()) t.enabled = send;
    }

    _sendMic(ab) {
        if (!this.micSending || !this.ctx) return;
        const pcm = new Uint8Array(ab);
        const msg = new ArrayBuffer(16 + pcm.byteLength);
        const d = new DataView(msg);
        d.setUint32(0, MAGIC, true);
        d.setUint32(4, 16, true);
        d.setUint8(8, 1);    // microphone
        d.setUint8(9, 1);    // mono
        d.setUint32(12, Math.round(this.ctx.sampleRate), true);
        new Uint8Array(msg, 16).set(pcm);
        this.o.sendBinary(msg);
    }

    _onStats(s) {
        this.bufferedMs = s.bufferedMs;
        this.underruns = s.underruns;
        this.targetMs = s.targetMs;
        const off = this.o.clockOff();
        if (Number.isFinite(off) && Number.isFinite(s.headMixMs) && this.ctx) {
            const outMs = 1000 * ((this.ctx.outputLatency || 0) + (this.ctx.baseLatency || 0));
            this.soundDelayMs = performance.now() + off - s.headMixMs + outMs;
        }
        if (Number.isFinite(this.soundDelayMs) && Number.isFinite(this.videoDelayMs)) {
            this.skewMs = this.soundDelayMs - this.videoDelayMs;
            // hold the sound back (up to 150 ms of buffer) when the pictures arrive later than it
            const want = Math.max(0, Math.min(150, this.bufferedMs - this.skewMs));
            this.syncMs = Number.isFinite(this.syncMs) ? this.syncMs * 0.8 + want * 0.2 : want;   // no sudden jumps
            if (this.player) this.player.port.postMessage({ syncMs: this.syncMs });
        }
    }
}
