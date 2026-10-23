// [WOLF TUNNELS 2026-10-23] Off-line checks of wolftunnelcamera.h (the pattern of wolfterraformmath_test.cpp: plain asserts, no
// viewer libraries). Build and run:
//   g++ -std=c++17 -O1 -Wall -Wextra -o /tmp/wtc indra/newview/tests/wolftunnelcamera_test.cpp && /tmp/wtc [boxes.txt]
// boxes.txt (optional): one axis-aligned box a line, "x0 y0 x1 y1 z0 z1" (the Moonfold railway plan's volumes); the avatar walks
// the line through their centres and the same checks run on it.
//
// Paul walks through a tunnel at walking speed, the camera behind him (CameraOffsetRearView -3, 0, 0.75: app_settings/
// settings.xml), and the camera is worked out every frame at 60 fps. The 10-21 keepCameraInside (copied below as the reference)
// is run beside the new one; the largest jump of the camera between two frames is what Paul sees as the flash.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include "../wolftunnelcamera.h"

using namespace WolfTunnelCamera;

namespace
{
    constexpr float FPS = 60.f;
    constexpr float WALK_MPS = 3.2f;          // avatar walk speed: OpenSim ScenePresence walking (~3.2 m/s)
    constexpr float BACK_M = 3.f, UP_M = 0.75f;   // CameraOffsetRearView (-3, 0, 0.75)
    constexpr float HEAD_M = 1.7f;            // head over the floor
    using Ground = std::function<float(float, float)>;

    // ---- the 10-21 keepCameraInside walk (wolffloatertunnels.cpp at git HEAD b501232492, lines 330-367), copied but for the LL
    // names: shrunk boxes (inBox grow -0.25), 0.2 m steps of the whole line, the open air at 0.25 m over the ground ------------------
    bool oldInBox(const Box& b, const Vec3& p, float grow)
    {
        const Vec3 l = toLocal(b, p);
        return std::fabs(l.x) <= b.s.x * 0.5f + grow && std::fabs(l.y) <= b.s.y * 0.5f + grow && std::fabs(l.z) <= b.s.z * 0.5f + grow;
    }
    Vec3 oldKeep(const std::vector<const Box*>& boxes, const Ground& ground, const Vec3& head, const Vec3& cam)
    {
        const float CAMERA_INSET = 0.25f, CAMERA_STEP = 0.2f;
        const Vec3 dir = cam - head;
        const float len = dir.length();
        auto inside = [&](const Vec3& p)
        {
            for (const Box* b : boxes) if (oldInBox(*b, p, -CAMERA_INSET)) return true;
            return p.z > ground(p.x, p.y) + CAMERA_INSET;
        };
        float keep = 0.f;
        const int steps = std::max(1, (int)std::ceil(len / CAMERA_STEP));
        for (int i = 1; i <= steps; ++i)
        {
            const float t = (float)i / steps;
            if (!inside(head + dir * t)) break;
            keep = t;
        }
        return head + dir * keep;
    }

    // ---- the new keepCameraInside's state, as wolffloatertunnels.cpp keeps it -----------------------------------------------------
    struct NewCam
    {
        float keep = 0.f;
        bool started = false;
        Vec3 last;
        Vec3 step(const std::vector<const Box*>& boxes, const Ground& ground, const Vec3& head, const Vec3& cam, float dt)
        {
            const Vec3 d = cam - head;
            const float len = d.length();
            const Vec3 unit = d * (1.f / len);
            const float target = allowedDistance(boxes, ground, head, unit, len);
            if (!started) { keep = std::min(std::max((last - head).length(), 0.f), len); started = true; }
            else keep = ease(keep, target, dt);
            keep = std::min(std::max(keep, 0.f), len);
            return head + unit * keep;
        }
    };

    bool headIn(const std::vector<const Box*>& boxes, const Vec3& head)
    {
        for (const Box* b : boxes) if (inBox(*b, head)) return true;
        return false;
    }

    // The camera with the head in no box: the wanted point, lifted to 0.5 m over the land (llagentcamera.cpp:2230-2252,
    // getCameraMinOffGround = 0.5) unless it is itself in a box (llagentcamera.cpp:2222-2225 wolf_in_tunnel = contains(cam)).
    Vec3 stock(const std::vector<const Box*>& boxes, const Ground& ground, Vec3 cam)
    {
        for (const Box* b : boxes) if (inBox(*b, cam)) return cam;
        const float min_z = ground(cam.x, cam.y) + 0.5f;
        if (cam.z < min_z) cam.z = min_z;
        return cam;
    }

    struct Walk { float old_jump = 0.f, new_jump = 0.f; int new_bad = 0; int frames = 0; };

