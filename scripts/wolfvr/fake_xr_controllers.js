// [VR BIG SCREEN 2026-11-03] Adds two xr-standard controllers to WolfStorm's fake WebXR device (wolfstorm/tools/
// fake_xr.js, which has none), for the e2e tests. Injected AFTER fake_xr.js. Not shipped to residents.
// [VR HUD CLICKS 2026-11-04] Modelled on the Meta Quest 3's Touch Plus controllers as Quest Browser reports them, after
// Paul's "the VR HUD - nothing I clicked worked": the button layout of the WebXR input profiles registry
// (packages/registry/profiles/meta/meta-quest-touch-plus.json, gamepad.buttons):
//   left:  0 trigger, 1 squeeze, 2 (none), 3 thumbstick, 4 x, 5 y, 6 thumbrest, 7 menu
//   right: 0 trigger, 1 squeeze, 2 (none), 3 thumbstick, 4 a, 5 b, 6 thumbrest
// the profiles list a Quest 3 gives ('meta-quest-touch-plus' first), a thumb RESTING on each thumbrest (reported
// pressed and touched: the worst case, which the page must ignore), and the session's select events a browser fires
// for the trigger (selectstart on press; select + selectend on release) on top of the gamepad state.
//   window.__fakeCtl.aim(hand, yaw, pitch)    point a controller (radians; yaw + = left, pitch + = up)
//   window.__fakeCtl.press(hand, index, down) a button
//   window.__fakeCtl.axis(hand, index, value) a stick axis (2/3 = thumbstick x/y)
//   window.__fakeCtl.thumbrest(hand, on)      the thumb on / off the rest
//   window.__swapViews = true                  the browser lists the right eye's view first
(function () {
    'use strict';
    if (!window.__fakeXR || window.__fakeCtl) return;
    const mk = (hand, x, n) => ({
        handedness: hand, targetRayMode: 'tracked-pointer',
        profiles: ['meta-quest-touch-plus', 'oculus-touch-v3', 'generic-trigger-squeeze-thumbstick'],
        targetRaySpace: { type: 'target-ray-' + hand }, gripSpace: { type: 'grip-' + hand },
        gamepad: { mapping: 'xr-standard', connected: true, axes: [0, 0, 0, 0],
                   buttons: Array.from({ length: n }, () => ({ pressed: false, touched: false, value: 0 })) },
        _pos: [x, 1.4, -0.2], _yaw: 0, _pitch: 0,
    });
    const ctl = { left: mk('left', -0.2, 8), right: mk('right', 0.2, 7) };
    for (const c of [ctl.left, ctl.right]) { const b = c.gamepad.buttons[6]; b.pressed = b.touched = true; b.value = 1; }
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
    let session = null, lastFrame = null;
    const xr = navigator.xr, orig = xr.requestSession.bind(xr);
    xr.requestSession = async (mode, init) => {
        const s = await orig(mode, init);
        s.inputSources = [ctl.left, ctl.right];
        session = s;
        const raf = s.requestAnimationFrame.bind(s);
        s.requestAnimationFrame = (cb) => raf((t, frame) => {
            frame.getPose = (space) => {
                for (const c of [ctl.left, ctl.right]) {
                    if (space === c.targetRaySpace || space === c.gripSpace) return { transform: { matrix: matrix(c) }, emulatedPosition: false };
                }
                return null;
            };
            frame.session = s;
            // window.__swapViews: the browser lists the right eye first (WebXR does not fix the order of views)
            if (!frame.__wrapped) {
                const gvp = frame.getViewerPose.bind(frame);
                frame.getViewerPose = (space) => {
                    const p = gvp(space);
                    if (p && window.__swapViews) return { ...p, views: p.views.slice().reverse() };
                    return p;
                };
                frame.__wrapped = true;
            }
            lastFrame = frame;
            cb(t, frame);
        });
        return s;
    };
    function fire(type, c) {
        if (!session || !lastFrame) return;
        const ev = new Event(type);
        ev.inputSource = c;
        ev.frame = lastFrame;
        session.dispatchEvent(ev);
    }
    window.__fakeCtl = {
        aim: (hand, yaw, pitch) => { ctl[hand]._yaw = yaw; ctl[hand]._pitch = pitch; },
        press: (hand, i, down) => {
            const b = ctl[hand].gamepad.buttons[i];
            const was = b.pressed;
            b.pressed = b.touched = !!down; b.value = down ? 1 : 0;
            if (i === 0 && down && !was) fire('selectstart', ctl[hand]);
            if (i === 0 && !down && was) { fire('select', ctl[hand]); fire('selectend', ctl[hand]); }
        },
        axis: (hand, i, v) => { ctl[hand].gamepad.axes[i] = v; },
        thumbrest: (hand, on) => { const b = ctl[hand].gamepad.buttons[6]; b.pressed = b.touched = !!on; b.value = on ? 1 : 0; },
    };
})();
