/**
 * @file wolfvrscreen.cpp
 * @brief WolfViewer VR in the headset browser, big-screen mode. See wolfvrscreen.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolfvrscreen.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llfocusmgr.h"
#include "llfloater.h"
#include "lltoast.h"
#include "llrootview.h"
#include "llviewermenu.h"
#include <algorithm>
#include <functional>
#include "llappviewer.h"
#include "lldir.h"
#include "llimagepng.h"
#include "lltoolbarview.h"
#include "lltoolmgr.h"
#include "lltoolpie.h"

#include "llrendertarget.h"
#include "llgl.h"
#include "llglheaders.h"
#include "llkeyboard.h"
#include "llstring.h"
#include "lltimer.h"
#include "llstartup.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llvoavatarself.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "wolfvrstream.h"
#include "wolfvrui.h"
#include "pipeline.h"

#include <cmath>
#include <cstring>
#include <vector>

extern bool gSnapshot;       // llviewerdisplay.cpp:190
extern bool gCubeSnapshot;   // llviewerdisplay.cpp:191
extern bool gDisplaySwapBuffers;   // llviewerdisplay.cpp:138 - swap() shows the frame only when true
extern bool gDepthDirty;           // llviewerdisplay.cpp:186
extern bool gResizeScreenTexture;  // llviewerdisplay.cpp:187 - display() reallocates the pipeline's targets when set
void render_hud_attachments();     // llviewerdisplay.cpp:209 (defined there, not in its header)
void render_ui_2d();               // llviewerdisplay.cpp:211

namespace
{
    constexpr int kSlots = 3;   // pixel buffers in flight: read into one, collect the one from two frames ago

    struct Capture
    {
        GLuint fbo = 0, tex = 0;
        GLuint pbo[kSlots] = {0, 0, 0};
        GLsync fence[kSlots] = {nullptr, nullptr, nullptr};
        F64 issued[kSlots] = {0, 0, 0};
        U64 order[kSlots] = {0, 0, 0};   // [VR READBACK] the order the reads were issued in (newest = largest)
        U64 issues = 0;
        wolfvr::HeadPose pose[kSlots];   // 3D mode: the head pose each picture was drawn for
        U32 w = 0, h = 0;          // picture size
        int next = 0;
        std::vector<uint8_t> buf;  // handed to the stream; comes back as the stream's spare buffer
        // where the window sits in the picture (letterbox), top-row-first picture pixels: [x0, x1) x [y0, y1)
        S32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        S32 win_w = 0, win_h = 0;
        F64 last = 0;
        bool world = false;        // the GL objects are set up for 3D mode (square window crop) or big screen
        bool eye0 = false;         // [VR STEREO] this frame's left eye is in the left half; the right eye reads it back
        // [VR DEPTH 2026-11-04] the eyes' depth, 1/8 of the picture each way: blitted from the scene's own depth buffer
        // (LLPipeline mRT->deferredScreen, GL_DEPTH_COMPONENT24) into a renderbuffer of the same format (a depth blit
        // needs matching formats and GL_NEAREST: glBlitFramebuffer, OpenGL 4.6 core 18.3.1), read back like the colour
        GLuint dfbo = 0, drb = 0;
        GLuint dpbo[kSlots] = {0, 0, 0};
        F32 znear[kSlots] = {0, 0, 0}, zfar[kSlots] = {0, 0, 0};
        bool has_depth[kSlots] = {false, false, false};
        U32 dw = 0, dh = 0;
        bool depth_ok = false;     // the first blit worked (checked once)
        bool depth_checked = false;
        std::vector<float> dbuf;
    };
    Capture gCap;

    // [VR MOVE 2026-11-03] Paul: "left controller must move him". The headset's left stick, held now, and jump/fly
    // up/down held, applied every frame like held movement keys (llviewerinput.cpp agent_push_forwardbackward:
    // gAgent.moveAt each frame while the key is down). A stick that has not been heard from for a second (the
    // headset slept or the network dropped) counts as let go, so the avatar never walks off on its own.
    struct Stick { F32 x = 0, y = 0; S32 lift = 0; F64 heard = 0; bool ran = false; };
    Stick gStick;
    constexpr F32 kDead = 0.25f;   // WolfStorm input_manager.js _vrLocomotionFlags DEAD
    constexpr F32 kRun = 0.9f;     // pushed (nearly) all the way: run, as double-tap-and-hold does

    // [VR 3D] the head pose the camera was turned to for this frame, and the view to give back afterwards
    wolfvr::HeadPose gFramePose;
    bool gCameraOverridden = false;
    F32 gSavedView = 0.f;

    // [VR STEREO 2026-11-04] Which eye display() is drawing: -1 one view (mono / big screen), 0 left, 1 right. Both
    // eyes are drawn from ONE head pose (taken for the left eye), each from its own position: the head centre the
    // avatar's eyes (gMap.base) plus that eye's offset from the headset, turned with the head.
    S32 gEye = -1;
    bool gPaused = false;       // [VR LEAN] the desktop window is not drawn this frame (displayFrame)
    bool gDebugStats = false;   // [VR LEAN] WolfVRDebugStats, read once (the first frame)
    // frame-time numbers for the log (and Paul's report): display() time per frame, mono vs stereo
    struct FrameTimes { F64 sum = 0, eye0 = 0; U32 n = 0; F64 since = 0; bool stereo = false; };
    FrameTimes gTimes;
    // [VR RENDER SIZE 2026-11-04] Measured (e2e_stages.mjs, The Caribbean): each eye was drawn across the whole world view
    // of the window (2522 x 1300 here, 3.3 million pixels) and then squeezed into a 1152 px square eye; drawing at half
    // the size took the eyes from 14.3 / 13.6 to 11.0 / 10.1 ms, so the GPU's share is real. Now, while the headset is
    // in VR 3D Mode, the world's area for the frame is a square the eye's own size (at most the window's height): the
    // same pixels the headset gets, sharper than the squeeze, at 40 % of the cost in pixels. The pipeline's targets
    // follow it (gResizeScreenTexture, once when it starts or changes); the real area is given back after each frame,
    // so the desktop's UI, picks and the pointer never see it.
    struct EyeRect { bool active = false; LLRect real, over; };
    EyeRect gEyeRect;
    void useEyeRect(bool on)
    {
        if (!gViewerWindow) return;
        if (on) gViewerWindow->wolfVRSetWorldViewRaw(gEyeRect.over);
        else if (gEyeRect.active) gViewerWindow->wolfVRSetWorldViewRaw(gEyeRect.real);
    }
    // [VR STAGES 2026-11-04] this frame's start and the end of its left eye (wolfvr::clockMs), put on the picture
    struct Stamp { double frame = 0, eye0 = 0; };
    Stamp gStamp;

    // [VR 3D FIX 2026-11-04] Paul after the Quest 3 test: "It still doesn't feel 3D when I'm in 3D mode". Three causes,
    // all fixed here: (1) the eyes were placed around the THIRD-PERSON camera, metres behind the avatar, so the near
    // world was never near; now the view is first person, at the avatar's eyes (the point the mouselook camera uses,
    // LLAgentCamera::calcCameraPositionTargetGlobal CAMERA_MODE_MOUSELOOK: the render position plus LLVOAvatar
    // mHeadOffset, the smoothed eye point updateHeadOffset keeps), plus the headset's own lean (motion parallax);
    // (2) each eye was one symmetric square view; now each eye is drawn for its own off-axis frustum, the tangents of
    // its XRView.projectionMatrix edges (the page sends them, "pt"), the way the headset itself would; (3) the whole
    // world view of the window is used for the eye, not its centre square, so more of the render reaches the headset.
    struct HeadMap
    {
        bool valid = false;
        LLVector3 base;              // the head centre this frame (agent coordinates)
        LLVector3 fwd, left, up;     // the avatar's heading frame (Z up): WebXR -Z maps to fwd, -X to left, +Y to up
        F32 q[4] = {0, 0, 0, 1};     // the head orientation this frame was drawn with (WebXR)
        F32 p[3] = {0, 0, 0};        // the head position in the page's reference space, and whether it was sent
        bool has_p = false;
        F32 view = 1.f;              // the camera's vertical field of view after the frame (radians), for picking
    };
    HeadMap gMap;
    // the headset position where leaning is measured from (re-centred on a new mode, a sit / stand, or a big step)
    struct Lean { bool set = false; F32 p0[3] = {0, 0, 0}; S32 sit = -1; };
    Lean gLean;
    constexpr F32 kLeanMax = 0.5f;      // metres: further than this is walking about the room, not leaning
    // this eye's off-axis window as a scale and offset of the symmetric projection (LLViewerCamera::setPerspective)
    struct EyeCrop { bool on = false; F32 ax = 1, bx = 0, ay = 1, by = 0; F32 zn = 0, zf = 0; };   // + the depth range drawn with
    EyeCrop gCrop;

    // rotate v by the unit quaternion q (WebXR) and map it to the viewer's axes of `m`:
    // v' = v + 2w (u x v) + 2 u x (u x v), u = (x, y, z); then (x, y, z) -> -z fwd, -x left, +y up
    LLVector3 headToAgent(const HeadMap& m, F32 vx, F32 vy, F32 vz)
    {
        const F32 qx = m.q[0], qy = m.q[1], qz = m.q[2], qw = m.q[3];
        const F32 cx = qy * vz - qz * vy, cy = qz * vx - qx * vz, cz = qx * vy - qy * vx;
        const F32 ccx = qy * cz - qz * cy, ccy = qz * cx - qx * cz, ccz = qx * cy - qy * cx;
        const F32 rx = vx + 2.f * (qw * cx + ccx), ry = vy + 2.f * (qw * cy + ccy), rz = vz + 2.f * (qw * cz + ccz);
        return m.fwd * (-rz) + m.left * (-rx) + m.up * ry;
    }
    // a direction in the page's reference space (no head rotation) to the viewer's axes
    LLVector3 refToAgent(const HeadMap& m, F32 vx, F32 vy, F32 vz) { return m.fwd * (-vz) + m.left * (-vx) + m.up * vy; }

    // The avatar's eyes (agent coordinates), as the mouselook camera places itself (see above). Forward and up from the
    // avatar's centre (mHeadOffset X and Z; its Y is one eye's, "since we only care about Z, just grab one of the
    // eyes", LLVOAvatar::updateHeadOffset), so the view is between the eyes. Seated on an object: through the seat's
    // render matrix, as mouselook does. No avatar: the agent camera.
    LLVector3 avatarEyes()
    {
        if (!isAgentAvatarValid() || gAgentAvatarp->mDrawable.isNull()) return LLViewerCamera::getInstance()->getOrigin();
        LLVector3 off(gAgentAvatarp->mHeadOffset.mV[VX], 0.f, gAgentAvatarp->mHeadOffset.mV[VZ]);
        if (gAgentAvatarp->isSitting() && gAgentAvatarp->getParent())
        {
            const LLMatrix4& mat = ((LLViewerObject*)gAgentAvatarp->getParent())->getRenderMatrix();
            return (gAgentAvatarp->getPosition() + off * gAgentAvatarp->getRotation()) * mat;
        }
        F32 fixup = 0.f;
        if (gAgentAvatarp->hasPelvisFixup(fixup) && !gAgentAvatarp->isSitting()) off.mV[VZ] -= fixup;
        return gAgentAvatarp->getRenderPosition() + off * gAgentAvatarp->getRenderRotation();
    }

    // This frame's head: the avatar's heading (yaw only - the head's own turn is added by the pose), its eyes, and the
    // headset's lean from where it was when 3D Mode started (a sit, a stand or a step of more than 1 m re-centres it).
    // [VR VIEW 2026-11-04] The rig's heading, as WolfStorm's (render_manager.js _placeVrRig "THE RIG'S YAW"): taken
    // from the avatar's facing when the headset enters VR and after a teleport, and turned ONLY by the right stick's
    // comfort turn (applyEvents TURN). Never chasing the avatar: whatever else turns it (a ship, a script, the desktop)
    // must not swing the world round the resident - uncommanded rotation is the classic nausea trigger.
    struct Rig { bool set = false; LLVector3 fwd = LLVector3::x_axis; LLVector3 at; };
    Rig gRig;

    LLVector3 avatarHeading()
    {
        LLVector3 fwd = gAgent.getAtAxis();
        fwd.mV[VZ] = 0.f;
        if (fwd.normVec() < 1e-3f) fwd = LLVector3::x_axis;
        return fwd;
    }

    void buildMap(const wolfvr::HeadPose& hp)
    {
        const LLVector3 here = gAgent.getPositionAgent();
        if (!gRig.set || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE || (here - gRig.at).length() > 50.f)
        {
            gRig.fwd = avatarHeading();   // entering VR, a teleport, or a jump of more than 50 m (a region crossing)
            gRig.set = true;
        }
        gRig.at = here;
        const LLVector3 fwd = gRig.fwd;
        gMap.fwd = fwd;
        gMap.up = LLVector3::z_axis;
        gMap.left = gMap.up % fwd;
        std::memcpy(gMap.q, hp.q, sizeof gMap.q);
        gMap.has_p = hp.has_pos;
        std::memcpy(gMap.p, hp.pos, sizeof gMap.p);
        // [VR VIEW 2026-11-04] Paul: "when i'm in 3d vr i'm inside my avatar's body ... i want to see my avatar and have
        // the option to hide it". Default: as WolfStorm VR (wolfstorm/js/rendering/render_manager.js _placeVrRig), the
        // headset's floor is placed behind and above the avatar by the desktop's own rear-view offset (settings.xml
        // CameraOffsetRearView [-3, 0, 0.75] x CameraOffsetScale), along the avatar's heading, and the head is wherever
        // the headset puts it above that floor (its real height and steps, 1:1). WolfVRHideAvatar: first person at the
        // avatar's eyes, with the avatar and its attachments not drawn (displayFrame), plus the headset's lean.
        static LLCachedControl<bool> hide_me(gSavedSettings, "WolfVRHideAvatar", false);
        const S32 sit = isAgentAvatarValid() && gAgentAvatarp->isSitting() ? (gAgentAvatarp->getParent() ? 2 : 1) : 0;
        if (!hide_me)
        {
            const LLVector3 av = isAgentAvatarValid() && gAgentAvatarp->mDrawable.notNull() ? gAgentAvatarp->getRenderPosition()
                                                                                         : gAgent.getPositionAgent();
            const LLVector3 rear = gSavedSettings.getVector3("CameraOffsetRearView") * gSavedSettings.getF32("CameraOffsetScale");
            const LLVector3 floor = av + gMap.fwd * rear.mV[VX] + gMap.left * rear.mV[VY] + gMap.up * rear.mV[VZ];
            // no head position from the page (an older page): standing height, WolfStorm's own first-person 1.6 m
            gMap.base = floor + (hp.has_pos ? refToAgent(gMap, hp.pos[0], hp.pos[1], hp.pos[2]) : gMap.up * 1.6f);
            gMap.valid = true;
            gLean.set = false;
            return;
        }
        LLVector3 base = avatarEyes();
        if (hp.has_pos)
        {
            if (!gLean.set || gLean.sit != sit) { std::memcpy(gLean.p0, hp.pos, sizeof gLean.p0); gLean.set = true; gLean.sit = sit; }
            F32 d[3] = {hp.pos[0] - gLean.p0[0], hp.pos[1] - gLean.p0[1], hp.pos[2] - gLean.p0[2]};
            const F32 n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (n > 1.0f) { std::memcpy(gLean.p0, hp.pos, sizeof gLean.p0); d[0] = d[1] = d[2] = 0.f; }
            else if (n > kLeanMax) { for (F32& c : d) c *= kLeanMax / n; }
            base += refToAgent(gMap, d[0], d[1], d[2]);
        }
        gMap.base = base;
        gMap.valid = true;
    }

    // [VR WORLD POINTER 2026-11-04] Paul: "I couldn't point and sit on anything". The right controller's ray, when it
    // is not on the action bar, is the mouse: on something drawn on the HUDs layer it is the mouse there; anywhere
    // else it finds what it points at in the world (the pipeline's own ray test, LLPipeline::lineSegmentIntersectInWorld,
    // the one cursorIntersect uses) and puts the mouse on that point as seen from the head, so the viewer's own hover,
    // click, touch, sit-on-click, right-click menu and click-to-walk all run exactly as with the desktop mouse
    // (LLViewerWindow::handleMouseMove / handleMouseDown / handleRightMouseDown -> LLToolPie, pickImmediate).
    enum class Aim { NONE, UI, WORLD };
    struct Pointer
    {
        F64 heard = 0;               // last ray (0 = none)
        F32 o[3] = {0, 0, 0}, d[3] = {0, 0, -1};
        bool has_layer = false;
        F32 lu = 0, lv = 0;
        Aim aim = Aim::NONE;
        Aim held[3] = {Aim::NONE, Aim::NONE, Aim::NONE};   // a button down captures what it was pressed on
        LLCoordGL pos;
        bool pos_ok = false;
        F32 dist = -1.f;             // metres along the ray to what it hits (-1 nothing)
        bool prim = false;           // what it hits is an object (not the ground)
        LLVector3 dbg_dir;           // [VR LEAN] debug only (WolfVRDebugStats): the direction it was projected along
        F64 cast_at = 0;
        std::string sent;            // the last "aim" message
        bool idle = false;           // no ray, and the page already told so
        F64 sent_at = 0;
    };
    Pointer gPtr;
    F64 gPointerUsed = 0;            // the headset's pointer (any mode) was used: the window counts the mouse as in it
    constexpr F32 kReach = 64.f;     // metres the pointer looks along its ray

    // [VR HUD LAYER 2026-11-04] The viewer's UI layer (HUD attachments + the 2D UI: floaters, notices, toolbars) drawn a
    // second time, into its own transparent target, for the headset - the desktop's own frame is not touched. At most
    // 30 a second; read back asynchronously like the world picture (pixel buffers + fences, collected later).
    struct Layer
    {
        LLRenderTarget rt;
        GLuint pbo[kSlots] = {0, 0, 0};
        GLsync fence[kSlots] = {nullptr, nullptr, nullptr};
        F64 issued[kSlots] = {0, 0, 0};
        U32 ww = 0, wh = 0;              // the window size the target is made for
        U32 w = 0, h = 0;                // the picture size (the window, scaled to fit 1600 x 900) the buffers are for
        GLuint sfbo = 0, stex = 0;       // the scaled copy that is read back
        int next = 0;
        F64 last = 0;
        F64 cost = 0, cost_n = 0, since = 0;   // draw time of the layer pass, for the log
        std::vector<uint8_t> buf;        // top row first, handed to the stream
        std::vector<uint8_t> alpha;      // the last collected layer's coverage, for "is there anything to click"
        U32 aw = 0, ah = 0;
    };
    Layer gLayer;

    // [VR PANELS 2026-11-04] Paul: "huds etc everything should be drawn in the headset ... a proper vr interface ...
    // migrate the floaters to the headset". Each open window (every floater in the floater view: map, inventory, IM,
    // people, dialogs, notices that are floaters) is told to the headset by name and rectangle on the UI layer
    // picture; the page cuts it out of that picture and shows it as its OWN panel in an arc in front of the resident,
    // where it stays put in the room (as Virtual Desktop's and SteamVR's desktop windows do), and a point at it is the
    // mouse on that window. The rest of the layer (HUD attachments, menus, tips) stays the head-locked HUD band.
    // Sent only when the list changes. Rectangles: 0..1 of the window, from its top-left (the layer picture's axes).
    std::string gPanelsSent;
    F64 gPanelsAt = 0;
    void sendPanels()
    {
        if (!gFloaterView || !gViewerWindow) return;
        const LLVector2 ds = gViewerWindow->getDisplayScale();
        const F32 W = (F32)llmax(1, gViewerWindow->getWindowWidthRaw()), H = (F32)llmax(1, gViewerWindow->getWindowHeightRaw());
        std::string list;
        int n = 0;
        // [VR PANELS] the windows top first (the floater view's child list starts with the front one); each panel also
        // gets the parts of it a window IN FRONT of it covers on the desktop ("o"), which the page leaves out of it -
        // Paul: "you can see bits of screens appearing" (video frames 18-22: the QR window over the inventory's corner)
        std::vector<LLRect> above;   // raw pixels, GL coordinates
        const auto esc = [](const std::string& t) {
            std::string o;
            for (char c : t) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((unsigned char)c >= 32) o += c; }
            return o;
        };
        auto uv = [&](const LLRect& r) {
            return llformat("[%.4f,%.4f,%.4f,%.4f]", llclamp(r.mLeft / W, 0.f, 1.f), llclamp(1.f - r.mTop / H, 0.f, 1.f),
                            llclamp(r.mRight / W, 0.f, 1.f), llclamp(1.f - r.mBottom / H, 0.f, 1.f));
        };
        for (LLView* v : *gFloaterView->getChildList())
        {
            LLFloater* f = dynamic_cast<LLFloater*>(v);
            if (!f || !f->isShown() || !f->isInVisibleChain()) continue;
            // [VR WINDOW 2026-11-04] never the VR window itself (its QR code and address are for the desktop)
            if (f->getInstanceName() == "wolf_vr_stream") continue;
            const LLRect s = f->calcScreenRect();
            const LLRect r(ll_round(s.mLeft * ds.mV[VX]), ll_round(s.mTop * ds.mV[VY]), ll_round(s.mRight * ds.mV[VX]), ll_round(s.mBottom * ds.mV[VY]));
            if (r.getWidth() < 0.01f * W || r.getHeight() < 0.01f * H) continue;
            if (n < 12)
            {
                // the window's own name (one per kind, plus its key for the ones that open many: IMs, profiles)
                std::string id = f->getInstanceName();
                if (f->getKey().isDefined()) id += ":" + f->getKey().asString();
                std::string title = f->getTitle();
                LLStringUtil::truncate(title, 60);
                LLStringUtil::truncate(id, 80);
                std::string over;
                for (const LLRect& a : above)
                {
                    LLRect i = r;
                    i.intersectWith(a);
                    if (i.getWidth() <= 0 || i.getHeight() <= 0) continue;
                    over += (over.empty() ? "" : ",") + uv(i);
                }
                list += llformat("%s{\"k\":\"%s\",\"n\":\"%s\",\"r\":%s,\"o\":[%s]}", n ? "," : "", esc(id).c_str(),
                                 esc(title).c_str(), uv(r).c_str(), over.c_str());
                ++n;
            }
            above.push_back(r);
        }
        const std::string msg = "{\"t\":\"panels\",\"p\":[" + list + "]}";
        const F64 now = LLTimer::getTotalSeconds();
        if (msg == gPanelsSent && now - gPanelsAt < 2.0) return;   // and again every 2 s, for a page that just connected
        gPanelsSent = msg;
        gPanelsAt = now;
        WolfVRStream::sendText(msg);
    }
    bool gLayerDownIgnored[3] = {false, false, false};   // a press on an empty part of the layer, and its release

    void freeLayer()
    {
        for (int i = 0; i < kSlots; ++i)
        {
            if (gLayer.fence[i]) glDeleteSync(gLayer.fence[i]);
            gLayer.fence[i] = nullptr;
        }
        if (gLayer.pbo[0]) glDeleteBuffers(kSlots, gLayer.pbo);
        for (GLuint& b : gLayer.pbo) b = 0;
        if (gLayer.sfbo) glDeleteFramebuffers(1, &gLayer.sfbo);
        if (gLayer.stex) glDeleteTextures(1, &gLayer.stex);
        gLayer.sfbo = gLayer.stex = 0;
        gLayer.rt.release();
        gLayer.w = gLayer.h = gLayer.ww = gLayer.wh = 0;
        gLayer.next = 0;
    }

    // Anything drawn near window pixel (x, y) (GL coordinates, origin bottom-left) in the last layer picture?
    bool layerHasPixel(S32 x, S32 y)
    {
        if (gLayer.alpha.empty() || !gLayer.aw || !gLayer.ah || !gLayer.ww || !gLayer.wh) return true;   // not known yet
        // window pixels -> the scaled picture's pixels
        x = (S32)((F64)x * gLayer.aw / gLayer.ww);
        y = (S32)((F64)y * gLayer.ah / gLayer.wh);
        const S32 row = (S32)gLayer.ah - 1 - y;                              // the buffer is top row first
        for (S32 dy = -3; dy <= 3; ++dy)
        {
            for (S32 dx = -3; dx <= 3; ++dx)
            {
                const S32 px = x + dx, py = row + dy;
                if (px < 0 || py < 0 || px >= (S32)gLayer.aw || py >= (S32)gLayer.ah) continue;
                if (gLayer.alpha[(size_t)py * gLayer.aw + px] >= 8) return true;
            }
        }
        return false;
    }

    void freeGl()
    {
        for (int i = 0; i < kSlots; ++i)
        {
            if (gCap.fence[i]) glDeleteSync(gCap.fence[i]);
            gCap.fence[i] = nullptr;
        }
        if (gCap.pbo[0]) glDeleteBuffers(kSlots, gCap.pbo);
        if (gCap.fbo) glDeleteFramebuffers(1, &gCap.fbo);
        if (gCap.tex) glDeleteTextures(1, &gCap.tex);
        if (gCap.dpbo[0]) glDeleteBuffers(kSlots, gCap.dpbo);
        if (gCap.dfbo) glDeleteFramebuffers(1, &gCap.dfbo);
        if (gCap.drb) glDeleteRenderbuffers(1, &gCap.drb);
        gCap = Capture();
    }

    bool makeGl(U32 w, U32 h)
    {
        freeGl();
        glGenTextures(1, &gCap.tex);
        glBindTexture(GL_TEXTURE_2D, gCap.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffers(1, &gCap.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, gCap.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gCap.tex, 0);
        const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glGenBuffers(kSlots, gCap.pbo);
        for (int i = 0; i < kSlots; ++i)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, gCap.pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        if (!ok)
        {
            LL_WARNS("WolfVR") << "big screen: could not make the capture framebuffer" << LL_ENDL;
            freeGl();
            return false;
        }
        gCap.w = w;
        gCap.h = h;
        // [VR DEPTH] the picture's depth grid (3D Mode: two eyes side by side, or the centre view): 1/8 each way
        if (w >= 64 && h >= 64)
        {
            gCap.dw = w / 8;
            gCap.dh = h / 8;
            glGenRenderbuffers(1, &gCap.drb);
            glBindRenderbuffer(GL_RENDERBUFFER, gCap.drb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)gCap.dw, (GLsizei)gCap.dh);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glGenFramebuffers(1, &gCap.dfbo);
            glBindFramebuffer(GL_FRAMEBUFFER, gCap.dfbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, gCap.drb);
            glDrawBuffer(GL_NONE);
            glReadBuffer(GL_NONE);
            const bool dok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glGenBuffers(kSlots, gCap.dpbo);
            for (int i = 0; i < kSlots; ++i)
            {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, gCap.dpbo[i]);
                glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)gCap.dw * gCap.dh * 4, nullptr, GL_STREAM_READ);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            if (!dok)
            {
                LL_WARNS("WolfVR") << "3D Mode: could not make the depth framebuffer; the headset gets pictures without depth" << LL_ENDL;
                gCap.dw = gCap.dh = 0;
            }
        }
        return true;
    }

    // The window, fitted into the picture keeping its shape (centred, black bars).
    S32 gSrc[4] = {0, 0, 0, 0};   // the part of the window that is copied (GL pixels)
    void fitWindow(S32 ww, S32 wh)
    {
        if (gCap.world)
        {
            // 3D mode: the camera's vertical field of view spans the WORLD view (the window less the menu bar and
            // anything docked: LLViewerWindow::getWorldViewRectRaw, what the 3D projection is set up for), so its
            // centre square is exactly the square 100-degree view. Seen 2026-11-03: the whole-window square had the
            // black menu-bar strip along its top.
            const LLRect wr = gViewerWindow->getWorldViewRectRaw();
            const S32 side = llmax(1, llmin(wr.getWidth(), wr.getHeight()));
            gSrc[0] = wr.mLeft + (wr.getWidth() - side) / 2;
            gSrc[1] = wr.mBottom + (wr.getHeight() - side) / 2;
            gSrc[2] = gSrc[0] + side;
            gSrc[3] = gSrc[1] + side;
            if (gCrop.on)
            {
                // [VR 3D FIX 2026-11-04] the eye's own window was drawn across the whole world view (eyeCrop)
                gSrc[0] = wr.mLeft;
                gSrc[1] = wr.mBottom;
                gSrc[2] = wr.mRight;
                gSrc[3] = wr.mTop;
            }
            gCap.win_w = ww;
            gCap.win_h = wh;
            gCap.x0 = gCap.y0 = 0;
            gCap.x1 = (S32)gCap.w;
            gCap.y1 = (S32)gCap.h;
            if (gEye >= 0)
            {
                // [VR STEREO] the eye's own square half of the side-by-side picture (left half = left eye)
                gCap.x0 = gEye * (S32)gCap.h;
                gCap.x1 = gCap.x0 + (S32)gCap.h;
            }
            return;
        }
        gSrc[0] = gSrc[1] = 0;
        gSrc[2] = ww;
        gSrc[3] = wh;
        gCap.win_w = ww;
        gCap.win_h = wh;
        const F64 sx = (F64)gCap.w / ww, sy = (F64)gCap.h / wh, s = llmin(sx, sy);
        const S32 fw = (S32)(ww * s + 0.5), fh = (S32)(wh * s + 0.5);
        gCap.x0 = ((S32)gCap.w - fw) / 2;
        gCap.y0 = ((S32)gCap.h - fh) / 2;
        gCap.x1 = gCap.x0 + fw;
        gCap.y1 = gCap.y0 + fh;
    }

    // DOM KeyboardEvent.key names (https://developer.mozilla.org/docs/Web/API/UI_Events/Keyboard_event_key_values)
    // to the viewer's own key codes (indra_constants.h). Printable characters go as typed characters instead.
    KEY domKey(const std::string& k)
    {
        static const std::pair<const char*, KEY> kMap[] = {
            {"Enter", KEY_RETURN}, {"Backspace", KEY_BACKSPACE}, {"Tab", KEY_TAB}, {"Escape", KEY_ESCAPE},
            {"ArrowLeft", KEY_LEFT}, {"ArrowRight", KEY_RIGHT}, {"ArrowUp", KEY_UP}, {"ArrowDown", KEY_DOWN},
            {"Delete", KEY_DELETE}, {"Home", KEY_HOME}, {"End", KEY_END}, {"PageUp", KEY_PAGE_UP},
            {"PageDown", KEY_PAGE_DOWN}, {"Insert", KEY_INSERT},
        };
        for (const auto& [name, key] : kMap)
        {
            if (k == name) return key;
        }
        return KEY_NONE;
    }

    MASK maskOf(U32 mods)
    {
        MASK m = MASK_NONE;
        if (mods & 1) m |= MASK_SHIFT;
        if (mods & 2) m |= MASK_CONTROL;
        if (mods & 4) m |= MASK_ALT;
        return m;
    }

    // A point on the picture (0..1 from its top-left) to the viewer window (GL coordinates, origin bottom-left).
    LLCoordGL toWindow(F32 u, F32 v)
    {
        const S32 ww = gViewerWindow->getWindowWidthRaw(), wh = gViewerWindow->getWindowHeightRaw();
        F64 fx = 0.5, fy = 0.5;
        if (gCap.x1 > gCap.x0 && gCap.y1 > gCap.y0)
        {
            fx = (u * gCap.w - gCap.x0) / (F64)(gCap.x1 - gCap.x0);
            fy = (v * gCap.h - gCap.y0) / (F64)(gCap.y1 - gCap.y0);
        }
        fx = llclamp(fx, 0.0, 1.0);
        fy = llclamp(fy, 0.0, 1.0);
        const S32 x = llclamp((S32)(fx * ww), 0, ww - 1);
        const S32 y = llclamp((S32)((1.0 - fy) * wh), 0, wh - 1);
        return LLCoordGL(x, y);
    }
}

namespace WolfVRScreen
{
void release()
{
    freeGl();
    freeLayer();
}

// [VR HUD LAYER 2026-11-04] End of render_ui (the right eye's / the only pass): draw the UI layer again into our own
// target, transparent where there is no UI, and send it. Everything the desktop shows over the world is in it - HUD
// attachments (ship helms, flight decks, game HUDs, residents' own), floaters, notices - so every HUD works in the
// headset with no code of its own.
void captureLayer()
{
    if (gSnapshot || gCubeSnapshot || gEye == 0 || !gViewerWindow) return;
    if (!WolfVRStream::layerWanted())
    {
        if (gLayer.w) freeLayer();
        return;
    }
    const F64 now = LLTimer::getTotalSeconds();
    // collect the oldest finished read first (every frame, so the fences never pile up)
    const int old = (gLayer.next + 1) % kSlots;
    if (gLayer.fence[old] && glClientWaitSync(gLayer.fence[old], 0, 0) != GL_TIMEOUT_EXPIRED)
    {
        glDeleteSync(gLayer.fence[old]);
        gLayer.fence[old] = nullptr;
        GLint pack = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, gLayer.pbo[old]);
        const size_t row = (size_t)gLayer.w * 4, bytes = row * gLayer.h;
        if (const uint8_t* p = (const uint8_t*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT))
        {
            gLayer.buf.resize(bytes);
            gLayer.alpha.resize((size_t)gLayer.w * gLayer.h);
            for (U32 y = 0; y < gLayer.h; ++y)   // GL rows are bottom first; the stream wants top first
            {
                const uint8_t* src = p + (size_t)(gLayer.h - 1 - y) * row;
                std::memcpy(&gLayer.buf[(size_t)y * row], src, row);
                uint8_t* a = &gLayer.alpha[(size_t)y * gLayer.w];
                uint8_t* d = &gLayer.buf[(size_t)y * row];
                for (U32 x = 0; x < gLayer.w; ++x)
                {
                    // [VR HUD ALPHA] a pixel with colour but no alpha is a HUD attachment's (drawn with alpha writes off
                    // by the pipeline): opaque, as it is on the desktop
                    uint8_t al = src[4 * x + 3];
                    if (!al && (src[4 * x] | src[4 * x + 1] | src[4 * x + 2]) > 3) { al = 255; d[4 * x + 3] = 255; }
                    a[x] = al;
                }
            }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            gLayer.aw = gLayer.w;
            gLayer.ah = gLayer.h;
            WolfVRStream::submitLayer(gLayer.buf, gLayer.w, gLayer.h, (now - gLayer.issued[old]) * 1000.0);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    }
    // 30 a second while something on it moves; 8 a second while it stands still (a change is then seen within 1/8 s,
    // and the rate goes back up at once) - the UI drawn a second time costs the viewer about 2 ms each time
    if (now - gLayer.last < (WolfVRStream::layerIdle() ? 1.0 / 8.0 : 1.0 / 30.0)) return;
    gLayer.last = now;

    const U32 ww = (U32)gViewerWindow->getWindowWidthRaw(), wh = (U32)gViewerWindow->getWindowHeightRaw();
    if (ww < 16 || wh < 16) return;
    if (ww != gLayer.ww || wh != gLayer.wh || !gLayer.rt.isComplete())
    {
        freeLayer();
        if (!gLayer.rt.allocate(ww, wh, GL_RGBA8, true))
        {
            LL_WARNS("WolfVR") << "UI layer: could not make the target " << ww << "x" << wh << LL_ENDL;
            return;
        }
        // The picture: the window scaled to fit 1600 x 900 (multiples of 16), sent stacked with its coverage as one
        // JPEG (wolfvrpipe.cpp layerLoop); a bigger desktop window is sharper on the PC, not in the headset.
        // [VR HUD JPEG 2026-11-04] at most 1600 x 900: about 22 pixels a degree on the 72-degree band, near the Quest
        // 3's own 25, as a JPEG picture of a few hundred KB when the UI changes
        const F64 sc = llmin(1.0, llmin(1600.0 / ww, 900.0 / wh));
        const U32 lw = llmax(16u, (U32)(ww * sc) / 16 * 16), lh = llmax(16u, (U32)(wh * sc) / 16 * 16);
        glGenTextures(1, &gLayer.stex);
        glBindTexture(GL_TEXTURE_2D, gLayer.stex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)lw, (GLsizei)lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        GLint prev = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
        glGenFramebuffers(1, &gLayer.sfbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gLayer.sfbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gLayer.stex, 0);
        const bool ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev);
        if (!ok)
        {
            LL_WARNS("WolfVR") << "UI layer: could not make the scaled copy " << lw << "x" << lh << LL_ENDL;
            freeLayer();
            return;
        }
        glGenBuffers(kSlots, gLayer.pbo);
        for (int i = 0; i < kSlots; ++i)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, gLayer.pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)lw * lh * 4, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        gLayer.ww = ww;
        gLayer.wh = wh;
        gLayer.w = lw;
        gLayer.h = lh;
    }

    const F64 t0 = LLTimer::getTotalSeconds();
    const bool eye_rect = gEyeRect.active;
    if (eye_rect) useEyeRect(false);   // [VR RENDER SIZE] the HUDs and windows are laid out on the whole window
    gLayer.rt.bindTarget();
    gGL.setColorMask(true, true);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    gLayer.rt.clear();
    gGL.setColorMask(true, true);
    // [VR HUD ALPHA] test only (WolfVRDebugUIAlphaOff): the 2D UI drawn with alpha writes off, as the pipeline leaves HUD
    // attachments - so the emulator, whose test avatar wears no HUD, checks that such pixels still reach the headset
    static LLCachedControl<bool> alpha_off(gSavedSettings, "WolfVRDebugUIAlphaOff", false);
    if (alpha_off) gGL.setColorMask(true, false);
    {
        LLGLSDefault gls_default;
        LLGLSUIDefault gls_ui;
        gPipeline.disableLights();
        render_ui_2d();
    }
    gGL.flush();
    {
        // [VR LAYER ALLOW-LIST 2026-11-04] Paul: "if i turn on huds and things appear i do get a lot of other junk". In his
        // video (frames 3-70) the junk was the navigation and favourites bars (inside the world view, so the old
        // "outside the world view" clear never took them), the vehicle icon bar bottom right, and the VR window's own QR
        // code. Now only what is useful in a headset is kept and everything else of the 2D UI is cleared: the windows
        // (each becomes its own panel), menus and pie menus, popups (a combo box's list), notices (toasts), and the Wolf
        // instruments drawn by the viewer (flight deck, car dashboard, ship's helm, game HUD, border banner, loading
        // notice). Never the VR window itself. HUD attachments are drawn AFTER this clear (below), so they all stay.
        const LLVector2 ds = gViewerWindow->getDisplayScale();
        std::vector<LLRect> keep;
        std::string kept;   // debug only: what was kept (WolfVRDebugStats)
        const S32 area = (S32)gLayer.ww * (S32)gLayer.wh;
        // a view kept as its rectangle; a view as big as half the window (a holder or an overlay that spans the screen,
        // such as the Wolf instruments' own full-screen views) is not taken whole: its visible children are, a few
        // levels down, so only what it really draws is kept
        std::function<void(LLView*, int)> add = [&](LLView* v, int depth) {
            if (!v || !v->getVisible() || !v->isInVisibleChain()) return;
            const LLRect r = v->calcScreenRect();
            const LLRect raw(ll_round(r.mLeft * ds.mV[VX]), ll_round(r.mTop * ds.mV[VY]), ll_round(r.mRight * ds.mV[VX]),
                             ll_round(r.mBottom * ds.mV[VY]));
            if (raw.getWidth() <= 0 || raw.getHeight() <= 0) return;
            if (!dynamic_cast<LLFloater*>(v) && (S64)raw.getWidth() * raw.getHeight() * 2 > area)
            {
                if (depth < 4)
                    for (LLView* c : *v->getChildList()) add(c, depth + 1);
                return;
            }
            keep.push_back(raw);
            if (gDebugStats && kept.size() < 900) kept += llformat("%s %dx%d; ", v->getName().c_str(), raw.getWidth(), raw.getHeight());
        };
        if (gFloaterView)
        {
            for (LLView* v : *gFloaterView->getChildList())
            {
                LLFloater* f = dynamic_cast<LLFloater*>(v);
                if (f && f->isShown() && f->getInstanceName() != "wolf_vr_stream") add(f, 0);
            }
        }
        if (gMenuHolder)
            for (LLView* v : *gMenuHolder->getChildList()) add(v, 0);
        for (auto& t : LLInstanceTracker<LLNotificationsUI::LLToast>::instance_snapshot()) add(&t, 0);
        static std::vector<LLHandle<LLView>> s_named;
        static bool s_found = false;
        if (!s_found && gViewerWindow->getRootView())
        {
            s_found = true;
            for (const char* n : {"popup_holder", "wolf_flight_deck", "wolf_car_dashboard", "wolf_helm_hud", "wolf_game_hud",
                                  "wolf_border_banner", "wolf_loading_notice"})
                if (LLView* v = gViewerWindow->getRootView()->findChildView(n, true)) s_named.push_back(v->getHandle());
        }
        for (const auto& h : s_named)
        {
            LLView* v = h.get();
            if (!v) continue;
            if (std::string(v->getName()) == "popup_holder") { for (LLView* c : *v->getChildList()) add(c, 1); }
            else add(v, 0);
        }
        static F64 s_kept_log = 0;
        if (gDebugStats && LLTimer::getTotalSeconds() - s_kept_log > 10.0)
        {
            s_kept_log = LLTimer::getTotalSeconds();
            LL_INFOS("WolfVR") << "UI layer keeps: " << kept << LL_ENDL;   // debug only, every 10 s
        }
        // clear the rest: the window cut into a grid at every kept edge, each grid cell outside every kept rectangle
        // cleared (runs of them along a row at once) - at most a few hundred scissored clears
        const S32 W = (S32)gLayer.ww, H = (S32)gLayer.wh;
        std::vector<S32> xs = {0, W}, ys = {0, H};
        for (const LLRect& r : keep)
        {
            xs.push_back(llclamp(r.mLeft, 0, W)); xs.push_back(llclamp(r.mRight, 0, W));
            ys.push_back(llclamp(r.mBottom, 0, H)); ys.push_back(llclamp(r.mTop, 0, H));
        }
        std::sort(xs.begin(), xs.end()); xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
        std::sort(ys.begin(), ys.end()); ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
        const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
        glEnable(GL_SCISSOR_TEST);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        for (size_t j = 0; j + 1 < ys.size(); ++j)
        {
            const S32 cy = (ys[j] + ys[j + 1]) / 2;
            S32 run = -1;
            for (size_t i = 0; i + 1 <= xs.size(); ++i)
            {
                bool covered = true;
                if (i + 1 < xs.size())
                {
                    const S32 cx = (xs[i] + xs[i + 1]) / 2;
                    covered = std::any_of(keep.begin(), keep.end(),
                                          [&](const LLRect& r) { return cx >= r.mLeft && cx < r.mRight && cy >= r.mBottom && cy < r.mTop; });
                }
                if (!covered && run < 0) run = (S32)i;
                if (covered && run >= 0)
                {
                    glScissor(xs[run], ys[j], xs[i] - xs[run], ys[j + 1] - ys[j]);
                    glClear(GL_COLOR_BUFFER_BIT);
                    run = -1;
                }
            }
        }
        if (!sc) glDisable(GL_SCISSOR_TEST);
    }
    // the HUD attachments last, over what was kept: they are the residents' own instruments and all stay.
    // [VR HUD ALPHA 2026-11-04] render_hud_attachments ends in LLPipeline::renderGeomPostDeferred, which leaves alpha writes
    // OFF (gGL.setColorMask(true, false), pipeline.cpp) when HUD attachments are worn - Paul wears one - so their pixels
    // have colour and no alpha: they get their coverage from their colour when the picture is collected (above).
    render_hud_attachments();
    gGL.setColorMask(true, true);
    gGL.flush();
    // scale it down into the copy that is read (still bottom row first)
    GLint read_fb = 0, draw_fb = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gLayer.rt.getFBO());
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gLayer.sfbo);
    glBlitFramebuffer(0, 0, (GLint)gLayer.ww, (GLint)gLayer.wh, 0, 0, (GLint)gLayer.w, (GLint)gLayer.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gLayer.sfbo);
    // the read, without waiting: into the slot after the one just collected
    const int slot = gLayer.next;
    gLayer.next = (gLayer.next + 1) % kSlots;
    if (gLayer.fence[slot]) { glDeleteSync(gLayer.fence[slot]); gLayer.fence[slot] = nullptr; }
    GLint pack = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, gLayer.pbo[slot]);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, (GLsizei)gLayer.w, (GLsizei)gLayer.h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gLayer.fence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    gLayer.issued[slot] = now;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    gLayer.rt.flush();
    gGL.setColorMask(true, false);
    if (eye_rect) useEyeRect(true);
    sendPanels();   // [VR PANELS]

    if (!gDebugStats) return;   // [VR LEAN] no measuring unless asked for
    gLayer.cost += LLTimer::getTotalSeconds() - t0;
    gLayer.cost_n++;
    if (!gLayer.since) gLayer.since = now;
    if (now - gLayer.since >= 10.0)
    {
        LL_INFOS("WolfVR") << "UI layer: " << gLayer.w << "x" << gLayer.h << ", " << gLayer.cost_n / (now - gLayer.since)
                           << " passes/s, " << gLayer.cost / llmax(1.0, gLayer.cost_n) * 1000.0 << " ms each (CPU)" << LL_ENDL;
        gLayer.cost = gLayer.cost_n = 0;
        gLayer.since = now;
    }
}

namespace
{
// [VR 3D FIX 2026-11-04] Test only (WolfVRDebugEyeDump, switches itself off): the two eyes of one stereo picture as
// PNG files in the logs folder, and how they were drawn, so the depth can be measured on the PC (disparity).
void maybeDumpEyes(const std::vector<uint8_t>& rgba, U32 w, U32 h, const wolfvr::HeadPose& hp)
{
    static LLCachedControl<bool> dump(gSavedSettings, "WolfVRDebugEyeDump", false);
    if (!dump || !hp.stereo || w != 2 * h || rgba.size() < (size_t)w * h * 4) return;
    gSavedSettings.setBOOL("WolfVRDebugEyeDump", false);
    const U32 ew = w / 2;
    for (int e = 0; e < 2; ++e)
    {
        LLPointer<LLImageRaw> raw = new LLImageRaw((U16)ew, (U16)h, 4);
        U8* dst = raw->getData();
        // LLImageRaw keeps the bottom row first (GL order); the picture is top row first
        for (U32 y = 0; y < h; ++y)
            std::memcpy(dst + (size_t)y * ew * 4, &rgba[((size_t)(h - 1 - y) * w + (size_t)e * ew) * 4], (size_t)ew * 4);
        LLPointer<LLImagePNG> png = new LLImagePNG;
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, e ? "wolfvr_eye_right.png" : "wolfvr_eye_left.png");
        if (!png->encode(raw, 0.f) || !png->save(path)) LL_WARNS("WolfVR") << "eye dump: could not write " << path << LL_ENDL;
    }
    const LLVector3 eyes = avatarEyes();
    LL_INFOS("WolfVR") << "eye dump: " << ew << "x" << h << " per eye; eye offsets (head space, m) L " << hp.eye[0] << ","
                       << hp.eye[1] << "," << hp.eye[2] << " R " << hp.eye[3] << "," << hp.eye[4] << "," << hp.eye[5]
                       << "; windows (tan L,R,B,T) L " << hp.tan[0] << "," << hp.tan[1] << "," << hp.tan[2] << "," << hp.tan[3]
                       << " R " << hp.tan[4] << "," << hp.tan[5] << "," << hp.tan[6] << "," << hp.tan[7] << (hp.has_tan ? "" : " (not sent)")
                       << "; head centre " << gMap.base << " avatar eyes " << eyes << " (" << (gMap.base - eyes).length()
                       << " m lean); the desktop's own camera (mode " << (S32)gAgentCamera.getCameraMode() << ") would be at "
                       << gAgent.getPosAgentFromGlobal(gAgentCamera.calcCameraPositionTargetGlobal()) << ", "
                       << (gAgent.getPosAgentFromGlobal(gAgentCamera.calcCameraPositionTargetGlobal()) - gMap.base).length()
                       << " m from the eyes (not used in the headset)" << LL_ENDL;
}

// [VR READBACK 2026-11-04] Measured (scripts/wolfvr/e2e_stages.mjs): a picture used to wait in its pixel buffer until
// the capture two frames later collected it - two whole frames, 100-200 ms at the 10-20 frames a second of a busy
// region, the biggest single delay between the head moving and the headset showing it. Now the NEWEST read whose copy
// has finished is collected as soon as it has: right after it is issued (rarely done yet), and at the start of the next
// frame (pumpInput, the idle loop), by when the GPU has finished it. An older one still in flight is dropped for it.
void collectCapture()
{
    if (!gCap.fbo || !gCap.w || !gCap.h) return;
    int best = -1;
    for (int i = 0; i < kSlots; ++i)
    {
        if (!gCap.fence[i]) continue;
        const GLenum r = glClientWaitSync(gCap.fence[i], 0, 0);
        if (r == GL_WAIT_FAILED) { glDeleteSync(gCap.fence[i]); gCap.fence[i] = nullptr; continue; }
        if (r == GL_TIMEOUT_EXPIRED) continue;
        if (best < 0 || gCap.order[i] > gCap.order[best]) best = i;
    }
    if (best < 0) return;
    for (int i = 0; i < kSlots; ++i)
    {
        // older than the one collected: never wanted any more (a newer picture is ready)
        if (i != best && gCap.fence[i] && gCap.order[i] < gCap.order[best]) { glDeleteSync(gCap.fence[i]); gCap.fence[i] = nullptr; }
    }
    const int old = best;
    const bool world = gCap.world;
    GLint pack = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glDeleteSync(gCap.fence[old]);
    gCap.fence[old] = nullptr;
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, gCap.pbo[old]);
        const size_t bytes = (size_t)gCap.w * gCap.h * 4;
        if (const void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT))
        {
            gCap.buf.resize(bytes);
            std::memcpy(gCap.buf.data(), p, bytes);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            const F64 ms = (LLTimer::getTotalSeconds() - gCap.issued[old]) * 1000.0;
            gCap.pose[old].t_collect = wolfvr::clockMs();   // [VR STAGES]
            if (world) maybeDumpEyes(gCap.buf, gCap.w, gCap.h, gCap.pose[old]);
            gCap.dbuf.clear();
            if (world && gCap.has_depth[old])
            {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, gCap.dpbo[old]);
                const size_t dbytes = (size_t)gCap.dw * gCap.dh * 4;
                if (const void* d = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)dbytes, GL_MAP_READ_BIT))
                {
                    gCap.dbuf.resize((size_t)gCap.dw * gCap.dh);
                    std::memcpy(gCap.dbuf.data(), d, dbytes);
                    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                }
            }
            if (world) WolfVRStream::submitWorldFrame(gCap.buf, gCap.pose[old], ms, gCap.dbuf, gCap.dbuf.empty() ? 0 : gCap.dw,
                                                      gCap.dbuf.empty() ? 0 : gCap.dh, gCap.znear[old], gCap.zfar[old]);
            else WolfVRStream::submitFrame(gCap.buf, ms);
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
}

// Big screen: the whole window, letterboxed. 3D: the centre square, drawn for gFramePose.
void captureInto(bool world)
{
    if (!gViewerWindow) return;
    if (world ? !WolfVRStream::worldWanted() : !WolfVRStream::bigScreenWanted()) return;
    U32 w = 0, h = 0;
    WolfVRStream::frameSize(w, h);
    if (!w || !h) return;
    // [VR STEREO] The right eye only finishes a picture whose left half was drawn this frame.
    const bool stereo = world && gEye >= 0;
    if (stereo && w != 2 * h) return;   // the stream is switching size: wait for the new picture size
    if (stereo && gEye == 1 && !gCap.eye0) return;
    // At most 75 pictures a second (the stream is 60): a viewer running faster does not need to copy every frame.
    const F64 now = LLTimer::getTotalSeconds();
    if (!(stereo && gEye == 1))
    {
        if (now - gCap.last < 1.0 / 75.0) return;
        gCap.last = now;
    }
    if (w != gCap.w || h != gCap.h || world != gCap.world)
    {
        if (!makeGl(w, h)) return;
        gCap.world = world;
        gCap.last = now;
    }
    const S32 ww = gViewerWindow->getWindowWidthRaw(), wh = gViewerWindow->getWindowHeightRaw();
    if (ww <= 0 || wh <= 0) return;
    const bool refit = ww != gCap.win_w || wh != gCap.win_h;
    if (refit || world) fitWindow(ww, wh);   // 3D: the world view can change without the window changing (docking)

    // Keep the viewer's GL state as we found it.
    GLint read_fb = 0, draw_fb = 0, pack = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (srgb) glDisable(GL_FRAMEBUFFER_SRGB);       // copy the pixels as they are on screen
    if (scissor) glDisable(GL_SCISSOR_TEST);

    // 1. back buffer -> picture texture, scaled, upside down (so the read below comes out top row first)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gCap.fbo);
    if (refit)
    {
        GLfloat cc[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);   // the letterbox bars
        glClearColor(cc[0], cc[1], cc[2], cc[3]);
    }
    glBlitFramebuffer(gSrc[0], gSrc[1], gSrc[2], gSrc[3], gCap.x0, gCap.y1, gCap.x1, gCap.y0, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // [VR DEPTH 2026-11-04] this eye's depth into its half of the depth grid (bottom row first, as GL keeps it; the
    // stream flips it). The scene's depth buffer is the same size as the window here (deferredScreen is allocated at
    // the window's size, LLPipeline::allocateScreenBuffer), scaled in case it is not.
    static LLCachedControl<bool> send_depth(gSavedSettings, "WolfVRDepth", true);
    if (world && gCrop.on && gCap.dw && gCap.dfbo && send_depth)
    {
        LLRenderTarget& ds = gPipeline.mRT->deferredScreen;
        // [VR RENDER SIZE] deferredScreen is the size of the WORLD VIEW (LLPipeline::resizeScreenTexture), which is the
        // eye's square while in VR 3D Mode: window pixels -> its pixels through the world view rectangle
        const LLRect wv = gViewerWindow->getWorldViewRectRaw();
        const F32 sx = (F32)ds.getWidth() / (F32)llmax(1, wv.getWidth()), sy = (F32)ds.getHeight() / (F32)llmax(1, wv.getHeight());
        const S32 half = stereo ? (S32)gCap.dw / 2 : (S32)gCap.dw;   // the centre view has the whole grid
        const S32 eye_x = stereo ? gEye * half : 0;
        if (!gCap.depth_checked) while (glGetError() != GL_NO_ERROR) {}   // once: start the check clean
        glBindFramebuffer(GL_READ_FRAMEBUFFER, ds.getFBO());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gCap.dfbo);
        glBlitFramebuffer(ll_round((gSrc[0] - wv.mLeft) * sx), ll_round((gSrc[1] - wv.mBottom) * sy), ll_round((gSrc[2] - wv.mLeft) * sx),
                          ll_round((gSrc[3] - wv.mBottom) * sy),
                          eye_x, 0, eye_x + half, (GLint)gCap.dh, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        if (!gCap.depth_checked)
        {
            // once: a depth blit the driver refuses (formats not matching after all) means no depth, never wrong depth
            gCap.depth_checked = true;
            gCap.depth_ok = glGetError() == GL_NO_ERROR;
            if (!gCap.depth_ok) LL_WARNS("WolfVR") << "3D Mode: the scene's depth could not be copied; pictures go without it" << LL_ENDL;
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gCap.fbo);
    }

    if (stereo && gEye == 0)
    {
        // [VR STEREO] left half done; the right eye's pass reads the whole picture back
        gCap.eye0 = true;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
        if (read_fb == 0) glReadBuffer(GL_BACK);
        if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        return;
    }
    gCap.eye0 = false;

    // 2. picture texture -> pixel buffer, without waiting
    const int slot = gCap.next;
    gCap.next = (gCap.next + 1) % kSlots;
    if (gCap.fence[slot])
    {
        // Still not collected after kSlots frames: the GPU is behind. Drop the old one rather than wait for it.
        glDeleteSync(gCap.fence[slot]);
        gCap.fence[slot] = nullptr;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gCap.fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, gCap.pbo[slot]);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, (GLsizei)gCap.w, (GLsizei)gCap.h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gCap.has_depth[slot] = world && gCrop.on && gCap.dw && gCap.dfbo && gCap.depth_ok && send_depth;
    if (gCap.has_depth[slot])
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gCap.dfbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, gCap.dpbo[slot]);
        glReadPixels(0, 0, (GLsizei)gCap.dw, (GLsizei)gCap.dh, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        gCap.znear[slot] = gCrop.zn;
        gCap.zfar[slot] = gCrop.zf;
    }
    gCap.fence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    gCap.issued[slot] = now;
    gCap.order[slot] = ++gCap.issues;
    gCap.pose[slot] = gFramePose;
    gCap.pose[slot].t_frame = gStamp.frame;   // [VR STAGES]
    gCap.pose[slot].t_eye0 = stereo ? gStamp.eye0 : 0.0;
    gCap.pose[slot].t_issue = wolfvr::clockMs();

    // 3. [VR READBACK 2026-11-04] start the copy now, and collect whatever has finished (collectCapture)
    glFlush();
    collectCapture();

    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    if (read_fb == 0) glReadBuffer(GL_BACK);
    if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

}

void applyEvents(const std::vector<wolfvr::InputEvent>& events);

namespace
{
// [VR WORLD POINTER 2026-11-04] Between frames the camera is the head as it was last drawn (the head centre, its axes,
// the symmetric view): the headset's pointer is projected with it and the viewer's picks (pickImmediate in the idle
// loop's hover and clicks, the queued picks at the start of display()) see the world from the same place.
void setPickCamera()
{
    if (!gMap.valid) return;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    LLVector3 at = headToAgent(gMap, 0.f, 0.f, -1.f), lft = headToAgent(gMap, -1.f, 0.f, 0.f), u = headToAgent(gMap, 0.f, 1.f, 0.f);
    at.normVec(); lft.normVec(); u.normVec();
    cam->setAxes(at, lft, u);
    cam->setOrigin(gMap.base);
    cam->setViewNoBroadcast(gMap.view);
}
}

// [VR LEAN 2026-11-04] Paul: "If you need to pause the main viewer's screen while in VR you can do that". In VR 3D Mode
// (WolfVRPauseDesktop, on by default) the desktop window is not drawn or shown: no UI pass, no swap, except one
// ordinary frame every few seconds so the window is never a dead, frozen one. Big Screen Mode always draws it (it IS
// the stream). gPaused is decided once per frame in displayFrame.
bool uiSkipped() { return gEye == 0 || gPaused; }
bool secondEye() { return gEye == 1; }
// [VR SHARED CULL 2026-11-04] The eyes are 6.4 cm apart and the left eye is culled with the smallest SYMMETRIC frustum
// that holds its window (applyHeadCamera), which on a headset whose eyes mirror each other also holds the right eye's
// window: what the left eye's cull and state sort found is what the right eye sees, less a 6.4 cm sliver at the far
// edge. So the right eye's pass draws those lists again from its own position instead of culling and sorting the whole
// region a second time (the main thread's work per eye, measured by e2e_stages.mjs). WolfVRShareCull off: as before.
bool shareCull()
{
    static LLCachedControl<bool> share(gSavedSettings, "WolfVRShareCull", true);
    return gEye == 1 && share;
}

// [VR STEREO 2026-11-04] The frame, mono or one pass per eye. The left eye is a whole display() whose picture goes only
// to the headset (swap() does not show it: gDisplaySwapBuffers false, the way a tiled snapshot renders, and render_ui
// stops after the world, uiSkipped); the right eye is the ordinary frame that is shown on the desktop. Each eye is a
// full render - culling, shadows, sky, water, glow - from its own position, so all of it is right for that eye.
void displayFrame()
{
    static bool s_read = false;
    if (!s_read) { s_read = true; gDebugStats = gSavedSettings.getBOOL("WolfVRDebugStats"); }
    static LLCachedControl<bool> pause_desktop(gSavedSettings, "WolfVRPauseDesktop", true);
    U32 w = 0, h = 0;
    WolfVRStream::frameSize(w, h);
    const bool world = LLStartUp::getStartupState() == STATE_STARTED && !gDisconnected && WolfVRStream::worldWanted() &&
                       WolfVRStream::headPose().valid;
    const bool stereo = world && w && w == 2 * h;
    // the desktop: paused in 3D Mode, with one real frame every 3 s; and while paused nothing waits for the screen's
    // refresh any more, so the frames are paced here to the stream's 60 a second rather than run flat out
    static F64 s_last_shown = 0, s_last_frame = 0;
    const F64 now0 = LLTimer::getTotalSeconds();
    const bool show_now = now0 - s_last_shown >= 3.0;
    gPaused = world && pause_desktop && !show_now;
    if (!gPaused) s_last_shown = now0;
    if (gPaused)
    {
        const F64 wait = 1.0 / 60.0 - (now0 - s_last_frame);
        if (wait > 0.0005) ms_sleep((U32)(wait * 1000.0));
    }
    s_last_frame = LLTimer::getTotalSeconds();
    const F64 t0 = s_last_frame;
    gStamp.frame = wolfvr::clockMs();   // [VR STAGES]
    gStamp.eye0 = 0;
    F64 t1 = t0;
    if (world) setPickCamera();   // the queued picks (display(): performPick) from the head, as the hover ones were
    // [VR VIEW 2026-11-04] "Hide my avatar": not drawn while the headset is in VR 3D Mode - the stock switches mouselook
    // uses (LLAgent::setShowAvatar -> needsRenderAvatar, read when the avatar is drawn; the attachments' mouselook
    // visibility, LLVOAvatarSelf::updateAttachmentVisibility), set for this frame and given back after it
    static LLCachedControl<bool> hide_me(gSavedSettings, "WolfVRHideAvatar", false);
    const bool hiding = world && hide_me && isAgentAvatarValid();
    const bool shown_before = gAgent.getShowAvatar();
    if (hiding)
    {
        gAgent.setShowAvatar(false);
        gAgentAvatarp->updateAttachmentVisibility(CAMERA_MODE_MOUSELOOK);
    }
    // [VR RENDER SIZE] the world's area for this frame: the eye's own square while in VR 3D Mode
    static LLCachedControl<bool> eye_size(gSavedSettings, "WolfVREyeRenderSize", true);
    const LLRect real = gViewerWindow->getWorldViewRectRaw();
    const S32 side = llmin((S32)h, real.getHeight(), real.getWidth()) / 8 * 8;
    if (world && eye_size && side >= 256)
    {
        const LLRect over(real.mLeft, real.mBottom + side, real.mLeft + side, real.mBottom);
        if (!gEyeRect.active || over != gEyeRect.over) gResizeScreenTexture = true;   // the targets follow, once
        gEyeRect.active = true;
        gEyeRect.real = real;
        gEyeRect.over = over;
        useEyeRect(true);
    }
    else if (gEyeRect.active)
    {
        gEyeRect.active = false;
        gResizeScreenTexture = true;   // back to the window's own size
    }
    if (!stereo)
    {
        if (gPaused) gDisplaySwapBuffers = false;   // not shown; swap() sets it back
        display();
        gDisplaySwapBuffers = true;
    }
    else
    {
        gEye = 0;
        gDisplaySwapBuffers = false;
        display();
        t1 = LLTimer::getTotalSeconds();
        gStamp.eye0 = wolfvr::clockMs();   // [VR STAGES]
        gEye = 1;
        // set again (an early return in the left eye's display() skips swap()); paused: the right eye is not shown either
        gDisplaySwapBuffers = !gPaused;
        gDepthDirty = true;           // the depth buffer is the other eye's; do not trust it for occlusion
        display();
        gEye = -1;
        gDisplaySwapBuffers = true;
    }
    useEyeRect(false);   // [VR RENDER SIZE] the real area for everything between frames (UI, picks, pointer)
    if (hiding && isAgentAvatarValid())
    {
        gAgent.setShowAvatar(shown_before);
        gAgentAvatarp->updateAttachmentVisibility(gAgentCamera.getCameraMode());
    }
    if (world) setPickCamera();   // the head centre again for the rest of the frame and the next idle loop
    const bool was_paused = gPaused;
    gPaused = false;
    if (!gDebugStats) return;   // [VR LEAN] no measuring unless asked for
    // Numbers every 10 s while the headset is in 3D Mode: the cost of the frame, for comparing mono and stereo.
    const F64 t2 = LLTimer::getTotalSeconds();
    if (!WolfVRStream::worldWanted()) { gTimes = FrameTimes(); return; }
    if (gTimes.n && gTimes.stereo != stereo) gTimes = FrameTimes();
    if (!gTimes.n) gTimes.since = t0;
    gTimes.stereo = stereo;
    gTimes.sum += t2 - t0;
    gTimes.eye0 += t1 - t0;
    gTimes.n++;
    if (t2 - gTimes.since >= 10.0)
    {
        const F64 span = t2 - gTimes.since;
        LL_INFOS("WolfVR") << "VR 3D " << (stereo ? "stereo" : "mono") << ": " << gTimes.n / span << " frames/s, display "
                           << gTimes.sum / gTimes.n * 1000.0 << " ms/frame"
                           << (stereo ? " (left eye " + llformat("%.2f", gTimes.eye0 / gTimes.n * 1000.0) + " ms)" : std::string())
                           << ", picture " << w << "x" << h << (was_paused ? ", desktop paused" : ", desktop drawn") << LL_ENDL;
        gTimes = FrameTimes();
    }
}

void captureFrame() { captureInto(false); }

void captureWorld()
{
    if (gSnapshot || gCubeSnapshot) return;   // a snapshot or reflection-probe pass is not the headset's view
    captureInto(true);
    gCrop.on = false;   // the headset's window is for the world only: the HUDs and the UI after it are drawn as always
}

// [VR 3D 2026-11-03] Turn this frame's camera to the headset's pose: the avatar's heading (yaw only) plus the head's
// own rotation, so turning the avatar still turns the view and the head looks around in it.
// WebXR space is right-handed, +Y up, -Z forward (wolfvrpipe_video.h Quat); the viewer's is X forward (at), Y left,
// Z up (llcoordframe.h). A vector (x, y, z) in head space is therefore -z forward, -x left, +y up.
// [VR 3D FIX 2026-11-04] First person at the avatar's eyes (buildMap), and in stereo each eye's own off-axis frustum.
void applyHeadCamera()
{
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const bool want = !gSnapshot && !gCubeSnapshot && WolfVRStream::worldWanted();
    // [VR STEREO] the right eye uses the left eye's head pose, so the two halves of one picture always match
    wolfvr::HeadPose hp = gEye == 1 ? gFramePose : want ? WolfVRStream::headPose() : wolfvr::HeadPose();
    if (!want) hp.valid = false;
    gCrop.on = false;
    if (!hp.valid)
    {
        if (gCameraOverridden)
        {
            cam->setViewNoBroadcast(gSavedView);
            gCameraOverridden = false;
        }
        gFramePose = wolfvr::HeadPose();
        gMap.valid = false;
        gLean.set = false;
        gRig.set = false;   // out of VR: the next session takes the avatar's facing again
        return;
    }
    if (!gCameraOverridden)
    {
        gSavedView = cam->getView();
        gCameraOverridden = true;
    }
    if (gEye != 1) buildMap(hp);   // once per frame: the right eye keeps the left eye's head
    LLVector3 at = headToAgent(gMap, 0.f, 0.f, -1.f), lft = headToAgent(gMap, -1.f, 0.f, 0.f), u = headToAgent(gMap, 0.f, 1.f, 0.f);
    at.normVec(); lft.normVec(); u.normVec();
    cam->setAxes(at, lft, u);
    hp.stereo = gEye >= 0;
    if (gEye >= 0 && hp.has_tan)
    {
        // This eye's window [L, R] x [B, T] (tangents) fills the whole world view. The camera itself stays the smallest
        // symmetric one that contains it (vertical tangent V, horizontal V * aspect), so culling, LOD and picking see
        // everything the eye does; the projection is that one, scaled and shifted onto the window (eyeCrop, applied
        // in LLViewerCamera::setPerspective the way its own zoomed tiles are).
        const F32* w = hp.tan + 4 * gEye;
        const F32 A = llmax(0.1f, cam->getAspect());
        const F32 V = llmax(llmax(std::fabs(w[2]), std::fabs(w[3])), llmax(std::fabs(w[0]), std::fabs(w[1])) / A);
        cam->setViewNoBroadcast(2.f * std::atan(V));
        gCrop.on = true;
        gCrop.ax = 2.f * V * A / (w[1] - w[0]);
        gCrop.bx = -(w[1] + w[0]) / (w[1] - w[0]);
        gCrop.ay = 2.f * V / (w[3] - w[2]);
        gCrop.by = -(w[3] + w[2]) / (w[3] - w[2]);
        hp.fov_deg = 2.f * std::atan(V) * RAD_TO_DEG;
    }
    else if (gEye < 0 && hp.has_tan)
    {
        // [VR CENTRE 2026-11-04] one view for both eyes (WolfVRStereo off): drawn from the head's centre for the window
        // that covers both eyes' frusta; with its depth the headset makes each eye's view from it (vrdepth.js), at
        // half the PC's cost of two real eyes
        F32 w[4] = {llmin(hp.tan[0], hp.tan[4]), llmax(hp.tan[1], hp.tan[5]), llmin(hp.tan[2], hp.tan[6]), llmax(hp.tan[3], hp.tan[7])};
        const F32 A = llmax(0.1f, cam->getAspect());
        const F32 V = llmax(llmax(std::fabs(w[2]), std::fabs(w[3])), llmax(std::fabs(w[0]), std::fabs(w[1])) / A);
        cam->setViewNoBroadcast(2.f * std::atan(V));
        gCrop.on = true;
        gCrop.ax = 2.f * V * A / (w[1] - w[0]);
        gCrop.bx = -(w[1] + w[0]) / (w[1] - w[0]);
        gCrop.ay = 2.f * V / (w[3] - w[2]);
        gCrop.by = -(w[3] + w[2]) / (w[3] - w[2]);
        hp.fov_deg = 2.f * std::atan(V) * RAD_TO_DEG;
        std::memcpy(hp.tan, w, sizeof w);   // the header's first window is this one (the page reads it as the centre's)
    }
    else
    {
        // mono (or a page without the eye windows): the square picture is 100 degrees across both ways, or what the
        // headset measured covers both its eyes' frusta (wolfvr.js eyeFov); the viewer's FOV is vertical
        const F32 fov = hp.fov_deg > 0.f ? hp.fov_deg : 100.f;
        cam->setViewNoBroadcast(fov * DEG_TO_RAD);
        hp.fov_deg = fov;
        hp.has_tan = false;
    }
    if (gEye >= 0)
    {
        // [VR STEREO 2026-11-04] this eye's own position: the head centre plus the eye's offset in head space, turned
        // with the head. Axes stay parallel: WebXR gives both eyes the head's orientation on Quest.
        const F32* e = hp.eye + 3 * gEye;
        cam->setOrigin(gMap.base + headToAgent(gMap, e[0], e[1], e[2]));
    }
    else
    {
        cam->setOrigin(gMap.base);
    }
    gFramePose = hp;
    gMap.view = cam->getView();
}

void noteDepthRange(F32 znear, F32 zfar)
{
    gCrop.zn = znear;
    gCrop.zf = zfar;
}

bool eyeCrop(F32& ax, F32& bx, F32& ay, F32& by)
{
    // on from applyHeadCamera until the world for the headset is captured (captureWorld), never for the UI after it
    if (!gCrop.on) return false;
    ax = gCrop.ax; bx = gCrop.bx; ay = gCrop.ay; by = gCrop.by;
    return true;
}

bool pointerActive() { return gPointerUsed > 0 && LLTimer::getTotalSeconds() - gPointerUsed < 2.0; }

// [VR AUDIO 2026-11-04] The ears follow the head: audio_update_listener (llvieweraudio.cpp, idle, before display() turns
// the camera to the head) asks here for the newest head pose's axes, the same mapping as applyHeadCamera, so a sound
// stays where it is in the world when the head turns.
bool listenerAxes(LLVector3& at, LLVector3& up)
{
    if (gSnapshot || gCubeSnapshot || !WolfVRStream::worldWanted()) return false;
    const wolfvr::HeadPose hp = WolfVRStream::headPose();
    if (!hp.valid) return false;
    HeadMap m;
    m.fwd = gRig.set ? gRig.fwd : avatarHeading();   // the rig's heading: the ears turn with the view
    m.up = LLVector3::z_axis;
    m.left = m.up % m.fwd;
    std::memcpy(m.q, hp.q, sizeof m.q);
    at = headToAgent(m, 0.f, 0.f, -1.f);
    up = headToAgent(m, 0.f, 1.f, 0.f);
    at.normVec();
    up.normVec();
    return true;
}

namespace
{
// A point (agent coordinates) to the window pixel the viewer's pick turns back into a ray through it: the exact inverse
// of LLViewerWindow::mouseDirectionGlobal (UI-scaled coordinates: the vertical field of view across the world view's
// height, centred on it), then to raw pixels (LLViewerWindow::handleAnyMouseClick divides by the display scale).
bool projectToWindow(const LLVector3& p, LLCoordGL& out)
{
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 v = p - cam->getOrigin();
    const F32 a = v * cam->getAtAxis();
    if (a < 0.05f) return false;
    const LLRect wr = gViewerWindow->getWorldViewRectScaled();
    const F32 dist = (F32)wr.getHeight() * 0.5f / std::tan(cam->getView() * 0.5f);
    const F32 xs = (F32)wr.getCenterX() - dist * (v * cam->getLeftAxis()) / a;
    const F32 ys = (F32)wr.getCenterY() + dist * (v * cam->getUpAxis()) / a;
    if (xs < wr.mLeft || xs > wr.mRight || ys < wr.mBottom || ys > wr.mTop) return false;
    const LLVector2 sc = gViewerWindow->getDisplayScale();
    out = LLCoordGL(ll_round(xs * sc.mV[VX]), ll_round(ys * sc.mV[VY]));
    return true;
}

LLCoordGL layerToWindow(F32 u, F32 v)
{
    const S32 ww = gViewerWindow->getWindowWidthRaw(), wh = gViewerWindow->getWindowHeightRaw();
    return LLCoordGL(llclamp((S32)(u * ww), 0, ww - 1), llclamp((S32)((1.f - v) * wh), 0, wh - 1));
}

const char* cursorName()
{
    switch (gViewerWindow->getWindow()->getCursor())
    {
    case UI_CURSOR_HAND: return "touch";
    case UI_CURSOR_TOOLSIT: return "sit";
    case UI_CURSOR_TOOLBUY: return "buy";
    case UI_CURSOR_TOOLPAY: return "pay";
    case UI_CURSOR_TOOLOPEN: return "open";
    case UI_CURSOR_TOOLPLAY: case UI_CURSOR_TOOLPAUSE: case UI_CURSOR_TOOLMEDIAOPEN: return "media";
    case UI_CURSOR_IBEAM: return "text";
    case UI_CURSOR_TOOLGRAB: case UI_CURSOR_GRABLOCKED: return "grab";
    default: return "";
    }
}

// Where the pointer is this frame: what it is on (a press keeps what it was pressed on until it is let go), the window
// pixel, and the distance along the ray for the headset's dot. Then the mouse is moved there and the page told.
void updatePointer(bool force)
{
    const F64 now = LLTimer::getTotalSeconds();
    const bool live = gPtr.heard > 0 && now - gPtr.heard < 0.5 && gMap.valid && WolfVRStream::worldWanted();
    const bool held = gPtr.held[0] != Aim::NONE || gPtr.held[2] != Aim::NONE;
    if (!live && !held && gPtr.aim == Aim::NONE && gPtr.idle) return;   // nothing to do until a ray comes (lean)
    if (!live && !held)
    {
        if (gPtr.aim != Aim::NONE) { gPtr.aim = Aim::NONE; gPtr.pos_ok = false; }
    }
    else
    {
        LLCoordGL layer_pos;
        const bool on_layer = gPtr.has_layer && (layer_pos = layerToWindow(gPtr.lu, gPtr.lv), layerHasPixel(layer_pos.mX, layer_pos.mY));
        Aim aim = held ? (gPtr.held[0] != Aim::NONE ? gPtr.held[0] : gPtr.held[2]) : on_layer ? Aim::UI : Aim::WORLD;
        LLCoordGL pos;
        bool ok = false;
        if (aim == Aim::UI)
        {
            pos = gPtr.has_layer ? layerToWindow(gPtr.lu, gPtr.lv) : gPtr.pos;
            ok = true;
            gPtr.dist = -1.f;
        }
        else
        {
            // the ray in the viewer's world: reference space -> the head as last drawn (gMap) -> agent coordinates
            const F32 ro[3] = {gPtr.o[0] - (gMap.has_p ? gMap.p[0] : 0.f), gPtr.o[1] - (gMap.has_p ? gMap.p[1] : 0.f),
                               gPtr.o[2] - (gMap.has_p ? gMap.p[2] : 0.f)};
            const LLVector3 O = gMap.base + refToAgent(gMap, ro[0], ro[1], ro[2]);
            LLVector3 D = refToAgent(gMap, gPtr.d[0], gPtr.d[1], gPtr.d[2]);
            D.normVec();
            if (force || now - gPtr.cast_at >= 1.0 / 30.0)   // the ray test at most 30 times a second
            {
                gPtr.cast_at = now;
                LLVector4a start, end, hit;
                start.load3(O.mV);
                end.load3((O + D * kReach).mV);
                S32 face = -1;
                LLViewerObject* obj = gPipeline.lineSegmentIntersectInWorld(start, end, false, false, true, false, &face,
                                                                            nullptr, nullptr, &hit, nullptr, nullptr, nullptr);
                if (obj)
                {
                    LLVector3 h(hit.getF32ptr());
                    gPtr.dist = (h - O).length();
                    gPtr.prim = obj->getPCode() == LL_PCODE_VOLUME;
                }
                else
                {
                    gPtr.dist = -1.f;
                    gPtr.prim = false;
                }
            }
            const LLVector3 target = O + D * (gPtr.dist > 0.f ? gPtr.dist : kReach);
            if (gDebugStats) { gPtr.dbg_dir = target - LLViewerCamera::getInstance()->getOrigin(); gPtr.dbg_dir.normVec(); }
            ok = projectToWindow(target, pos);
            if (!ok && held) { pos = gPtr.pos; ok = gPtr.pos_ok; }
        }
        if (!ok) aim = Aim::NONE;
        if (ok && (!gPtr.pos_ok || pos.mX != gPtr.pos.mX || pos.mY != gPtr.pos.mY || aim != gPtr.aim))
        {
            gViewerWindow->handleMouseMove(gViewerWindow->getWindow(), pos, MASK_NONE);
        }
        gPtr.aim = aim;
        gPtr.pos = pos;
        gPtr.pos_ok = ok;
    }
    // tell the page where the ray landed and what the viewer would do there (its cursor), when it changes
    const char* kind = gPtr.aim == Aim::UI ? "ui" : gPtr.aim == Aim::WORLD ? "world" : "none";
    const char* cur = gPtr.aim == Aim::NONE ? "" : cursorName();
    const std::string msg = llformat("{\"t\":\"aim\",\"k\":\"%s\",\"d\":%.2f,\"c\":\"%s\",\"o\":%d}", kind,
                                     gPtr.aim == Aim::WORLD ? gPtr.dist : -1.f, cur, gPtr.aim == Aim::WORLD && gPtr.prim ? 1 : 0);
    if (msg != gPtr.sent && (force || now - gPtr.sent_at >= 1.0 / 30.0))
    {
        gPtr.sent = msg;
        gPtr.sent_at = now;
        WolfVRStream::sendText(msg);
    }
    gPtr.idle = gPtr.aim == Aim::NONE && msg == gPtr.sent;
}

// A press or release of the 3D pointer: on what the ray is on now (a press), or on what the press was on (a release).
void pointerButton(const wolfvr::InputEvent& e)
{
    const int bi = e.button == 2 ? 2 : 0;
    LLWindow* window = gViewerWindow->getWindow();
    if (e.type == wolfvr::InputEvent::Type::DOWN)
    {
        if (gPtr.held[bi] != Aim::NONE) return;
        updatePointer(true);
        if (gPtr.aim == Aim::NONE || !gPtr.pos_ok) return;
        gPtr.held[bi] = gPtr.aim;
        const bool dbg_mouse = LLView::sDebugMouseHandling;
        if (gDebugStats) LLView::sDebugMouseHandling = true;   // [VR LEAN] debug only: the viewer logs who took the press
        const bool handled = bi == 2 ? gViewerWindow->handleRightMouseDown(window, gPtr.pos, MASK_NONE)
                                     : gViewerWindow->handleMouseDown(window, gPtr.pos, MASK_NONE);
        LLView::sDebugMouseHandling = dbg_mouse;
        if (gDebugStats)
        {
            // [VR LEAN] debug only: what one press did (one line per press, never per frame)
            const LLPickInfo& pk = LLToolPie::getInstance()->getPick();
            LL_INFOS("WolfVR") << "pointer " << (bi == 2 ? "right" : "left") << " down on " << (gPtr.aim == Aim::UI ? "UI" : "world")
                               << " at " << gPtr.pos.mX << "," << gPtr.pos.mY << " (" << gPtr.dist << " m), handled " << handled
                               << ", tool " << LLToolMgr::getInstance()->getCurrentTool()->getName() << ", last pick type "
                               << (S32)pk.mPickType << " object " << (pk.getObject() ? pk.getObject()->getID().asString() : std::string("none"))
                               << ", camera at " << LLViewerCamera::getInstance()->getOrigin() << " view " << LLViewerCamera::getInstance()->getView()
                               << ", ray through the pixel " << gViewerWindow->mouseDirectionGlobal(ll_round(gPtr.pos.mX / gViewerWindow->getDisplayScale().mV[VX]),
                                                                                                   ll_round(gPtr.pos.mY / gViewerWindow->getDisplayScale().mV[VY]))
                               << " towards the hit " << gPtr.dbg_dir << " display scale " << gViewerWindow->getDisplayScale()
                               << ", mouse captor " << (gFocusMgr.getMouseCapture() ? gFocusMgr.getMouseCapture()->getName() : std::string("none"))
                               << LL_ENDL;
        }
    }
    else
    {
        if (gPtr.held[bi] == Aim::NONE) return;
        updatePointer(true);
        gPtr.held[bi] = Aim::NONE;
        if (bi == 2) gViewerWindow->handleRightMouseUp(window, gPtr.pos, MASK_NONE);
        else gViewerWindow->handleMouseUp(window, gPtr.pos, MASK_NONE);
    }
}
}

void pumpInput()
{
    WolfVRStream::pumpModeRequest();
    if (!gViewerWindow) return;
    if (gCap.fbo) collectCapture();   // [VR READBACK 2026-11-04] last frame's picture, as soon as its copy is done
    const std::vector<wolfvr::InputEvent> events = WolfVRStream::takeInput();
    applyEvents(events);
    updatePointer(false);   // [VR WORLD POINTER] every frame while the ray is heard (and to let go when it is not)
    // the held stick, every frame
    const F64 now = LLTimer::getTotalSeconds();
    if (now - gStick.heard > 1.0) { gStick.x = gStick.y = 0.f; gStick.lift = 0; }
    const bool moving = std::fabs(gStick.x) > kDead || std::fabs(gStick.y) > kDead;
    if (!gAgent.isMovementLocked() && isAgentAvatarValid())
    {
        // stick y is negative pushed forward (xr-standard); moveLeft(+1) = left, stick x is positive to the right
        if (gStick.y < -kDead) gAgent.moveAt(1);
        else if (gStick.y > kDead) gAgent.moveAt(-1);
        if (gStick.x < -kDead) gAgent.moveLeft(1);
        else if (gStick.x > kDead) gAgent.moveLeft(-1);
        if (gStick.lift) gAgent.moveUp(gStick.lift);
    }
    const bool run = moving && llmax(std::fabs(gStick.x), std::fabs(gStick.y)) > kRun;
    if (run && !gStick.ran) { gAgent.setTempRun(); gStick.ran = true; }
    else if (!run && gStick.ran) { gAgent.clearTempRun(); gStick.ran = false; }
}

void applyEvents(const std::vector<wolfvr::InputEvent>& events)
{
    if (events.empty()) return;
    LLWindow* window = gViewerWindow->getWindow();
    using T = wolfvr::InputEvent::Type;
    for (const wolfvr::InputEvent& e : events)
    {
        const MASK mask = maskOf(e.mods);
        if (e.type == T::MOVE || e.type == T::DOWN || e.type == T::UP || e.type == T::WHEEL || e.type == T::RAY)
            gPointerUsed = LLTimer::getTotalSeconds();
        if (e.type == T::RAY)
        {
            std::memcpy(gPtr.o, e.o, sizeof gPtr.o);
            std::memcpy(gPtr.d, e.d, sizeof gPtr.d);
            gPtr.has_layer = e.has_layer;
            gPtr.lu = e.u;
            gPtr.lv = e.v;
            gPtr.heard = LLTimer::getTotalSeconds();
            continue;
        }
        if (e.world && (e.type == T::DOWN || e.type == T::UP))
        {
            pointerButton(e);   // [VR WORLD POINTER]
            continue;
        }
        // [VR HUD LAYER] a point on the UI layer is a point on the whole window (no letterbox)
        auto where = [&](const wolfvr::InputEvent& ev) {
            if (!ev.layer) return toWindow(ev.u, ev.v);
            const S32 ww = gViewerWindow->getWindowWidthRaw(), wh = gViewerWindow->getWindowHeightRaw();
            return LLCoordGL(llclamp((S32)(ev.u * ww), 0, ww - 1), llclamp((S32)((1.f - ev.v) * wh), 0, wh - 1));
        };
        switch (e.type)
        {
        case T::MOVE:
            gViewerWindow->handleMouseMove(window, where(e), mask);
            break;
        case T::DOWN:
        case T::UP:
        {
            const LLCoordGL pos = where(e);
            // [VR HUD LAYER] a press where the layer shows nothing would land on the world behind the desktop's view,
            // not on anything the resident can see in the headset: ignore it (and its release)
            const int bi = e.button == 2 ? 2 : 0;
            if (e.layer && e.type == T::DOWN && !layerHasPixel(pos.mX, pos.mY)) { gLayerDownIgnored[bi] = true; break; }
            if (e.type == T::UP && gLayerDownIgnored[bi]) { gLayerDownIgnored[bi] = false; break; }
            gViewerWindow->handleMouseMove(window, pos, mask);
            if (e.button == 2)
            {
                if (e.type == T::DOWN) gViewerWindow->handleRightMouseDown(window, pos, mask);
                else gViewerWindow->handleRightMouseUp(window, pos, mask);
            }
            else
            {
                if (e.type == T::DOWN) gViewerWindow->handleMouseDown(window, pos, mask);
                else gViewerWindow->handleMouseUp(window, pos, mask);
            }
            break;
        }
        case T::WHEEL:
            gViewerWindow->handleMouseMove(window, where(e), mask);
            gViewerWindow->handleScrollWheel(window, e.wheel);
            break;
        case T::KEY_DOWN:
        case T::KEY_UP:
        {
            if (!gKeyboard) break;
            KEY key = domKey(e.key);
            if (key == KEY_NONE && e.key.size() == 1 && (mask & (MASK_CONTROL | MASK_ALT)))
            {
                // a shortcut such as Ctrl+F: the viewer's key codes for letters and digits are upper-case ASCII
                const char c = e.key[0];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                {
                    key = (KEY)((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
                }
            }
            if (key == KEY_NONE)
            {
                // A printable character without Ctrl/Alt is typed text: the page also sends it as "text".
                break;
            }
            if (e.type == T::KEY_DOWN) gKeyboard->handleTranslatedKeyDown(key, mask);
            else gKeyboard->handleTranslatedKeyUp(key, mask);
            break;
        }
        case T::STICK:
            gStick.x = e.x;
            gStick.y = e.y;
            gStick.heard = LLTimer::getTotalSeconds();
            break;
        case T::LIFT:
            gStick.lift = e.wheel;
            gStick.heard = LLTimer::getTotalSeconds();
            break;
        case T::TURN:
            // One snap turn of 45 degrees (WolfStorm render_manager.js _updateVRInput, Meta's comfort default);
            // LLAgent::yaw turns the body about the up vector, + = left.
            if (!gAgent.isMovementLocked())
            {
                const F32 a = (F32)e.wheel * 45.f * DEG_TO_RAD;
                gAgent.yaw(a);
                // [VR VIEW] the rig turns with it, by the same step (the only thing that turns the rig)
                gRig.fwd.rotVec(a, LLVector3::z_axis);
                gRig.fwd.mV[VZ] = 0.f;
                gRig.fwd.normVec();
            }
            break;
        case T::ACTION:
            WolfVRUi::run(e.key, e.text);   // [VR CONTROLS 2026-11-04] an action-bar button (wolfvrui.cpp)
            break;
        case T::RAY:
            break;   // handled above
        case T::TELEPORT:
        {
            // [VR TELEPORT 2026-11-04] Paul: "i need to press trigger twice and tp to the place i've pressed". The
            // desktop's double-click teleport (LLToolPie::teleportToClickedLocation, which refuses objects with a
            // click action and the resident's own avatar, and honours RLV): its hover pick taken fresh at the pointer
            updatePointer(true);
            if (gPtr.aim != Aim::WORLD || !gPtr.pos_ok) break;
            const LLVector2 sc = gViewerWindow->getDisplayScale();
            LLToolPie* pie = LLToolPie::getInstance();
            pie->handleHover(ll_round(gPtr.pos.mX / sc.mV[VX]), ll_round(gPtr.pos.mY / sc.mV[VY]), MASK_NONE);
            const bool went = pie->teleportToClickedLocation();
            if (gDebugStats)   // [VR LEAN] debug only, one line per double press
                LL_INFOS("WolfVR") << "pointer teleport at " << gPtr.pos.mX << "," << gPtr.pos.mY << ": pick type "
                                   << (S32)pie->getHoverPick().mPickType << ", " << (went ? "going" : "refused") << LL_ENDL;
            break;
        }
        case T::TEXT:
        {
            const LLWString w = utf8str_to_wstring(e.text);
            for (llwchar c : w)
            {
                if (c >= 32) gViewerWindow->handleUnicodeChar(c, MASK_NONE);
            }
            break;
        }
        }
    }
}
}