    /** Walk the polyline pts (x, y, floor z) and compare the cameras frame by frame. */
    Walk walk(const std::vector<Box>& boxes, const Ground& ground, const std::vector<Vec3>& pts, const char* name)
    {
        std::vector<const Box*> ptrs;
        for (const Box& b : boxes) ptrs.push_back(&b);
        Walk w;
        Vec3 prev_old, prev_new, prev_wanted;
        bool have = false;
        // the avatar turns into each new leg over a moment, as a walking avatar does (our sim's own half-life, 0.15 s)
        Vec3 facing;
        bool facing_set = false;
        NewCam nc;
        const float dt = 1.f / FPS, ds = WALK_MPS / FPS;
        for (size_t k = 1; k < pts.size(); ++k)
        {
            const Vec3 a = pts[k - 1], b = pts[k];
            const Vec3 ab = b - a;
            const float seg = Vec3(ab.x, ab.y, 0.f).length();
            const Vec3 leg = Vec3(ab.x, ab.y, 0.f) * (1.f / seg);
            if (!facing_set) { facing = leg; facing_set = true; }
            for (float s = 0.f; s < seg; s += ds)
            {
                const float k = 1.f - std::pow(2.f, -dt / 0.15f);
                facing = facing + (leg - facing) * k;
                const Vec3 fwd = facing * (1.f / facing.length());
                const Vec3 foot = a + ab * (s / seg);
                const Vec3 head = foot + Vec3(0.f, 0.f, HEAD_M);
                const Vec3 wanted = head - fwd * BACK_M + Vec3(0.f, 0.f, UP_M);
                Vec3 cam_old, cam_new;
                if (headIn(ptrs, head))
                {
                    cam_old = oldKeep(ptrs, ground, head, wanted);
                    nc.last = have ? prev_new : stock(ptrs, ground, wanted);
                    cam_new = nc.step(ptrs, ground, head, wanted, dt);
                    // settled (not mid-glide) cameras are never in rock
                    if (std::fabs(nc.keep - allowedDistance(ptrs, ground, head, (wanted - head) * (1.f / (wanted - head).length()),
                                                            (wanted - head).length())) < 0.01f
                        && !covered(ptrs, ground, cam_new))
                        ++w.new_bad;
                }
                else
                {
                    cam_old = cam_new = stock(ptrs, ground, wanted);
                    nc.started = false;
                }
                if (have)
                {
                    // how much further the camera moved this frame than the wanted (stock) camera did: a follow camera does move
                    // with the avatar and swing as he turns; anything more is a jump
                    const float jw = (wanted - prev_wanted).length();
                    const float jo = (cam_old - prev_old).length() - jw, jn = (cam_new - prev_new).length() - jw;
                    w.old_jump = std::max(w.old_jump, jo);
                    w.new_jump = std::max(w.new_jump, jn);
                }
                prev_old = cam_old; prev_new = cam_new; prev_wanted = wanted; have = true;
                ++w.frames;
            }
        }
        std::printf("%-34s frames %5d  largest extra camera jump in a frame: old %.2f m, new %.3f m, settled cameras in rock: %d\n",
                    name, w.frames, w.old_jump, w.new_jump, w.new_bad);
        return w;
    }

    // ---- boxes the way the region's generator makes them (Source: wolfstorm/rust_proxy/src/wolftunnel.rs:1838-1896 volumes()):
    // one per route piece of at most VOL_PIECE_M = 16 m; pieces of one straight run overlap 0.1 m each way; past a bend each leg's
    // box reaches the band's reach further; the box is VOL_BELOW_M = 1 m under the floor, VOL_ABOVE_M = 0.3 m over the roof and
    // VOL_SIDE_M = 0.3 m beside the wall; sizes rounded up to the centimetre, centres to the centre-metre ------------------------
    constexpr float VOL_PIECE_M = 16.f, VOL_BELOW_M = 1.f, VOL_ABOVE_M = 0.3f, VOL_SIDE_M = 0.3f;
    float yawOf(float ux, float uy) { float a = std::atan2(uy, ux) / DEG_TO_RAD_F; return a < 0.f ? a + 360.f : a; }
    std::vector<Box> volumes(const std::vector<Vec3>& route, float hw, float top)
    {
        std::vector<Box> out;
        const float reach = hw + VOL_SIDE_M;   // a smooth tunnel: Air::reach() = hw (ro() = 0)
        const size_t n = route.size();
        for (size_t k = 1; k < n; ++k)
        {
            const Vec3 a = route[k - 1], b = route[k];
            const float dx = b.x - a.x, dy = b.y - a.y, seg = std::sqrt(dx * dx + dy * dy);
            const float ux = dx / seg, uy = dy / seg;
            const int pieces = std::max(1, (int)std::ceil(seg / VOL_PIECE_M));
            for (int p = 0; p < pieces; ++p)
            {
                float s0 = seg * p / pieces, s1 = seg * (p + 1) / pieces;
                if (p == 0 && k > 1) s0 -= reach;
                if (p + 1 == pieces && k + 1 < n) s1 += reach;
                if (p > 0) s0 -= 0.1f;
                if (p + 1 < pieces) s1 += 0.1f;
                const float grade = (b.z - a.z) / seg;
                const float fa = a.z + grade * s0, fb = a.z + grade * s1;
                const float lo = std::min(fa, fb) - VOL_BELOW_M, hi = std::max(fa, fb) + top + VOL_ABOVE_M;
                const float mid = (s0 + s1) * 0.5f;
                Box v;
                v.c = Vec3(a.x + ux * mid, a.y + uy * mid, (lo + hi) * 0.5f);
                v.s = Vec3(s1 - s0, reach * 2.f, hi - lo);
                v.yaw_deg = std::round(yawOf(ux, uy) * 10.f) / 10.f;
                v.s = Vec3(std::ceil(v.s.x * 100.f) / 100.f, std::ceil(v.s.y * 100.f) / 100.f, std::ceil(v.s.z * 100.f) / 100.f);
                v.c = Vec3(std::round(v.c.x * 100.f) / 100.f, std::round(v.c.y * 100.f) / 100.f, std::round(v.c.z * 100.f) / 100.f);
                out.push_back(v);
            }
        }
        return out;
    }

