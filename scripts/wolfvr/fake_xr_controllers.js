// [VR BIG SCREEN 2026-11-03] Adds two xr-standard controllers to WolfStorm's fake WebXR device (wolfstorm/tools/
// fake_xr.js, which has none), for e2e_bigscreen.mjs. Injected AFTER fake_xr.js. Not shipped to residents.
//   window.__fakeCtl.aim(hand, yaw, pitch)    point a controller (radians; yaw + = left, pitch + = up)
//   window.__fakeCtl.press(hand, index, down) a button (xr-standard: 0 trigger, 1 grip, 4 A/X, 5 B/Y, 6+ menu)
//   window.__fakeCtl.axis(hand, index, value) a stick axis (2/3 = thumbstick x/y)
(function () {
    'use strict';
    if (!window.__fakeXR || window.__fakeCtl) return;
    const mk = (hand, x) => ({
        handedness: hand, targetRayMode: 'tracked-pointer', profiles: ['oculus-touch-v3', 'generic-trigger-squeeze-thumbstick'],
        targetRaySpace: { type: 'target-ray-' + hand }, gripSpace: { type: 'grip-' + hand },
        gamepad: { mapping: 'xr-standard', connected: true, axes: [0, 0, 0, 0],
                   buttons: Array.from({ length: 7 }, () => ({ pressed: false, touched: false, value: 0 })) },
        _pos: [x, 1.4, -0.2], _yaw: 0, _pitch: 0,
    });
    const ctl = { left: mk('left', -0.2), right: mk('right', 0.2) };
    function matrix(c) {
        // R = Ry(yaw) * Rx(pitch), translated; column-major (as fake_xr.js headMatrix)
        const cy = Math.cos(c._yaw), sy = Math.sin(c._yaw), cp = Math.cos(c._pitch), sp = Math.sin(c._pitch);
        const m = new Float32Array(16);
        m[0] = cy; m[4] = sy * sp; m[8] = sy * cp;  m[12] = c._pos[0];
        m[1] = 0;  m[5] = cp;      m[9] = -sp;      m[13] = c._pos[1];
        m[2] = -sy; m[6] = cy * sp; m[10] = cy * cp; m[14] = c._pos[2];
        m[15] = 1;
        return m;
    }
    const xr = navigator.xr, orig = xr.requestSession.bind(xr);
    xr.requestSession = async (mode, init) => {
        const s = await orig(mode, init);
        s.inputSources = [ctl.left, ctl.right];
        const raf = s.requestAnimationFrame.bind(s);
        s.requestAnimationFrame = (cb) => raf((t, frame) => {
            frame.getPose = (space) => {
                for (const c of [ctl.left, ctl.right]) {
                    if (space === c.targetRaySpace || space === c.gripSpace) return { transform: { matrix: matrix(c) }, emulatedPosition: false };
                }
                return null;
            };
            cb(t, frame);
        });
        return s;
    };
    window.__fakeCtl = {
        aim: (hand, yaw, pitch) => { ctl[hand]._yaw = yaw; ctl[hand]._pitch = pitch; },
        press: (hand, i, down) => { const b = ctl[hand].gamepad.buttons[i]; b.pressed = b.touched = !!down; b.value = down ? 1 : 0; },
        axis: (hand, i, v) => { ctl[hand].gamepad.axes[i] = v; },
    };
})();