    /** The route with its ends moved m metres in: a deep test tunnel's ends are solid rock (no mouth), so the walk starts and
     *  ends inside, where a real walk would already be. */
    std::vector<Vec3> trimmed(std::vector<Vec3> r, float m)
    {
        auto in = [m](Vec3& end, const Vec3& next) { Vec3 d = next - end; const float l = Vec3(d.x, d.y, 0.f).length(); end = end + d * (m / l); };
        in(r.front(), r[1]);
        in(r.back(), r[r.size() - 2]);
        return r;
    }

    void testEase()
    {
        assert(ease(3.f, 1.f, 0.f) == 3.f);                       // no time, no change
        const float in1 = ease(3.f, 1.f, EASE_IN_S);              // one half-life: half way
        assert(std::fabs(in1 - 2.f) < 1e-4f);
        const float out1 = ease(1.f, 3.f, EASE_OUT_S);
        assert(std::fabs(out1 - 2.f) < 1e-4f);
        assert(ease(1.f, 3.f, 1.f / FPS) < ease(3.f, 1.f, 1.f / FPS) + 2.f);   // out is the slower
        assert(std::fabs(ease(1.f, 3.f, 100.f) - 3.f) < 1e-4f);  // a long time: there
    }

    void testJoin()
    {
        // One straight 48 m run (3 pieces, joins at 16 and 32 m), deep under a hill: the walk out from the head along the tunnel
        // passes the joins; the old shrunk boxes stopped at each one, the union does not.
        const std::vector<Vec3> route = { Vec3(10.f, 10.f, 20.f), Vec3(58.f, 10.f, 20.f) };
        const std::vector<Box> boxes = volumes(route, 2.f, 3.f);
        assert(boxes.size() == 3);
        std::vector<const Box*> ptrs;
        for (const Box& b : boxes) ptrs.push_back(&b);
        const Ground deep = [](float, float) { return 60.f; };
        const Vec3 head(26.5f, 10.f, 21.7f);                       // 0.5 m past the first join
        const Vec3 wanted = head + Vec3(-BACK_M, 0.f, UP_M);
        const Vec3 d = wanted - head;
        const float len = d.length();
        const float keep = allowedDistance(ptrs, deep, head, d * (1.f / len), len);
        assert(std::fabs(keep - len) < 1e-3f);                     // the whole way: the join is no wall
        const Vec3 old = oldKeep(ptrs, deep, head, wanted);
        std::printf("join 0.5 m behind the head: old keeps %.2f m of %.2f m, new %.2f m\n", (old - head).length(), len, keep);
        assert((old - head).length() < 1.f);                       // the bug, as it was
        // the wall still stops it: straight out sideways, 2 m to the wall, so 1.6 m less the ball (0.4 m) past it... the box reaches
        // 0.3 m past the wall, so the camera stays 0.1 m inside the air
        const float side = allowedDistance(ptrs, deep, head, Vec3(0.f, 1.f, 0.f), 5.f);
        assert(std::fabs(side - (2.f + VOL_SIDE_M - INSET_M)) < 0.01f);
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testEase();
    testJoin();

    const Ground deep = [](float, float) { return 60.f; };
    // a long straight tunnel (5 joins)
    {
        const std::vector<Vec3> route = { Vec3(10.f, 10.f, 20.f), Vec3(100.f, 10.f, 20.f) };
        const Walk w = walk(volumes(route, 2.f, 3.f), deep, trimmed(route, 6.f), "straight 90 m, 6 boxes");
        assert(w.old_jump > 1.f && w.new_jump < 0.15f && w.new_bad == 0);
    }
    // bends of 30, 60 and 90 degrees, a slope, a long leg
    {
        const std::vector<Vec3> route = { Vec3(10.f, 10.f, 20.f), Vec3(40.f, 10.f, 20.f), Vec3(66.f, 25.f, 22.f),
                                          Vec3(81.f, 51.f, 22.f), Vec3(55.f, 66.f, 21.f), Vec3(20.f, 66.f, 21.f) };
        const Walk w = walk(volumes(route, 2.f, 3.f), deep, trimmed(route, 6.f), "bends 30/60/90 + slope");
        assert(w.new_jump < 0.3f && w.new_bad == 0);
    }
    // a wide road tunnel (6 m) with a room (a cavern box, r 8 m) on it
    {
        const std::vector<Vec3> route = { Vec3(10.f, 10.f, 20.f), Vec3(70.f, 10.f, 20.f) };
        std::vector<Box> boxes = volumes(route, 3.f, 4.f);
        Box room;
        room.c = Vec3(40.f, 10.f, 20.f - VOL_BELOW_M + (6.f + VOL_ABOVE_M + VOL_BELOW_M) * 0.5f);
        room.s = Vec3(2.f * (8.f + VOL_SIDE_M), 2.f * (8.f + VOL_SIDE_M), 6.f + VOL_ABOVE_M + VOL_BELOW_M);
        boxes.push_back(room);
        const Walk w = walk(boxes, deep, trimmed(route, 6.f), "road 6 m with a room");
        assert(w.new_jump < 0.15f && w.new_bad == 0);
    }
    // a mouth: level ground at the floor outside, the hill rising 1 in 1 from the mouth (x = 30) to 15 m over the floor;
    // the avatar walks from 12 m outside to 30 m inside
    {
        const std::vector<Vec3> route = { Vec3(30.f, 10.f, 20.f), Vec3(80.f, 10.f, 20.f) };
        const Ground hill = [](float x, float) { return 20.f + std::min(std::max(x - 30.f, 0.f), 15.f); };
        const std::vector<Vec3> path = { Vec3(18.f, 10.f, 20.f), Vec3(60.f, 10.f, 20.f) };
        const Walk w = walk(volumes(route, 2.f, 3.f), hill, path, "mouth, walking in");
        const std::vector<Vec3> back = { Vec3(60.f, 10.f, 20.f), Vec3(18.f, 10.f, 20.f) };
        const Walk o = walk(volumes(route, 2.f, 3.f), hill, back, "mouth, walking out");
        assert(w.new_jump < 0.3f && o.new_jump < 0.3f && w.new_bad == 0 && o.new_bad == 0);
    }

    if (argc > 1)
    {
        std::ifstream f(argv[1]);
        std::vector<Box> boxes;
        std::vector<Vec3> path;
        std::string line;
        while (std::getline(f, line))
        {
            std::istringstream is(line);
            float x0, y0, x1, y1, z0, z1;
            if (!(is >> x0 >> y0 >> x1 >> y1 >> z0 >> z1)) continue;
            Box b;
            b.c = Vec3((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f);
            b.s = Vec3(x1 - x0, y1 - y0, z1 - z0);
            boxes.push_back(b);
            path.push_back(Vec3(b.c.x, b.c.y, z0 + 1.f));      // the floor taken 1 m over the box's bottom (VOL_BELOW_M)
        }
        // the file holds several tunnels (and halls): a chain is a run of boxes each within 12 m of the one before
        float worst_old = 0.f, worst_new = 0.f;
        int bad = 0, chains = 0;
        size_t i0 = 0;
        for (size_t i = 1; i <= path.size(); ++i)
        {
            if (i < path.size() && (path[i] - path[i - 1]).length() < 12.f) continue;
            if (i - i0 >= 4)
            {
                const std::string name = std::string(argv[1]).substr(std::string(argv[1]).find_last_of('/') + 1) + " chain " + std::to_string(++chains);
                const Walk w = walk(boxes, deep, std::vector<Vec3>(path.begin() + i0 + 1, path.begin() + i - 1), name.c_str());
                worst_old = std::max(worst_old, w.old_jump);
                worst_new = std::max(worst_new, w.new_jump);
                bad += w.new_bad;
            }
            i0 = i;
        }
        std::printf("%s: %d chain(s), worst extra jump old %.2f m, new %.3f m, settled cameras in rock %d\n", argv[1], chains, worst_old,
                    worst_new, bad);
    }
    std::printf("wolftunnelcamera_test: all checks passed\n");
    return 0;
}
