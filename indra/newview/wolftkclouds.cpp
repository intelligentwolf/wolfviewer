/**
 * @file wolftkclouds.cpp
 * @brief WolfViewer: takram three-geospatial's volumetric clouds, translated. See wolftkclouds.h (licence).
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial
 * (github.com/takram-design-engineering/three-geospatial, commit b012ad06d858fc035d88aacfd73f092f93c994e4),
 * MIT License, Copyright (c) 2024 Shota Matsuda (full notice in wolftkclouds.h).
 *
 * CascadedShadowMaps / FrustumCorners are "based on the following work with slight modifications.
 * https://github.com/StrandedKitty/three-csm/ https://github.com/mrdoob/three.js/tree/r169/examples/jsm/csm"
 * (CascadedShadowMaps.ts:1-3), carrying this notice:
 *
 * MIT License
 *
 * Copyright (c) 2019 vtHawk
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * three.js r171 (MIT License, Copyright (c) 2010-2024 three.js authors): Matrix4 lookAt / makeOrthographic /
 * makePerspective, Vector3 applyMatrix4, MathUtils.lerp, Box3 - their semantics reproduced below.
 */

// Source citations: "three-geospatial <path>:<lines>" = packages/<path> at the commit above; "three <file>:<lines>"
// = three.js r171 src/<file>.

#include "llviewerprecompiledheaders.h"

#include "wolftkclouds.h"
#include "wolfcloudgenus.h"   // ICE_G_SCALE (the shader's WOLF_ICE_G_SCALE)

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>

#include <glm/gtc/type_ptr.hpp>

#include "llgl.h"
#include "llglstates.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "pipeline.h"
#include "wolftkatmosphere.h"
#include "wolftkgl.h"

namespace WolfTk
{
namespace
{
    // ── constants ────────────────────────────────────────────────────────────────────────────────
    // Source: three-geospatial clouds/src/constants.ts:1-2
    constexpr S32 CLOUD_SHAPE_TEXTURE_SIZE = 128;
    constexpr S32 CLOUD_SHAPE_DETAIL_TEXTURE_SIZE = 32;
    // Source: three-geospatial clouds/src/LocalWeather.ts:12 (size 512), Turbulence.ts:12 (size 128)
    constexpr S32 LOCAL_WEATHER_SIZE = 512;
    constexpr S32 TURBULENCE_SIZE = 128;
    // Source: three-geospatial clouds/src/bayer.ts:4-9 bayerIndices
    const S32 BAYER_INDICES[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

    /** Source: three-geospatial clouds/src/bayer.ts:11-21 bayerOffsets */
    glm::vec2 bayerOffset(S32 index)
    {
        for (S32 i = 0; i < 16; ++i)
        {
            if (BAYER_INDICES[i] == index)
            {
                return glm::vec2(((i % 4) + 0.5f) / 4.f, (std::floor(i / 4.f) + 0.5f) / 4.f);
            }
        }
        return glm::vec2(0.f);
    }

    /** Source: three MathUtils.js:63-67 lerp */
    F64 lerp(F64 x, F64 y, F64 t) { return (1.0 - t) * x + t * y; }

    // ── three.js math, double precision ──────────────────────────────────────────────────────────
    /** Source: three math/Vector3.js:237-252 applyMatrix4 (with the perspective divide) */
    glm::dvec3 applyMatrix4(const glm::dvec3& v, const glm::dmat4& m)
    {
        const glm::dvec4 r = m * glm::dvec4(v, 1.0);
        return glm::dvec3(r) * (1.0 / r.w);
    }

    /** Source: three math/Matrix4.js:290-337 lookAt - sets the rotation part only (elements 0-2, 4-6, 8-10),
     *  the rest of `m` kept, as three does. glm is column-major like three's elements: m[c][r] = te[c * 4 + r]. */
    void lookAt(glm::dmat4& m, const glm::dvec3& eye, const glm::dvec3& target, const glm::dvec3& up)
    {
        glm::dvec3 z = eye - target;
        if (glm::dot(z, z) == 0.0) z.z = 1.0;
        z = glm::normalize(z);
        glm::dvec3 x = glm::cross(up, z);
        if (glm::dot(x, x) == 0.0)
        {
            if (std::abs(up.z) == 1.0) z.x += 0.0001; else z.z += 0.0001;
            z = glm::normalize(z);
            x = glm::cross(up, z);
        }
        x = glm::normalize(x);
        const glm::dvec3 y = glm::cross(z, x);
        m[0][0] = x.x; m[1][0] = y.x; m[2][0] = z.x;
        m[0][1] = x.y; m[1][1] = y.y; m[2][1] = z.y;
        m[0][2] = x.z; m[1][2] = y.z; m[2][2] = z.z;
    }

    /** Source: three math/Matrix4.js:813-852 makeOrthographic (WebGLCoordinateSystem) */
    glm::dmat4 makeOrthographic(F64 left, F64 right, F64 top, F64 bottom, F64 nearv, F64 farv)
    {
        const F64 w = 1.0 / (right - left), h = 1.0 / (top - bottom), p = 1.0 / (farv - nearv);
        const F64 x = (right + left) * w, y = (top + bottom) * h;
        const F64 z = (farv + nearv) * p, zInv = -2.0 * p;
        glm::dmat4 m(0.0);
        m[0][0] = 2.0 * w; m[3][0] = -x;
        m[1][1] = 2.0 * h; m[3][1] = -y;
        m[2][2] = zInv;    m[3][2] = -z;
        m[3][3] = 1.0;
        return m;
    }

    // Source: three core/Object3D.js:1020 Object3D.DEFAULT_UP = (0, 1, 0). In three-geospatial's clouds story the
    // world IS ECEF (useApplyLocation.tsx:24-32 puts the camera at geodetic.toECEF), so its light-space "up" is ECEF
    // +Y; the viewer's world is LL's agent frame, so the same ECEF +Y is handed in, turned into that world
    // (CascadedShadowMaps::update's `up`, from Clouds::render).
    const glm::dvec3 DEFAULT_UP_ECEF(0.0, 1.0, 0.0);

    /** Source: three-geospatial clouds/src/helpers/FrustumCorners.ts:24-132 FrustumCorners */
    struct FrustumCorners
    {
        glm::dvec3 nearc[4], farc[4];

        /** setFromCamera (:51-82), a perspective camera */
        void setFromCamera(const glm::dmat4& projectionMatrixInverse, F64 farv)
        {
            nearc[0] = glm::dvec3(1, 1, -1); nearc[1] = glm::dvec3(1, -1, -1); nearc[2] = glm::dvec3(-1, -1, -1); nearc[3] = glm::dvec3(-1, 1, -1);
            for (S32 i = 0; i < 4; ++i) nearc[i] = applyMatrix4(nearc[i], projectionMatrixInverse);
            farc[0] = glm::dvec3(1, 1, 1); farc[1] = glm::dvec3(1, -1, 1); farc[2] = glm::dvec3(-1, -1, 1); farc[3] = glm::dvec3(-1, 1, 1);
            for (S32 i = 0; i < 4; ++i)
            {
                glm::dvec3& corner = farc[i];
                corner = applyMatrix4(corner, projectionMatrixInverse);
                const F64 absZ = std::abs(corner.z);
                corner *= std::min(farv / absZ, 1.0);   // perspective branch (:76-78)
            }
        }

        /** split (:84-118) */
        void split(const std::vector<F64>& clipDepths, std::vector<FrustumCorners>& result) const
        {
            result.resize(clipDepths.size());
            for (size_t index = 0; index < clipDepths.size(); ++index)
            {
                FrustumCorners& frustum = result[index];
                for (S32 i = 0; i < 4; ++i)
                {
                    frustum.nearc[i] = index == 0 ? nearc[i] : glm::mix(nearc[i], farc[i], clipDepths[index - 1]);
                    frustum.farc[i] = index == clipDepths.size() - 1 ? farc[i] : glm::mix(nearc[i], farc[i], clipDepths[index]);
                }
            }
        }

        /** applyMatrix4 (:120-126) */
        void applyMatrix(const glm::dmat4& m)
        {
            for (S32 i = 0; i < 4; ++i) { nearc[i] = applyMatrix4(nearc[i], m); farc[i] = applyMatrix4(farc[i], m); }
        }
    };

    /** Source: three-geospatial clouds/src/helpers/splitFrustum.ts:37-45 'practical' (lerp of uniform and
     *  logarithmic splits by lambda) */
    void splitPractical(S32 count, F64 nearv, F64 farv, F64 lambda, std::vector<F64>& result)
    {
        result.resize(count);
        for (S32 i = 0; i < count; ++i)
        {
            const F64 uniform = (nearv + ((farv - nearv) * (i + 1)) / count) / farv;
            const F64 logarithmic = (nearv * std::pow(farv / nearv, (F64)(i + 1) / count)) / farv;
            result[i] = lerp(uniform, logarithmic, lambda);
        }
    }

    struct Cascade
    {
        glm::dvec2 interval;
        glm::dmat4 matrix, inverseMatrix, projectionMatrix, inverseProjectionMatrix, viewMatrix, inverseViewMatrix;
    };

    /** Source: three-geospatial clouds/src/CascadedShadowMaps.ts:79-288 CascadedShadowMaps */
    struct CascadedShadowMaps
    {
        std::vector<Cascade> cascades;
        S32 mapSize = 512;
        // Source: CascadedShadowMaps.ts:70-77 defaults (maxFar null, farScale 1, practical, margin 0, fade true);
        // splitLambda 0.6 from CloudsEffect.ts:250-254. maxFar: see Impl::shadowCamera.
        F64 maxFar = -1.0;   // < 0 = null
        F64 farScale = 1.0;
        F64 splitLambda = 0.6;
        F64 margin = 0.0;
        bool fade = true;
        FrustumCorners cameraFrustum;
        std::vector<FrustumCorners> frusta;
        std::vector<F64> splits;
        F64 far = 0.0;

        void setCascadeCount(S32 n) { cascades.resize(n, Cascade{ glm::dvec2(0.0), glm::dmat4(1.0), glm::dmat4(1.0), glm::dmat4(1.0), glm::dmat4(1.0), glm::dmat4(1.0), glm::dmat4(1.0) }); }

        /** updateIntervals (:144-163) */
        void updateIntervals(const glm::dmat4& projInv, F64 nearv)
        {
            const S32 count = (S32)cascades.size();
            splitPractical(count, nearv, far, splitLambda, splits);
            cameraFrustum.setFromCamera(projInv, far);
            cameraFrustum.split(splits, frusta);
            for (S32 i = 0; i < count; ++i)
            {
                cascades[i].interval = glm::dvec2(i > 0 ? splits[i - 1] : 0.0, splits[i]);
            }
        }

        /** getFrustumRadius (:165-187) */
        F64 getFrustumRadius(F64 nearv, const FrustumCorners& frustum) const
        {
            F64 diagonalLength = std::max(glm::distance(frustum.farc[0], frustum.farc[2]), glm::distance(frustum.farc[0], frustum.nearc[2]));
            if (fade)
            {
                const F64 distance = frustum.farc[0].z / (far - nearv);
                diagonalLength += 0.25 * distance * distance * (far - nearv);
            }
            return diagonalLength * 0.5;
        }

        /** updateMatrices (:189-256) */
        void updateMatrices(const glm::dmat4& matrixWorld, F64 nearv, const glm::dvec3& sunDirection, F64 distance, const glm::dvec3& up)
        {
            glm::dmat4 lightOrientationMatrix(1.0);
            lookAt(lightOrientationMatrix, glm::dvec3(0.0), sunDirection * -1.0, up);
            const glm::dmat4 cameraToLightMatrix = glm::inverse(lightOrientationMatrix) * matrixWorld;
            for (size_t i = 0; i < frusta.size(); ++i)
            {
                Cascade& cascade = cascades[i];
                const F64 radius = getFrustumRadius(nearv, frusta[i]);
                const F64 left = -radius, right = radius, top = radius, bottom = -radius;
                cascade.projectionMatrix = makeOrthographic(left, right, top, bottom, -margin, radius * 2.0 + margin);

                FrustumCorners f = frusta[i];
                f.applyMatrix(cameraToLightMatrix);
                // Source: three math/Box3.js makeEmpty / expandByPoint / getCenter
                glm::dvec3 bmin(std::numeric_limits<F64>::infinity()), bmax(-std::numeric_limits<F64>::infinity());
                for (S32 j = 0; j < 4; ++j)
                {
                    bmin = glm::min(bmin, f.nearc[j]); bmax = glm::max(bmax, f.nearc[j]);
                    bmin = glm::min(bmin, f.farc[j]); bmax = glm::max(bmax, f.farc[j]);
                }
                glm::dvec3 center = (bmin + bmax) * 0.5;
                center.z = bmax.z + margin;

                // Round light-space translation to even texel increments.
                const F64 texelWidth = (right - left) / mapSize;
                const F64 texelHeight = (top - bottom) / mapSize;
                center.x = std::round(center.x / texelWidth) * texelWidth;
                center.y = std::round(center.y / texelHeight) * texelHeight;

                center = applyMatrix4(center, lightOrientationMatrix);
                const glm::dvec3 position = sunDirection * distance + center;
                lookAt(cascade.inverseViewMatrix, center, position, up);
                // Source: three math/Matrix4.js:469-480 setPosition
                cascade.inverseViewMatrix[3][0] = position.x;
                cascade.inverseViewMatrix[3][1] = position.y;
                cascade.inverseViewMatrix[3][2] = position.z;
            }
        }

        /** update (:258-287); camera far is the camera given (see Impl::shadowCamera) */
        void update(const glm::dmat4& projInv, const glm::dmat4& matrixWorld, F64 nearv, F64 cameraFar, const glm::dvec3& sunDirection, F64 distance,
                    const glm::dvec3& up)
        {
            far = maxFar >= 0.0 ? std::min(maxFar, cameraFar * farScale) : cameraFar * farScale;
            updateIntervals(projInv, nearv);
            updateMatrices(matrixWorld, nearv, sunDirection, distance, up);
            for (Cascade& c : cascades)
            {
                c.inverseProjectionMatrix = glm::inverse(c.projectionMatrix);
                c.viewMatrix = glm::inverse(c.inverseViewMatrix);
                c.matrix = c.projectionMatrix * c.viewMatrix;
                c.inverseMatrix = c.inverseViewMatrix * c.inverseProjectionMatrix;
            }
        }
    };

    // ── GL objects ───────────────────────────────────────────────────────────────────────────────
    struct Tex
    {
        GLuint id = 0;
        GLenum target = GL_TEXTURE_2D;
        S32 w = 0, h = 0, d = 0;
        GLenum fmt = GL_RGBA16F;
        void release() { if (id) { glDeleteTextures(1, &id); id = 0; } w = h = d = 0; }
    };

    GLenum baseFormat(GLenum fmt) { return (fmt == GL_R16F || fmt == GL_R8) ? GL_RED : GL_RGBA; }

    /** A texture like a three render target's: Linear filtering, ClampToEdge (three's default wrapping),
     *  zero-filled as WebGL fills new textures (WebGL 1.0 specification section 4.1 "Resource Restrictions"),
     *  so the first frame's history reads 0 there as well. */
    void alloc(Tex& t, GLenum target, GLenum fmt, S32 w, S32 h, S32 d = 1, GLenum wrap = GL_CLAMP_TO_EDGE, GLenum min_filter = GL_LINEAR)
    {
        if (t.id && t.target == target && t.fmt == fmt && t.w == w && t.h == h && t.d == d) return;
        t.release();
        t.target = target; t.fmt = fmt; t.w = w; t.h = h; t.d = d;
        glGenTextures(1, &t.id);
        // through LL's texture-unit cache (a raw glBindTexture would leave LLTexUnit believing its old texture is
        // still bound there, and a later bind of that texture would be skipped)
        LLTexUnit* unit = gGL.getTexUnit(0);
        unit->unbind(LLTexUnit::TT_TEXTURE);
        unit->activate();
        glBindTexture(target, t.id);
        const GLenum type = fmt == GL_R8 || fmt == GL_RGBA8 ? GL_UNSIGNED_BYTE : GL_HALF_FLOAT;
        if (target == GL_TEXTURE_2D) glTexImage2D(target, 0, fmt, w, h, 0, baseFormat(fmt), type, nullptr);
        else glTexImage3D(target, 0, fmt, w, h, d, 0, baseFormat(fmt), type, nullptr);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, min_filter);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap);
        if (target == GL_TEXTURE_3D) glTexParameteri(target, GL_TEXTURE_WRAP_R, wrap);
        glBindTexture(target, 0);   // unit 0 now holds nothing, as LLTexUnit was told above
        // zero fill (no scissor: glClearBufferfv honours it)
        LLGLDisable scissor(GL_SCISSOR_TEST);
        GLuint fb = 0;
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        const GLfloat zero[4] = { 0.f, 0.f, 0.f, 0.f };
        for (S32 layer = 0; layer < (target == GL_TEXTURE_2D ? 1 : d); ++layer)
        {
            if (target == GL_TEXTURE_2D) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.id, 0);
            else glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t.id, 0, layer);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glClearBufferfv(GL_COLOR, 0, zero);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fb);
    }

    /** One FBO with `atts` as colour attachments 0..n-1 (a 2D texture, or one layer of a 2D-array / 3D one).
     *  For the array targets: three-geospatial clouds/src/helpers/setArrayRenderTargetLayers.ts:4-29
     *  (every layer of the output array attached, COLOR_ATTACHMENT0 + layer, all drawn). */
    struct Att { const Tex* tex; S32 layer; };
    class Fbo
    {
    public:
        ~Fbo() { release(); }
        void release() { if (mId) { glDeleteFramebuffers(1, &mId); mId = 0; } }
        bool bind(const std::vector<Att>& atts)
        {
            if (!mId) glGenFramebuffers(1, &mId);
            glBindFramebuffer(GL_FRAMEBUFFER, mId);
            GLenum bufs[8];
            for (size_t i = 0; i < 8; ++i)
            {
                if (i < atts.size())
                {
                    const Att& a = atts[i];
                    if (a.tex->target == GL_TEXTURE_2D) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, GL_TEXTURE_2D, a.tex->id, 0);
                    else glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, a.tex->id, 0, a.layer);
                    bufs[i] = GL_COLOR_ATTACHMENT0 + (GLenum)i;
                }
                else
                {
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, GL_TEXTURE_2D, 0, 0);
                }
            }
            glDrawBuffers((GLsizei)atts.size(), bufs);
            return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
    private:
        GLuint mId = 0;
    };

    /** The full-screen draw (three's ShaderPass / Mesh(PlaneGeometry(2, 2)) -> LL's screen triangle). */
    void drawScreen(S32 w, S32 h)
    {
        glViewport(0, 0, w, h);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    // ── uniform helpers ──────────────────────────────────────────────────────────────────────────
    void u1f(LLGLSLShader& sh, const char* n, F32 v) { sh.uniform1f(LLStaticHashedString(n), v); }
    void u1i(LLGLSLShader& sh, const char* n, S32 v) { sh.uniform1i(LLStaticHashedString(n), v); }
    void u2f(LLGLSLShader& sh, const char* n, F32 x, F32 y) { sh.uniform2f(LLStaticHashedString(n), x, y); }
    void u3f(LLGLSLShader& sh, const char* n, F32 x, F32 y, F32 z) { sh.uniform3f(LLStaticHashedString(n), x, y, z); }
    void u4f(LLGLSLShader& sh, const char* n, F32 x, F32 y, F32 z, F32 w) { sh.uniform4f(LLStaticHashedString(n), x, y, z, w); }
    void umat4(LLGLSLShader& sh, const char* n, const glm::mat4& m) { sh.uniformMatrix4fv(LLStaticHashedString(n), 1, GL_FALSE, glm::value_ptr(m)); }
    void umat4v(LLGLSLShader& sh, const char* n, const F32* m, U32 count) { sh.uniformMatrix4fv(LLStaticHashedString(n), count, GL_FALSE, m); }
    void u2fv(LLGLSLShader& sh, const char* n, const F32* v, U32 count) { sh.uniform2fv(LLStaticHashedString(n), count, v); }

    // ── quality presets ──────────────────────────────────────────────────────────────────────────
    // Source: three-geospatial clouds/src/qualityPresets.ts:8-52 (values = the 'high' preset, also `defaults`),
    // :59-121 the four presets (each a copy of the defaults with its overrides).
    QualityPreset makeDefaults()
    {
        QualityPreset q;
        q.resolutionScale = 1.f; q.lightShafts = true; q.shapeDetail = true; q.turbulence = true; q.haze = true;
        q.multiScatteringOctaves = 8; q.accurateSunSkyLight = true; q.accuratePhaseFunction = false;
        q.maxIterationCount = 500; q.minStepSize = 50.f; q.maxStepSize = 1000.f; q.maxRayDistance = 2e5f; q.perspectiveStepScale = 1.01f;
        q.minDensity = 1e-5f; q.minExtinction = 1e-5f; q.minTransmittance = 1e-2f;
        q.maxIterationCountToGround = 3; q.maxIterationCountToSun = 2; q.minSecondaryStepSize = 100.f; q.secondaryStepScale = 2.f;
        q.maxShadowLengthIterationCount = 500; q.minShadowLengthStepSize = 50.f; q.maxShadowLengthRayDistance = 2e5f;
        q.cascadeCount = 3; q.mapSize = 512;
        q.shadowMaxIterationCount = 50; q.shadowMinStepSize = 100.f; q.shadowMaxStepSize = 1000.f;
        q.shadowMinDensity = 1e-5f; q.shadowMinExtinction = 1e-5f; q.shadowMinTransmittance = 1e-4f;
        return q;
    }
}

// static
const QualityPreset& QualityPreset::get(S32 quality)
{
    static QualityPreset presets[4];
    static bool init = false;
    if (!init)
    {
        init = true;
        const QualityPreset d = makeDefaults();
        // low (qualityPresets.ts:74-100)
        QualityPreset& lo = presets[0] = d;
        lo.lightShafts = false; lo.shapeDetail = false; lo.turbulence = false;
        lo.accurateSunSkyLight = false; lo.maxIterationCount = 200; lo.minStepSize = 100.f; lo.maxRayDistance = 1e5f;
        lo.minDensity = 1e-4f; lo.minExtinction = 1e-4f; lo.minTransmittance = 1e-1f; lo.maxIterationCountToGround = 0; lo.maxIterationCountToSun = 1;
        lo.shadowMaxIterationCount = 25; lo.shadowMinDensity = 1e-4f; lo.shadowMinExtinction = 1e-4f; lo.shadowMinTransmittance = 1e-2f;
        lo.cascadeCount = 2; lo.mapSize = 256;
        // medium (:101-119)
        QualityPreset& me = presets[1] = d;
        me.lightShafts = false; me.turbulence = false;
        me.minDensity = 1e-4f; me.minExtinction = 1e-4f; me.accurateSunSkyLight = false; me.maxIterationCountToSun = 2; me.maxIterationCountToGround = 1;
        me.shadowMinDensity = 1e-4f; me.shadowMinExtinction = 1e-4f; me.mapSize = 256;
        // high (:120) = defaults
        presets[2] = d;
        // ultra (:121-131)
        QualityPreset& ul = presets[3] = d;
        ul.minStepSize = 10.f; ul.mapSize = 1024;
        // [WolfViewer round 11] light shafts off at every tier - takram's own medium and low choice (qualityPresets.ts:65, :92
        // "lightShafts: false, // Expensive"): the shadow-length march (clouds.frag:622-648) alternates texel by texel between
        // a ray clamped at a far deck's front and one through its gaps (round 10, shaft_probe.py: 110 vs 192 per texel), and
        // the dome drops 0 or 80 km of in-scatter accordingly - the toothed horizon line. Back with a far-field filter (round 12).
        for (QualityPreset& q : presets) q.lightShafts = false;
    }
    return presets[llclamp(quality, 0, 3)];
}

// Source: three-geospatial clouds/src/CloudLayers.ts:31-68 CloudLayers.DEFAULT
// static
CloudLayers CloudLayers::defaults()
{
    CloudLayers l;
    CloudLayer& a = l.layer[0];
    a.channel = 'r'; a.altitude = 750.f; a.height = 650.f; a.densityScale = 0.2f; a.shapeAmount = 1.f; a.shapeDetailAmount = 1.f;
    a.weatherExponent = 1.f; a.shapeAlteringBias = 0.35f; a.coverageFilterWidth = 0.6f; a.shadow = true;
    CloudLayer& b = l.layer[1];
    b.channel = 'g'; b.altitude = 1000.f; b.height = 1200.f; b.densityScale = 0.2f; b.shapeAmount = 1.f; b.shapeDetailAmount = 1.f;
    b.weatherExponent = 1.f; b.shapeAlteringBias = 0.35f; b.coverageFilterWidth = 0.6f; b.shadow = true;
    CloudLayer& c = l.layer[2];
    c.channel = 'b'; c.altitude = 7500.f; c.height = 500.f; c.densityScale = 0.003f; c.shapeAmount = 0.4f; c.shapeDetailAmount = 0.f;
    c.weatherExponent = 1.f; c.shapeAlteringBias = 0.35f; c.coverageFilterWidth = 0.5f;
    l.layer[3].channel = 'a';   // { channel: 'a' }: the rest CloudLayer.DEFAULT (height 0: no layer)
    return l;
}

// Source: three-geospatial clouds/src/CloudLayers.ts:107-109 localWeatherChannels
std::string CloudLayers::localWeatherChannels() const
{
    return std::string(1, layer[0].channel) + layer[1].channel + layer[2].channel + layer[3].channel;
}

// Source: three-geospatial clouds/src/CloudLayers.ts:141-180 packIntervalHeights (the interval coding pattern:
// the gaps between the union of the layers' [altitude, altitude + height] ranges)
void CloudLayers::packIntervalHeights(F32 min_intervals[3], F32 max_intervals[3]) const
{
    struct Entry { F32 value; S32 flag; };
    Entry entries[8];
    for (S32 i = 0; i < 4; ++i)
    {
        entries[i] = { layer[i].altitude, 0 };
        entries[i + 4] = { layer[i].altitude + layer[i].height, 1 };
    }
    // compareEntries (:26-28): by value, then flag
    std::sort(entries, entries + 8, [](const Entry& a, const Entry& b) { return a.value != b.value ? a.value < b.value : a.flag < b.flag; });
    F32 imin[3] = { 0.f, 0.f, 0.f }, imax[3] = { 0.f, 0.f, 0.f };
    S32 intervalIndex = 0, balance = 0;
    for (S32 entryIndex = 0; entryIndex < 8; ++entryIndex)
    {
        const Entry& e = entries[entryIndex];
        if (balance == 0 && entryIndex > 0)
        {
            // CloudLayers.ts:158-162; three intervals at most between four layers
            if (intervalIndex < 3)
            {
                imin[intervalIndex] = entries[entryIndex - 1].value;
                imax[intervalIndex] = e.value;
            }
            ++intervalIndex;
        }
        balance += e.flag == 0 ? 1 : -1;
    }
    for (S32 i = 0; i < 3; ++i) { min_intervals[i] = imin[i]; max_intervals[i] = imax[i]; }
}

// ── the implementation ───────────────────────────────────────────────────────────────────────────
struct Clouds::Impl
{
    S32 shaderLevel = 1;
    bool loaded = false;
    // programs per quality (0..3): the clouds pass, its resolve (temporal upscale), the shadow pass, its resolve
    std::unique_ptr<LLGLSLShader> clouds[4], cloudsResolve[4], shadow[4], shadowResolve[4];
    bool tried[4] = { false, false, false, false };
    std::unique_ptr<LLGLSLShader> procShape, procShapeDetail, procLocalWeather, procTurbulence;

    // procedural textures (CloudShape, CloudShapeDetail, LocalWeather, Turbulence)
    Tex shapeTex, shapeDetailTex, localWeatherTex, turbulenceTex;
    bool proceduralDone = false;
    bool proceduralFailed = false;

    // shadow pass targets (ShadowPass.ts:115-162)
    Tex shadowCurrentTex, shadowResolveTex, shadowHistoryTex;   // swapped by index
    S32 shadowCascadeCount = 0, shadowMapSizeV = 0;
    // clouds pass targets (CloudsPass.ts:134-157)
    Tex cColor, cDepthVelocity, cShadowLength;           // current (low resolution)
    Tex rColor[2], rShadowLength[2];                     // resolve / history, swapped by `resolveIndex`
    S32 resolveIndex = 0;                                 // rColor[resolveIndex] = resolve target, the other = history
    S32 cWidth = 0, cHeight = 0;
    bool cLightShafts = false;
    // probe targets
    Tex pColor, pDepthVelocity, pShadowLength;
    Fbo fbo;

    CascadedShadowMaps shadowMaps;
    // CloudsMaterial previous matrices (CloudsMaterial.ts:140-141, 393-398): absent until the first render
    bool havePrev = false;
    glm::mat4 prevProj, prevView;
    // ShadowMaterial reprojectionMatrices (ShadowMaterial.ts:90-92, identity until copyReprojection)
    F32 shadowReprojection[4 * 16];
    // the AP composite's shadow data (CloudsEffect.ts:421-437)
    F32 intervals[4 * 2] = {};
    F32 matrices[4 * 16] = {};
    F32 inverseMatrices[4 * 16] = {};
    F32 topHeight = 0.f, bottomHeight = 0.f;
    // CloudsEffect state
    U32 frame = 0;
    // Source: three-geospatial clouds/src/CloudsEffect.ts:180-186 mutable parameter instances
    glm::vec2 localWeatherRepeat = glm::vec2(100.f);
    glm::vec2 localWeatherOffset = glm::vec2(0.f);
    glm::vec2 localWeatherOffsetTurned = glm::vec2(0.f);   // [WolfViewer 2026-10-14] the unstretched lookup's (cloudsCommon.glsl)
    glm::vec3 shapeRepeat = glm::vec3(0.0003f);
    glm::vec3 shapeOffset = glm::vec3(0.f);
    glm::vec3 shapeDetailRepeat = glm::vec3(0.006f);
    glm::vec3 shapeDetailOffset = glm::vec3(0.f);
    glm::vec2 turbulenceRepeat = glm::vec2(20.f);
    // CloudsEffect.ts:199-201 velocities (shape / detail: 0; the weather's from the frame)
    glm::vec3 shapeVelocity = glm::vec3(0.f), shapeDetailVelocity = glm::vec3(0.f);
    F64 cloudEvolution = 0.0;   // [WolfViewer round 17] metres, the shapes' drift ([2026-10-14] F64: it is no longer wrapped)
    S32 lastQuality = -1;
    // the main view's camera (the probe faces select shadow cascades with it)
    glm::mat4 mainView = glm::mat4(1.f);

    Impl()
    {
        for (S32 i = 0; i < 4; ++i)
        {
            const glm::mat4 id(1.f);
            memcpy(shadowReprojection + i * 16, glm::value_ptr(id), sizeof(F32) * 16);
        }
    }

    LLGLSLShader* make(const std::string& name, const char* vs, const char* fs, const std::map<std::string, std::string>& defines)
    {
        LLGLSLShader* sh = new LLGLSLShader();
        sh->mName = name;
        sh->mShaderFiles.clear();
        sh->clearPermutations();
        for (const auto& d : defines) sh->addPermutation(d.first, d.second);
        sh->mShaderFiles.push_back(std::make_pair(vs, GL_VERTEX_SHADER));
        sh->mShaderFiles.push_back(std::make_pair(fs, GL_FRAGMENT_SHADER));
        sh->mShaderLevel = shaderLevel;
        if (!sh->createShader())
        {
            LL_WARNS("WolfEnv") << name << " failed to build: the takram clouds are off at this quality" << LL_ENDL;
            delete sh;
            return nullptr;
        }
        return sh;
    }

    /** The defines of AtmosphereMaterialBase (AtmosphereMaterialBase.ts:147-158, 240-263): the precomputed
     *  table sizes (atmosphere/src/constants.ts:8-21), combined scattering (no single-Mie texture), the
     *  higher-order scattering texture present. */
    static void atmosphereDefines(std::map<std::string, std::string>& d)
    {
        d["PI"] = "3.141592653589793";   // `${Math.PI}`
        d["TRANSMITTANCE_TEXTURE_WIDTH"] = "256"; d["TRANSMITTANCE_TEXTURE_HEIGHT"] = "64";
        d["SCATTERING_TEXTURE_R_SIZE"] = "32"; d["SCATTERING_TEXTURE_MU_SIZE"] = "128";
        d["SCATTERING_TEXTURE_MU_S_SIZE"] = "32"; d["SCATTERING_TEXTURE_NU_SIZE"] = "8";
        d["IRRADIANCE_TEXTURE_WIDTH"] = "64"; d["IRRADIANCE_TEXTURE_HEIGHT"] = "16";
        d["METER_TO_LENGTH_UNIT"] = "0.0010000";   // (1 / 1000).toFixed(7)
        d["COMBINED_SCATTERING_TEXTURES"] = "1";
        d["HAS_HIGHER_ORDER_SCATTERING_TEXTURE"] = "1";
    }

    /** Build the four programs of a quality preset (once). The defines are those the takram materials keep:
     *  CloudsMaterial.ts:436-481 (+ onBeforeRender :263-309), ShadowMaterial.ts:106-110,120-139,
     *  CloudsResolveMaterial.ts:103-107, ShadowResolveMaterial.ts:71-72. */
    bool ensureQuality(S32 q)
    {
        q = llclamp(q, 0, 3);
        if (tried[q]) return clouds[q] && cloudsResolve[q] && shadow[q] && shadowResolve[q];
        tried[q] = true;
        const QualityPreset& p = QualityPreset::get(q);
        static const char* NAMES[4] = { "low", "medium", "high", "ultra" };
        const std::string channels = CloudLayers::defaults().localWeatherChannels();   // "rgba"

        std::map<std::string, std::string> c;
        atmosphereDefines(c);
        // DEPTH_PACKING: postprocessing hands the effect its depth texture with BasicDepthPacking (CloudsPass.ts
        // setDepthTexture :231-237); three constants.js:155 BasicDepthPacking = 3200 -> readDepthValue = .r
        c["DEPTH_PACKING"] = "3200";
        c["LOCAL_WEATHER_CHANNELS"] = channels;
        if (p.shapeDetail) c["SHAPE_DETAIL"] = "1";
        if (p.turbulence) c["TURBULENCE"] = "1";
        if (p.lightShafts) c["SHADOW_LENGTH"] = "1";
        if (p.haze) c["HAZE"] = "1";
        c["MULTI_SCATTERING_OCTAVES"] = llformat("%d", p.multiScatteringOctaves);
        if (p.accurateSunSkyLight) c["ACCURATE_SUN_SKY_LIGHT"] = "1";
        if (p.accuratePhaseFunction) c["ACCURATE_PHASE_FUNCTION"] = "1";
        c["SHADOW_CASCADE_COUNT"] = llformat("%d", p.cascadeCount);
        c["SHADOW_SAMPLE_COUNT"] = "8";                // CloudsMaterial.ts:468-469 shadowSampleCount = 8
        // CloudsMaterial.ts:474-481 defineFloat (toFixed(7), decorators.ts:135-160)
        c["SCATTER_ANISOTROPY_1"] = "0.7000000";
        c["SCATTER_ANISOTROPY_2"] = "-0.2000000";
        c["SCATTER_ANISOTROPY_MIX"] = "0.5000000";
        // [WolfViewer step 3] the ice phase: the forward lobe scaled by roughened ice's asymmetry over water's (wolfcloudgenus.h)
        c["WOLF_ICE_G_SCALE"] = llformat("%.7f", WolfCloudGenus::ICE_G_SCALE);
        c["WOLF_MS_ATTENUATION"] = llformat("%.7f", WolfCloudGenus::MS_ATTENUATION);   // [WolfViewer round 8] the octaves from the cloud albedo
        c["PERSPECTIVE_CAMERA"] = "1";                 // copyCameraSettings :313-317 (a perspective camera)
        // onBeforeRender (CloudsMaterial.ts:286-308): POWDER when powderScale (0.8, :214) > 0
        c["POWDER"] = "1";
        // GROUND_BOUNCE when groundBounceScale (1, :213) > 0 and maxIterationCountToGround > 0.
        // [WOLF] Source fix, Paul's instruction: CloudsMaterial.ts:586 (per-file line 302) reads
        //   `if (nextPowder) { this.defines.GROUND_BOUNCE = '1' } ...` inside the GROUND_BOUNCE change test,
        // i.e. it switches the ground bounce by the POWDER condition (a copy of the block above it); the test that
        // decides the change is `nextGroundIrradiance` (:581-584), which is what is used here.
        const F32 groundBounceScale = 1.f;   // CloudsMaterial.ts:213
        const bool nextGroundIrradiance = groundBounceScale > 0.f && p.maxIterationCountToGround > 0;
        if (nextGroundIrradiance) c["GROUND_BOUNCE"] = "1";

        std::map<std::string, std::string> s;
        s["SHADOW"] = "1"; s["TEMPORAL_PASS"] = "1"; s["TEMPORAL_JITTER"] = "1";   // ShadowMaterial.ts:106-110 (+ :128-132)
        s["LOCAL_WEATHER_CHANNELS"] = channels;
        s["CASCADE_COUNT"] = llformat("%d", p.cascadeCount);
        if (p.shapeDetail) s["SHAPE_DETAIL"] = "1";
        if (p.turbulence) s["TURBULENCE"] = "1";

        std::map<std::string, std::string> r;
        r["TEMPORAL_UPSCALE"] = "1";                   // CloudsResolveMaterial.ts:103-104 (temporalUpscale true)
        if (p.lightShafts) r["SHADOW_LENGTH"] = "1";   // :106-107 follows lightShafts (CloudsPass.ts:273-283)

        std::map<std::string, std::string> sr;
        sr["CASCADE_COUNT"] = llformat("%d", p.cascadeCount);

        const std::string tag = std::string(" (") + NAMES[q] + ")";
        clouds[q].reset(make("WolfTk Clouds" + tag, "wolftk/cloudsV.glsl", "wolftk/cloudsF.glsl", c));
        cloudsResolve[q].reset(make("WolfTk Clouds Resolve" + tag, "wolftk/screenV.glsl", "wolftk/cloudsResolveF.glsl", r));
        shadow[q].reset(make("WolfTk Clouds Shadow" + tag, "wolftk/screenV.glsl", "wolftk/shadowF.glsl", s));
        shadowResolve[q].reset(make("WolfTk Clouds Shadow Resolve" + tag, "wolftk/screenV.glsl", "wolftk/shadowResolveF.glsl", sr));
        return clouds[q] && cloudsResolve[q] && shadow[q] && shadowResolve[q];
    }

    /** Source: three-geospatial clouds/src/Procedural3DTexture.ts:73-87 render / ProceduralTexture.ts:78-87 render:
     *  once (needsRender), every layer of a 3D one its own draw with `layer` = layer / size. */
    bool renderProcedural()
    {
        if (proceduralDone) return true;
        if (proceduralFailed) return false;   // tried once this GL session (release() clears it)
        if (!procShape || !procShapeDetail || !procLocalWeather || !procTurbulence) return false;
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        // 3D: RedFormat (R8), Linear, Repeat (Procedural3DTexture.ts:140-151)
        auto render3D = [&](LLGLSLShader& sh, Tex& t, S32 size)
        {
            alloc(t, GL_TEXTURE_3D, GL_R8, size, size, size, GL_REPEAT);
            sh.bind();
            bool ok = true;
            for (S32 layer = 0; layer < size; ++layer)
            {
                u1f(sh, "layer", (F32)layer / (F32)size);
                if (!fbo.bind({ { &t, layer } })) { ok = false; break; }
                drawScreen(size, size);
            }
            sh.unbind();
            return ok;
        };
        // 2D: RGBA (RGBA8), mipmapped LinearMipMapLinear / Linear, Repeat (ProceduralTexture.ts:59-70)
        auto render2D = [&](LLGLSLShader& sh, Tex& t, S32 size)
        {
            alloc(t, GL_TEXTURE_2D, GL_RGBA8, size, size, 1, GL_REPEAT, GL_LINEAR_MIPMAP_LINEAR);
            sh.bind();
            u1f(sh, "layer", 0.f);
            const bool ok = fbo.bind({ { &t, 0 } });
            if (ok) drawScreen(size, size);
            sh.unbind();
            LLTexUnit* unit = gGL.getTexUnit(0);
            unit->unbind(LLTexUnit::TT_TEXTURE);
            unit->activate();
            glBindTexture(GL_TEXTURE_2D, t.id);
            glGenerateMipmap(GL_TEXTURE_2D);   // texture.generateMipmaps = true (:64)
            glBindTexture(GL_TEXTURE_2D, 0);
            return ok;
        };
        const bool ok = render3D(*procShape, shapeTex, CLOUD_SHAPE_TEXTURE_SIZE)
                     && render3D(*procShapeDetail, shapeDetailTex, CLOUD_SHAPE_DETAIL_TEXTURE_SIZE)
                     && render2D(*procLocalWeather, localWeatherTex, LOCAL_WEATHER_SIZE)
                     && render2D(*procTurbulence, turbulenceTex, TURBULENCE_SIZE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!ok)
        {
            LL_WARNS("WolfEnv") << "the clouds' noise textures could not be rendered (incomplete framebuffer); no clouds" << LL_ENDL;
            proceduralFailed = true;
            return false;
        }
        proceduralDone = true;
        return true;
    }

    /** The parameter, layer and atmosphere uniforms both materials share
     *  (uniforms.ts:45-68 createCloudParameterUniforms, :96-186 createCloudLayerUniforms / updateCloudLayerUniforms,
     *  :204-216 createAtmosphereUniforms). */
    void sharedUniforms(LLGLSLShader& sh, const CloudLayers& layers, const CloudsFrame& f, const glm::mat4& w2e, const glm::mat4& e2w,
                        F32 bottomRadius, F32 topRadius, bool shadowMaterial)
    {
        // participating medium (uniforms.ts:50-51)
        u1f(sh, "scatteringCoefficient", 1.f);
        u1f(sh, "absorptionCoefficient", 0.f);
        // weather and shape (:54-66)
        u1f(sh, "coverage", f.coverage);
        // [WolfViewer round 9] the repeat scaled by the EEP's cloud_scale (CloudsFrame weather_repeat_scale): bigger clouds = fewer repeats
        u2f(sh, "localWeatherRepeat", localWeatherRepeat.x * f.weather_repeat_scale, localWeatherRepeat.y * f.weather_repeat_scale);
        u2f(sh, "localWeatherOffset", localWeatherOffset.x, localWeatherOffset.y);
        u1f(sh, "wolfCloudEvolution", (F32)cloudEvolution);   // [WolfViewer round 17]
        u3f(sh, "shapeRepeat", shapeRepeat.x, shapeRepeat.y, shapeRepeat.z);
        u3f(sh, "shapeOffset", shapeOffset.x, shapeOffset.y, shapeOffset.z);
        u3f(sh, "shapeDetailRepeat", shapeDetailRepeat.x, shapeDetailRepeat.y, shapeDetailRepeat.z);
        u3f(sh, "shapeDetailOffset", shapeDetailOffset.x, shapeDetailOffset.y, shapeDetailOffset.z);
        u2f(sh, "turbulenceRepeat", turbulenceRepeat.x, turbulenceRepeat.y);
        u1f(sh, "turbulenceDisplacement", f.turbulence_displacement);   // [WolfViewer 2026-10-13] CloudsFrame: 350 + the EEP variance
        WolfTk::bindNamed(sh, "localWeatherTexture", 8, GL_TEXTURE_2D, localWeatherTex.id);
        WolfTk::bindNamed(sh, "shapeTexture", 9, GL_TEXTURE_3D, shapeTex.id);
        WolfTk::bindNamed(sh, "shapeDetailTexture", 10, GL_TEXTURE_3D, shapeDetailTex.id);
        WolfTk::bindNamed(sh, "turbulenceTexture", 11, GL_TEXTURE_2D, turbulenceTex.id);

        // cloud layers: updateCloudLayerUniforms (uniforms.ts:124-186)
        const CloudLayer* L = layers.layer;
        u4f(sh, "minLayerHeights", L[0].altitude, L[1].altitude, L[2].altitude, L[3].altitude);
        u4f(sh, "maxLayerHeights", L[0].altitude + L[0].height, L[1].altitude + L[1].height, L[2].altitude + L[2].height, L[3].altitude + L[3].height);
        F32 imin[3], imax[3];
        layers.packIntervalHeights(imin, imax);
        u3f(sh, "minIntervalHeights", imin[0], imin[1], imin[2]);
        u3f(sh, "maxIntervalHeights", imax[0], imax[1], imax[2]);
        u4f(sh, "densityScales", L[0].densityScale, L[1].densityScale, L[2].densityScale, L[3].densityScale);
        u4f(sh, "shapeAmounts", L[0].shapeAmount, L[1].shapeAmount, L[2].shapeAmount, L[3].shapeAmount);
        u4f(sh, "shapeDetailAmounts", L[0].shapeDetailAmount, L[1].shapeDetailAmount, L[2].shapeDetailAmount, L[3].shapeDetailAmount);
        u4f(sh, "weatherExponents", L[0].weatherExponent, L[1].weatherExponent, L[2].weatherExponent, L[3].weatherExponent);
        u4f(sh, "shapeAlteringBiases", L[0].shapeAlteringBias, L[1].shapeAlteringBias, L[2].shapeAlteringBias, L[3].shapeAlteringBias);
        u4f(sh, "coverageFilterWidths", L[0].coverageFilterWidth, L[1].coverageFilterWidth, L[2].coverageFilterWidth, L[3].coverageFilterWidth);
        // [WolfViewer step 3] the genus fields (cloudsParameters.glsl wolfLayer*): per-layer coverage, absorption, ice, and the
        // weather texture turned to the wind and stretched along it (column-major: S R, uv = S R (uv - 0.5) + 0.5)
        u4f(sh, "wolfLayerCoverage", L[0].coverage, L[1].coverage, L[2].coverage, L[3].coverage);
        u4f(sh, "wolfLayerAbsorption", L[0].absorption, L[1].absorption, L[2].absorption, L[3].absorption);
        u4f(sh, "wolfLayerIce", L[0].ice, L[1].ice, L[2].ice, L[3].ice);
        u4f(sh, "wolfLayerPowder", L[0].powder, L[1].powder, L[2].powder, L[3].powder);   // [WolfViewer round 16c]
        {
            const F32 a = f.weather_rotation_deg * DEG_TO_RAD, ca = cosf(a), sa = sinf(a), inv = 1.f / llmax(f.weather_stretch, 1e-3f);
            const F32 m[4] = { inv * ca, sa, -inv * sa, ca };   // columns: (S R)[.,0], (S R)[.,1] with S = diag(1/stretch, 1)
            static LLStaticHashedString s_rot("wolfWeatherRotation");
            const S32 loc = sh.getUniformLocation(s_rot);
            if (loc >= 0) glUniformMatrix2fv(loc, 1, GL_FALSE, m);
            u2f(sh, "wolfWeatherPivot", f.weather_pivot.mV[0], f.weather_pivot.mV[1]);   // [WolfViewer round 13]
            // [WolfViewer 2026-10-14] the stretch only for the layers that have that shape (cirrus, streets); the others are
            // read through the turn R alone (cloudsCommon.glsl sampleWeather), so the cumulus under a cirrus veil keeps its shape
            const F32 turn[4] = { ca, sa, -sa, ca };            // columns of R
            static LLStaticHashedString s_turn("wolfWeatherTurn");
            const S32 tloc = sh.getUniformLocation(s_turn);
            if (tloc >= 0) glUniformMatrix2fv(tloc, 1, GL_FALSE, turn);
            u4f(sh, "wolfLayerStretched", f.weather_layer_stretched[0], f.weather_layer_stretched[1],
                f.weather_layer_stretched[2], f.weather_layer_stretched[3]);
            u2f(sh, "wolfWeatherOffsetTurned", localWeatherOffsetTurned.x, localWeatherOffsetTurned.y);
            u4f(sh, "wolfLayerTower", f.weather_layer_tower[0], f.weather_layer_tower[1], f.weather_layer_tower[2], f.weather_layer_tower[3]);
        }
        u4f(sh, "densityProfile.expTerms", L[0].densityProfile.expTerm, L[1].densityProfile.expTerm, L[2].densityProfile.expTerm, L[3].densityProfile.expTerm);
        u4f(sh, "densityProfile.exponents", L[0].densityProfile.exponent, L[1].densityProfile.exponent, L[2].densityProfile.exponent, L[3].densityProfile.exponent);
        u4f(sh, "densityProfile.linearTerms", L[0].densityProfile.linearTerm, L[1].densityProfile.linearTerm, L[2].densityProfile.linearTerm, L[3].densityProfile.linearTerm);
        u4f(sh, "densityProfile.constantTerms", L[0].densityProfile.constantTerm, L[1].densityProfile.constantTerm, L[2].densityProfile.constantTerm, L[3].densityProfile.constantTerm);
        F32 totalMin = std::numeric_limits<F32>::infinity(), totalMax = 0.f, sBottom = std::numeric_limits<F32>::infinity(), sTop = 0.f;
        F32 mask[4] = { 0.f, 0.f, 0.f, 0.f };
        for (S32 i = 0; i < 4; ++i)
        {
            const F32 maxH = L[i].altitude + L[i].height;
            if (L[i].height > 0.f)
            {
                if (L[i].altitude < totalMin) totalMin = L[i].altitude;
                if (L[i].shadow && L[i].altitude < sBottom) sBottom = L[i].altitude;
                if (maxH > totalMax) totalMax = maxH;
                if (L[i].shadow && maxH > sTop) sTop = maxH;
            }
            mask[i] = L[i].shadow ? 1.f : 0.f;
        }
        // :171-184 (minHeight/maxHeight, shadowBottomHeight/shadowTopHeight; the "else" keeps the max at 0)
        const F32 minHeight = std::isfinite(totalMin) ? totalMin : 0.f;
        const F32 maxHeight = std::isfinite(totalMin) ? totalMax : 0.f;
        bottomHeight = std::isfinite(sBottom) ? sBottom : 0.f;
        topHeight = std::isfinite(sBottom) ? sTop : 0.f;
        u1f(sh, "minHeight", minHeight);
        u1f(sh, "maxHeight", maxHeight);
        u1f(sh, "shadowTopHeight", topHeight);
        u1f(sh, "shadowBottomHeight", bottomHeight);
        // CloudsMaterial overrides shadowLayerMask with (1, 1, 1, 1) "Disable mask" (CloudsMaterial.ts:244); the
        // shadow material keeps the layers' mask (it alone defines SHADOW, clouds.glsl:87-89)
        if (shadowMaterial) u4f(sh, "shadowLayerMask", mask[0], mask[1], mask[2], mask[3]);
        else u4f(sh, "shadowLayerMask", 1.f, 1.f, 1.f, 1.f);

        // atmosphere uniforms (uniforms.ts:204-216): radii in metres
        u1f(sh, "bottomRadius", bottomRadius);
        u1f(sh, "topRadius", topRadius);
        umat4(sh, "worldToECEFMatrix", w2e);
        umat4(sh, "ecefToWorldMatrix", e2w);
        u3f(sh, "altitudeCorrection", 0.f, 0.f, 0.f);   // a sphere: no correction (getAltitudeCorrectionOffset.ts on a sphere = 0)
        u3f(sh, "sunDirection", f.sun_dir_ecef.mV[0], f.sun_dir_ecef.mV[1], f.sun_dir_ecef.mV[2]);
        // [WolfViewer round 8] the clouds lit by the moon as by the sun (cloudsParameters.glsl wolfLight* / wolfOther*)
        u1f(sh, "wolfLightScale", f.light_scale);
        u3f(sh, "wolfOtherDirection", f.other_dir_ecef.mV[0], f.other_dir_ecef.mV[1], f.other_dir_ecef.mV[2]);
        u1f(sh, "wolfOtherScale", f.other_scale);
        // the noise seed: the takram `time` of generators.ts:22 (seconds); taken modulo 1024 s for float
        // precision - fract(time) (all the shader uses) is unchanged by it
        u1f(sh, "time", (F32)std::fmod(f.time, 1024.0));
        u1i(sh, "frame", (S32)frame);
    }

    /** CloudsMaterial.copyCameraSettings (CloudsMaterial.ts:311-389) for camera (view, proj), `temporalUpscale`;
     *  `resolution` is the material's (setSize). Leaves the derived matrices in the uniforms. */
    void cameraUniforms(LLGLSLShader& sh, const CloudsFrame& f, bool temporalUpscale, const glm::vec2& resolution, F32 bottomRadius,
                        const glm::mat4& prevP, const glm::mat4& prevV, const glm::mat4& cascadeView)
    {
        // the inverses in double, as three.js computes them (its Matrix4 is double; float only at the upload)
        const glm::dmat4 viewD = glm::dmat4(f.view);
        const glm::dmat4 inverseViewD = glm::inverse(viewD);
        const glm::mat4 inverseView = glm::mat4(inverseViewD);
        // viewMatrix: in clouds.frag it only selects the shadow cascade (getCascadeIndex / getFadedCascadeIndex);
        // the main view's camera for the main view and, for a probe face, also the main view's (whose frustum the
        // cascades were fitted to, CloudsPass.copyShadow :159-168)
        umat4(sh, "viewMatrix", cascadeView);
        umat4(sh, "inverseViewMatrix", inverseView);
        glm::dmat4 inverseProjection, reprojection;
        if (temporalUpscale)
        {
            const S32 fr = (S32)(frame % 16);
            const glm::vec2 offset = bayerOffset(fr);
            const F32 dx = ((offset.x - 0.5f) / resolution.x) * 4.f;
            const F32 dy = ((offset.y - 0.5f) / resolution.y) * 4.f;
            u2f(sh, "temporalJitter", dx, dy);
            u1f(sh, "mipLevelScale", 0.25f);   // "NOTE: Not exactly"
            glm::dmat4 p = glm::dmat4(f.proj);
            p[2][0] += dx * 2.0;   // elements[8]
            p[2][1] += dy * 2.0;   // elements[9]
            inverseProjection = glm::inverse(p);
            reprojection = glm::dmat4(prevP);
            reprojection[2][0] += dx * 2.0;
            reprojection[2][1] += dy * 2.0;
            reprojection = reprojection * glm::dmat4(prevV);
        }
        else
        {
            u2f(sh, "temporalJitter", 0.f, 0.f);
            u1f(sh, "mipLevelScale", 1.f);
            inverseProjection = glm::inverse(glm::dmat4(f.proj));
            reprojection = glm::dmat4(prevP) * glm::dmat4(prevV);
        }
        umat4(sh, "inverseProjectionMatrix", glm::mat4(inverseProjection));
        umat4(sh, "reprojectionMatrix", glm::mat4(reprojection));
        umat4(sh, "viewReprojectionMatrix", glm::mat4(reprojection * inverseViewD));
        u1f(sh, "cameraNear", f.near_clip);
        u1f(sh, "cameraFar", f.far_clip);
        u3f(sh, "cameraPosition", f.camera_world.mV[0], f.camera_world.mV[1], f.camera_world.mV[2]);
        // cameraHeight = the geodetic height of the camera (Geodetic.setFromECEF). On the sphere of bottomRadius
        // the viewer's ECEF is built on, that is |p| - bottomRadius.
        const glm::dvec3 camEcef = glm::dvec3(f.world_to_ecef * glm::dvec4(f.camera_world.mV[0], f.camera_world.mV[1], f.camera_world.mV[2], 1.0));
        u1f(sh, "cameraHeight", (F32)(glm::length(camEcef) - (F64)bottomRadius));
    }

    /** The clouds material's own scalar uniforms (CloudsMaterial.ts:190-256) from the preset. */
    void cloudsMaterialUniforms(LLGLSLShader& sh, const QualityPreset& p, const CloudsFrame& f)
    {
        // scattering (:210-214)
        u1f(sh, "skyLightScale", 1.f);
        u1f(sh, "groundBounceScale", 1.f);
        u1f(sh, "powderScale", 0.8f);
        u1f(sh, "powderExponent", 150.f);
        // primary raymarch (:217-224)
        u1i(sh, "maxIterationCount", p.maxIterationCount);
        u1f(sh, "minStepSize", p.minStepSize);
        u1f(sh, "maxStepSize", p.maxStepSize);
        u1f(sh, "maxRayDistance", p.maxRayDistance);
        u1f(sh, "perspectiveStepScale", p.perspectiveStepScale);
        u1f(sh, "minDensity", p.minDensity);
        u1f(sh, "minExtinction", p.minExtinction);
        u1f(sh, "minTransmittance", p.minTransmittance);
        // secondary raymarch (:227-230)
        u1i(sh, "maxIterationCountToSun", p.maxIterationCountToSun);
        u1i(sh, "maxIterationCountToGround", p.maxIterationCountToGround);
        u1f(sh, "minSecondaryStepSize", p.minSecondaryStepSize);
        u1f(sh, "secondaryStepScale", p.secondaryStepScale);
        // beer shadow map (:233-243)
        u2f(sh, "shadowTexelSize", 1.f / shadowMaps.mapSize, 1.f / shadowMaps.mapSize);   // setShadowSize (:424-426)
        u1f(sh, "maxShadowFilterRadius", 6.f);
        // shadow length (:247-249)
        u1i(sh, "maxShadowLengthIterationCount", p.maxShadowLengthIterationCount);
        u1f(sh, "minShadowLengthStepSize", p.minShadowLengthStepSize);
        u1f(sh, "maxShadowLengthRayDistance", p.maxShadowLengthRayDistance);
        // haze (:252-255)
        u1f(sh, "hazeDensityScale", f.haze_density_scale);
        u1f(sh, "hazeExponent", f.haze_exponent);   // [WolfViewer 2026-10-13] CloudsFrame: 1e-3 (CloudsMaterial.ts), or the EEP's
        u1f(sh, "wolfHazeAlways", f.haze_always ? 1.f : 0.f);
        // [WolfViewer 2026-10-13] the stock EEP haze (CloudsFrame ll_*; cloudsF.glsl wolfLLHaze)
        u1f(sh, "wolfLLHazeOn", f.ll_haze ? 1.f : 0.f);
        u3f(sh, "wolfLLBlueW", f.ll_blue_w.mV[0], f.ll_blue_w.mV[1], f.ll_blue_w.mV[2]);
        u3f(sh, "wolfLLHazeW", f.ll_haze_w.mV[0], f.ll_haze_w.mV[1], f.ll_haze_w.mV[2]);
        u3f(sh, "wolfLLCs", f.ll_cs.mV[0], f.ll_cs.mV[1], f.ll_cs.mV[2]);
        u3f(sh, "wolfLLTmpAmb", f.ll_tmp_amb.mV[0], f.ll_tmp_amb.mV[1], f.ll_tmp_amb.mV[2]);
        u3f(sh, "wolfLLCombined", f.ll_combined.mV[0], f.ll_combined.mV[1], f.ll_combined.mV[2]);
        u3f(sh, "wolfLLGlow", f.ll_glow_x, f.ll_glow_z, f.ll_smgf);
        u3f(sh, "wolfLLHazeScale", f.ll_density_distance, f.ll_max_y, f.ll_hdr);
        u3f(sh, "wolfLLLightnorm", f.ll_lightnorm_ecef.mV[0], f.ll_lightnorm_ecef.mV[1], f.ll_lightnorm_ecef.mV[2]);
        u3f(sh, "wolfLLLightDir", f.ll_light_dir_ecef.mV[0], f.ll_light_dir_ecef.mV[1], f.ll_light_dir_ecef.mV[2]);
        u1f(sh, "hazeScatteringCoefficient", 0.9f);
        u1f(sh, "hazeAbsorptionCoefficient", 0.5f);
        // [WolfViewer] the haze under a deck is lit by the sky under the deck (cloudsF.glsl approximateHaze)
        u3f(sh, "wolfSkyUnderCloud", f.sky_under_cloud.mV[0], f.sky_under_cloud.mV[1], f.sky_under_cloud.mV[2]);
    }

    /** CloudsPass.copyShadow (CloudsPass.ts:159-168) + what CloudsEffect.updateAtmosphereComposition hands the
     *  aerial perspective (:421-437). */
    void copyShadow(LLGLSLShader& sh)
    {
        const S32 n = (S32)shadowMaps.cascades.size();
        // shadowIntervals / shadowMatrices are uniforms of 4 elements (CloudsMaterial.ts:236-241, "Populate the max
        // number of elements"); the program's SHADOW_CASCADE_COUNT decides how many it declares
        for (S32 i = 0; i < 4; ++i)
        {
            const bool has = i < n;
            const glm::mat4 m = has ? glm::mat4(shadowMaps.cascades[i].matrix) : glm::mat4(1.f);
            const glm::mat4 im = has ? glm::mat4(shadowMaps.cascades[i].inverseMatrix) : glm::mat4(1.f);
            intervals[i * 2 + 0] = has ? (F32)shadowMaps.cascades[i].interval.x : 0.f;
            intervals[i * 2 + 1] = has ? (F32)shadowMaps.cascades[i].interval.y : 0.f;
            memcpy(matrices + i * 16, glm::value_ptr(m), sizeof(F32) * 16);
            memcpy(inverseMatrices + i * 16, glm::value_ptr(im), sizeof(F32) * 16);
        }
        u2fv(sh, "shadowIntervals", intervals, n);
        umat4v(sh, "shadowMatrices", matrices, n);
        u1f(sh, "shadowFar", (F32)shadowMaps.far);
    }

    const Tex& shadowOutput() const { return shadowHistoryTex; }

    /** A pass's framebuffer was incomplete: logged once, the caller's framebuffer and viewport back, no clouds. */
    bool incomplete(const GLint vp[4])
    {
        static bool s_warned = false;
        if (!s_warned)
        {
            LL_WARNS("WolfEnv") << "a clouds pass framebuffer is incomplete on this driver; the clouds are skipped" << LL_ENDL;
            s_warned = true;
        }
        for (S32 u = 8; u <= 12; ++u) { WolfTk::unbindUnit(u, GL_TEXTURE_2D); WolfTk::unbindUnit(u, GL_TEXTURE_3D); WolfTk::unbindUnit(u, GL_TEXTURE_2D_ARRAY); }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        return false;
    }   // ShadowPass.ts:164-171 (temporalPass: history after the swap)
};

Clouds::Clouds() : mImpl(new Impl()), mLayers(CloudLayers::defaults()) {}

Clouds::~Clouds()
{
    release();   // the textures and framebuffer (GL objects): the owner's GL context is current at shutdown
    delete mImpl;
}

bool Clouds::loadShaders(S32 shader_level)
{
    Impl& m = *mImpl;
    unloadShaders();
    m.shaderLevel = shader_level;
    std::map<std::string, std::string> none;
    m.procShape.reset(m.make("WolfTk Cloud Shape", "wolftk/proceduralV.glsl", "wolftk/cloudShapeF.glsl", none));
    m.procShapeDetail.reset(m.make("WolfTk Cloud Shape Detail", "wolftk/proceduralV.glsl", "wolftk/cloudShapeDetailF.glsl", none));
    m.procLocalWeather.reset(m.make("WolfTk Local Weather", "wolftk/proceduralV.glsl", "wolftk/localWeatherF.glsl", none));
    m.procTurbulence.reset(m.make("WolfTk Turbulence", "wolftk/proceduralV.glsl", "wolftk/turbulenceF.glsl", none));
    // the presets the viewer's tiers use are built now (Wolf tier 2 = medium, tier 3 = high); low and ultra when asked
    const bool ok = m.ensureQuality(1) && m.ensureQuality(2);
    m.loaded = ok && m.procShape && m.procShapeDetail && m.procLocalWeather && m.procTurbulence;
    return m.loaded;
}

void Clouds::unloadShaders()
{
    Impl& m = *mImpl;
    for (S32 q = 0; q < 4; ++q)
    {
        for (std::unique_ptr<LLGLSLShader>* p : { &m.clouds[q], &m.cloudsResolve[q], &m.shadow[q], &m.shadowResolve[q] })
        {
            if (*p) { (*p)->unload(); p->reset(); }
        }
        m.tried[q] = false;
    }
    for (std::unique_ptr<LLGLSLShader>* p : { &m.procShape, &m.procShapeDetail, &m.procLocalWeather, &m.procTurbulence })
    {
        if (*p) { (*p)->unload(); p->reset(); }
    }
    m.loaded = false;
}

bool Clouds::shadersReady() const { return mImpl->loaded; }

// Source: three-geospatial clouds/src/CloudsEffect.ts:454-488 update (with updateSharedUniforms :345-397),
// ShadowPass.ts:340-356 update, CloudsPass.ts:185-201 update.
bool Clouds::render(const Atmosphere& atmosphere, const CloudsFrame& f)
{
    Impl& m = *mImpl;
    if (!m.loaded || !m.ensureQuality(f.quality)) return false;
    const S32 q = llclamp(f.quality, 0, 3);
    const QualityPreset& p = QualityPreset::get(q);
    LLGLSLShader& cloudsSh = *m.clouds[q];
    LLGLSLShader& cloudsResolveSh = *m.cloudsResolve[q];
    LLGLSLShader& shadowSh = *m.shadow[q];
    LLGLSLShader& shadowResolveSh = *m.shadowResolve[q];

    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);
    LLGLDisable cull(GL_CULL_FACE);
    LLGLDisable scissor(GL_SCISSOR_TEST);

    if (!m.renderProcedural())
    {
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        return false;
    }

    // CloudsEffect.update :459-471: the shadow map size and cascade count follow the preset
    if (p.cascadeCount != m.shadowCascadeCount || p.mapSize != m.shadowMapSizeV)
    {
        m.shadowCascadeCount = p.cascadeCount;
        m.shadowMapSizeV = p.mapSize;
        m.shadowMaps.setCascadeCount(p.cascadeCount);
        m.shadowMaps.mapSize = p.mapSize;
        // ShadowPass.setSize (:142-162): the current target holds depth * 2 layers with temporalPass (depth velocity)
        alloc(m.shadowCurrentTex, GL_TEXTURE_2D_ARRAY, GL_RGBA16F, p.mapSize, p.mapSize, p.cascadeCount * 2);
        alloc(m.shadowResolveTex, GL_TEXTURE_2D_ARRAY, GL_RGBA16F, p.mapSize, p.mapSize, p.cascadeCount);
        alloc(m.shadowHistoryTex, GL_TEXTURE_2D_ARRAY, GL_RGBA16F, p.mapSize, p.mapSize, p.cascadeCount);
    }
    // the clouds pass size (CloudsPass.setSize :203-224, temporalUpscale, resolutionScale 1 in every preset)
    if ((S32)f.width != m.cWidth || (S32)f.height != m.cHeight || p.lightShafts != m.cLightShafts || q != m.lastQuality)
    {
        m.cWidth = (S32)f.width; m.cHeight = (S32)f.height; m.cLightShafts = p.lightShafts;
        const S32 lw = (S32)std::ceil(f.width / 4.0), lh = (S32)std::ceil(f.height / 4.0);
        // createRenderTarget (CloudsPass.ts:37-72): HalfFloat RGBA colour, a clone for depth velocity, a RedFormat
        // clone for the shadow length; the resolve / history targets without depth velocity
        alloc(m.cColor, GL_TEXTURE_2D, GL_RGBA16F, lw, lh);
        alloc(m.cDepthVelocity, GL_TEXTURE_2D, GL_RGBA16F, lw, lh);
        if (p.lightShafts) alloc(m.cShadowLength, GL_TEXTURE_2D, GL_R16F, lw, lh); else m.cShadowLength.release();
        for (S32 i = 0; i < 2; ++i)
        {
            alloc(m.rColor[i], GL_TEXTURE_2D, GL_RGBA16F, f.width, f.height);
            if (p.lightShafts) alloc(m.rShadowLength[i], GL_TEXTURE_2D, GL_R16F, f.width, f.height); else m.rShadowLength[i].release();
        }
        // CloudsMaterial.setSize (:400-422): invalidates the reprojection
        m.havePrev = false;
    }
    m.lastQuality = q;
    if (f.reset_history) m.havePrev = false;   // as setSize's invalidation (CloudsMaterial.ts:418-421)

    ++m.frame;
    // updateSharedUniforms (:345-397): offsets += velocity x deltaTime
    m.localWeatherOffset += glm::vec2(f.local_weather_velocity.mV[0], f.local_weather_velocity.mV[1]) * f.dt;
    // [WolfViewer 2026-10-14] wrapped into 0..1: the weather texture repeats, so a whole step is no change, and the float keeps
    // its precision (it grew without end). The plain lookup's x times the stretch (cloudsCommon.glsl sampleWeather) stays
    // seamless because the stretch is a whole number (wolfcloudgenus.cpp: 1 or 4).
    m.localWeatherOffset -= glm::floor(m.localWeatherOffset);
    m.localWeatherOffsetTurned += glm::vec2(f.local_weather_velocity_turned.mV[0], f.local_weather_velocity_turned.mV[1]) * f.dt;
    m.localWeatherOffsetTurned -= glm::floor(m.localWeatherOffsetTurned);
    // [WolfViewer round 17] the shapes' vertical drift in metres (cloudsCommon.glsl wolfCloudEvolution): takram's ratio of drift to
    // cloud motion, 2e4 x the offset of its repeat-100 texture (clouds.glsl:120-121), 0.2.
    // [WolfViewer 2026-10-14] NOT wrapped any more: it moves the shape lookup along the surface normal (cloudsCommon.glsl
    // evolution = -surfaceNormal x this), which is not a noise-texture axis, so the wrap at 1 / shapeRepeat.z was no whole
    // period and every shape jumped at once each wrap (14-55 min apart). Held in F64.
    m.cloudEvolution += 0.2 * (F64)llmax(f.wind_speed, 0.f) * (F64)f.dt;
    m.shapeOffset += m.shapeVelocity * f.dt;
    m.shapeDetailOffset += m.shapeDetailVelocity * f.dt;
    const glm::mat4 w2e = glm::mat4(f.world_to_ecef);
    const glm::dmat4 e2wd = glm::inverse(f.world_to_ecef);
    const glm::mat4 e2w = glm::mat4(e2wd);
    const F32 bottomRadius = atmosphere.bottomRadiusKm() * 1000.f;
    const F32 topRadius = atmosphere.topRadiusKm() * 1000.f;
    // :380-396 the sun distance by its zenith angle, the shadow maps fitted to the camera with the sun in world space
    const glm::dvec3 camEcef = glm::dvec3(f.world_to_ecef * glm::dvec4(f.camera_world.mV[0], f.camera_world.mV[1], f.camera_world.mV[2], 1.0));
    const glm::dvec3 surfaceNormal = glm::normalize(camEcef);   // ellipsoid.getSurfaceNormal on the viewer's sphere
    const glm::dvec3 sunEcef(f.sun_dir_ecef.mV[0], f.sun_dir_ecef.mV[1], f.sun_dir_ecef.mV[2]);
    const F64 zenithAngle = glm::dot(sunEcef, surfaceNormal);
    const F64 distance = lerp(1e6, 1e3, zenithAngle);
    const glm::dmat3 ecefToWorldRotation = glm::transpose(glm::dmat3(f.world_to_ecef));
    const glm::dvec3 sunWorld = ecefToWorldRotation * sunEcef;
    // The shadow maps' camera. Source: storybook/src/clouds/Clouds-Basic.tsx:78 `shadow-maxFar={1e5}` with the
    // story camera's far 4e5 (:97): the cascades cover up to min(1e5, 4e5 x farScale 1) = 100 km. LL's own far
    // clip is the draw distance (a few hundred metres), so the cascades are fitted to the same camera (LL's view,
    // field of view, aspect and near) with the story's far 4e5 - its projection rebuilt with three
    // Matrix4.makePerspective's c, d (Matrix4.js:777-811, WebGL coordinate system).
    const F64 STORY_FAR = 4e5;
    m.shadowMaps.maxFar = 1e5;
    glm::dmat4 sp = glm::dmat4(f.proj);
    {
        const F64 n = f.near_clip, fa = STORY_FAR;
        sp[2][2] = -(fa + n) / (fa - n);
        sp[3][2] = (-2.0 * fa * n) / (fa - n);
    }
    const glm::dmat4 matrixWorld = glm::inverse(glm::dmat4(f.view));
    m.shadowMaps.update(glm::inverse(sp), matrixWorld, f.near_clip, STORY_FAR, sunWorld, distance, glm::normalize(ecefToWorldRotation * DEFAULT_UP_ECEF));
    m.mainView = f.view;

    // ── ShadowPass.update (ShadowPass.ts:340-356) ──
    {
        LLGLSLShader& sh = shadowSh;
        sh.bind();
        m.sharedUniforms(sh, mLayers, f, w2e, e2w, bottomRadius, topRadius, true);
        // ShadowMaterial uniforms (ShadowMaterial.ts:82-105): presets' shadow values, opticalDepthTailScale 2
        u2f(sh, "resolution", (F32)p.mapSize, (F32)p.mapSize);   // setSize (:116-118)
        u1i(sh, "maxIterationCount", p.shadowMaxIterationCount);
        u1f(sh, "minStepSize", p.shadowMinStepSize);
        u1f(sh, "maxStepSize", p.shadowMaxStepSize);
        u1f(sh, "minDensity", p.shadowMinDensity);
        u1f(sh, "minExtinction", p.shadowMinExtinction);
        u1f(sh, "minTransmittance", p.shadowMinTransmittance);
        u1f(sh, "opticalDepthTailScale", 2.f);
        // copyShadow (:96-103): inverse shadow matrices; reprojectionMatrices from the last copyReprojection
        F32 inv[4 * 16];
        for (S32 i = 0; i < p.cascadeCount; ++i)
        {
            const glm::mat4 im = glm::mat4(m.shadowMaps.cascades[i].inverseMatrix);
            memcpy(inv + i * 16, glm::value_ptr(im), sizeof(F32) * 16);
        }
        umat4v(sh, "inverseShadowMatrices", inv, p.cascadeCount);
        umat4v(sh, "reprojectionMatrices", m.shadowReprojection, p.cascadeCount);
        std::vector<Att> atts;
        for (S32 l = 0; l < p.cascadeCount * 2; ++l) atts.push_back({ &m.shadowCurrentTex, l });
        if (!m.fbo.bind(atts)) { sh.unbind(); return m.incomplete(vp); }
        drawScreen(p.mapSize, p.mapSize);
        sh.unbind();
        for (S32 u = 8; u <= 11; ++u) WolfTk::unbindUnit(u, u == 9 || u == 10 ? GL_TEXTURE_3D : GL_TEXTURE_2D);
    }
    {
        // the resolve (ShadowResolveMaterial: varianceGamma 1, temporalAlpha 0.01, ShadowResolveMaterial.ts:51-62)
        LLGLSLShader& sh = shadowResolveSh;
        sh.bind();
        u2f(sh, "texelSize", 1.f / p.mapSize, 1.f / p.mapSize);
        u1f(sh, "varianceGamma", 1.f);
        u1f(sh, "temporalAlpha", 0.01f);
        WolfTk::bindNamed(sh, "inputBuffer", 8, GL_TEXTURE_2D_ARRAY, m.shadowCurrentTex.id);
        WolfTk::bindNamed(sh, "historyBuffer", 9, GL_TEXTURE_2D_ARRAY, m.shadowHistoryTex.id);
        std::vector<Att> atts;
        for (S32 l = 0; l < p.cascadeCount; ++l) atts.push_back({ &m.shadowResolveTex, l });
        if (!m.fbo.bind(atts)) { sh.unbind(); return m.incomplete(vp); }
        drawScreen(p.mapSize, p.mapSize);
        sh.unbind();
        WolfTk::unbindUnit(8, GL_TEXTURE_2D_ARRAY);
        WolfTk::unbindUnit(9, GL_TEXTURE_2D_ARRAY);
        // copyReprojection (:105-111): this frame's cascade matrices for the next reprojection
        for (S32 i = 0; i < p.cascadeCount; ++i)
        {
            const glm::mat4 mm = glm::mat4(m.shadowMaps.cascades[i].matrix);
            memcpy(m.shadowReprojection + i * 16, glm::value_ptr(mm), sizeof(F32) * 16);
        }
        // swapBuffers (:113-122)
        std::swap(m.shadowResolveTex, m.shadowHistoryTex);
    }

    // ── CloudsPass.update (CloudsPass.ts:185-201) ──
    const S32 lw = m.cColor.w, lh = m.cColor.h;
    const glm::vec2 resolution((F32)(lw * 4), (F32)(lh * 4));   // setSize(lowResWidth * 4, lowResHeight * 4, width, height)
    {
        LLGLSLShader& sh = cloudsSh;
        sh.bind();
        m.sharedUniforms(sh, mLayers, f, w2e, e2w, bottomRadius, topRadius, false);
        atmosphere.setRuntimeUniforms(sh, f.pre_exposure);
        m.cloudsMaterialUniforms(sh, p, f);
        u2f(sh, "resolution", resolution.x, resolution.y);
        u2f(sh, "targetUvScale", resolution.x / f.width, resolution.y / f.height);   // CloudsMaterial.setSize (:406-414)
        const glm::mat4 prevP = m.havePrev ? m.prevProj : f.proj;   // `previousProjectionMatrix ?? camera.projectionMatrix`
        const glm::mat4 prevV = m.havePrev ? m.prevView : f.view;
        m.cameraUniforms(sh, f, true, resolution, bottomRadius, prevP, prevV, f.view);
        m.copyShadow(sh);
        WolfTk::bindReserved(sh, LLShaderMgr::WOLF_TK_SHADOW_BUFFER, GL_TEXTURE_2D_ARRAY, m.shadowOutput().id);
        WolfTk::bindNamed(sh, "depthBuffer", 12, GL_TEXTURE_2D, f.depth_tex);
        std::vector<Att> atts = { { &m.cColor, 0 }, { &m.cDepthVelocity, 0 } };
        if (p.lightShafts) atts.push_back({ &m.cShadowLength, 0 });
        if (!m.fbo.bind(atts)) { sh.unbind(); return m.incomplete(vp); }
        drawScreen(lw, lh);
        sh.unbind();
        for (S32 u = 8; u <= 12; ++u) WolfTk::unbindUnit(u, u == 9 || u == 10 ? GL_TEXTURE_3D : GL_TEXTURE_2D);
        WolfTk::unbindUnit(sh.getTextureChannel(LLShaderMgr::WOLF_TK_SHADOW_BUFFER), GL_TEXTURE_2D_ARRAY);
    }
    {
        // the resolve (CloudsResolveMaterial: varianceGamma 2, temporalAlpha 0.1, CloudsResolveMaterial.ts:68-79)
        LLGLSLShader& sh = cloudsResolveSh;
        sh.bind();
        const S32 ri = m.resolveIndex, hi = 1 - m.resolveIndex;
        u2f(sh, "texelSize", 1.f / f.width, 1.f / f.height);   // setSize (:83-85)
        u1i(sh, "frame", (S32)m.frame);
        // onBeforeRender (:87-101)
        const glm::vec2 off = bayerOffset((S32)(m.frame % 16));
        u2f(sh, "jitterOffset", (off.x - 0.5f) * 4.f, (off.y - 0.5f) * 4.f);
        u1f(sh, "varianceGamma", 2.f);
        u1f(sh, "temporalAlpha", 0.1f);
        WolfTk::bindNamed(sh, "colorBuffer", 8, GL_TEXTURE_2D, m.cColor.id);
        WolfTk::bindNamed(sh, "depthVelocityBuffer", 9, GL_TEXTURE_2D, m.cDepthVelocity.id);
        WolfTk::bindNamed(sh, "colorHistoryBuffer", 10, GL_TEXTURE_2D, m.rColor[hi].id);
        if (p.lightShafts)
        {
            WolfTk::bindNamed(sh, "shadowLengthBuffer", 11, GL_TEXTURE_2D, m.cShadowLength.id);
            WolfTk::bindNamed(sh, "shadowLengthHistoryBuffer", 12, GL_TEXTURE_2D, m.rShadowLength[hi].id);
        }
        // [WolfViewer round 13] the scene depth for the resolve's history rejection (cloudsResolveF.glsl wolfSceneDistance)
        WolfTk::bindNamed(sh, "depthBuffer", 13, GL_TEXTURE_2D, f.depth_tex);
        u1f(sh, "cameraNear", f.near_clip);
        u1f(sh, "cameraFar", f.far_clip);
        std::vector<Att> atts = { { &m.rColor[ri], 0 } };
        if (p.lightShafts) atts.push_back({ &m.rShadowLength[ri], 0 });
        if (!m.fbo.bind(atts)) { sh.unbind(); return m.incomplete(vp); }
        drawScreen((S32)f.width, (S32)f.height);
        sh.unbind();
        for (S32 u = 8; u <= 13; ++u) WolfTk::unbindUnit(u, GL_TEXTURE_2D);
    }
    // copyReprojection (CloudsMaterial.ts:393-398), then swapBuffers (CloudsPass.ts:174-183): the history is the
    // texture just resolved, the output (outputBuffer :239-242) that history
    m.prevProj = f.proj;
    m.prevView = f.view;
    m.havePrev = true;
    m.resolveIndex = 1 - m.resolveIndex;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    return true;
}

bool Clouds::renderProbe(const Atmosphere& atmosphere, const CloudsFrame& f)
{
    Impl& m = *mImpl;
    if (!m.loaded || !m.ensureQuality(f.quality) || !m.proceduralDone || !m.shadowOutput().id) return false;
    const S32 q = llclamp(f.quality, 0, 3);
    const QualityPreset& p = QualityPreset::get(q);
    LLGLSLShader& sh = *m.clouds[q];
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);
    LLGLDisable cull(GL_CULL_FACE);
    LLGLDisable scissor(GL_SCISSOR_TEST);
    // The clouds pass without temporal upscaling (CloudsPass.setSize :217-219: the current target at full size,
    // CloudsMaterial.copyCameraSettings :344-353 non-temporal branch), its output used directly: a probe face has
    // no history to resolve against.
    alloc(m.pColor, GL_TEXTURE_2D, GL_RGBA16F, f.width, f.height);
    alloc(m.pDepthVelocity, GL_TEXTURE_2D, GL_RGBA16F, f.width, f.height);
    if (p.lightShafts) alloc(m.pShadowLength, GL_TEXTURE_2D, GL_R16F, f.width, f.height); else m.pShadowLength.release();
    const glm::mat4 w2e = glm::mat4(f.world_to_ecef);
    const glm::mat4 e2w = glm::mat4(glm::inverse(f.world_to_ecef));
    const F32 bottomRadius = atmosphere.bottomRadiusKm() * 1000.f;
    const F32 topRadius = atmosphere.topRadiusKm() * 1000.f;
    const glm::vec2 resolution((F32)f.width, (F32)f.height);
    sh.bind();
    m.sharedUniforms(sh, mLayers, f, w2e, e2w, bottomRadius, topRadius, false);
    atmosphere.setRuntimeUniforms(sh, f.pre_exposure);
    m.cloudsMaterialUniforms(sh, p, f);
    u2f(sh, "resolution", resolution.x, resolution.y);
    u2f(sh, "targetUvScale", 1.f, 1.f);
    // no reprojection for a face: the previous matrices are its own (the velocity it writes is not read)
    m.cameraUniforms(sh, f, false, resolution, bottomRadius, f.proj, f.view, m.mainView);
    m.copyShadow(sh);
    WolfTk::bindReserved(sh, LLShaderMgr::WOLF_TK_SHADOW_BUFFER, GL_TEXTURE_2D_ARRAY, m.shadowOutput().id);
    WolfTk::bindNamed(sh, "depthBuffer", 12, GL_TEXTURE_2D, f.depth_tex);
    std::vector<Att> atts = { { &m.pColor, 0 }, { &m.pDepthVelocity, 0 } };
    if (p.lightShafts) atts.push_back({ &m.pShadowLength, 0 });
    if (!m.fbo.bind(atts)) { sh.unbind(); return m.incomplete(vp); }
    drawScreen((S32)f.width, (S32)f.height);
    sh.unbind();
    for (S32 u = 8; u <= 12; ++u) WolfTk::unbindUnit(u, u == 9 || u == 10 ? GL_TEXTURE_3D : GL_TEXTURE_2D);
    WolfTk::unbindUnit(sh.getTextureChannel(LLShaderMgr::WOLF_TK_SHADOW_BUFFER), GL_TEXTURE_2D_ARRAY);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    return true;
}

// outputBuffer / shadowLengthBuffer (CloudsPass.ts:239-255): the history target after the swap
GLuint Clouds::colorTex() const { return mImpl->rColor[1 - mImpl->resolveIndex].id; }
GLuint Clouds::marchDepthVelocityTex(S32& w, S32& h) const { w = mImpl->cDepthVelocity.w; h = mImpl->cDepthVelocity.h; return mImpl->cDepthVelocity.id; }
GLuint Clouds::marchColorTex() const { return mImpl->cColor.id; }
GLuint Clouds::shadowLengthTex() const { return mImpl->rShadowLength[1 - mImpl->resolveIndex].id; }
GLuint Clouds::probeColorTex() const { return mImpl->pColor.id; }
GLuint Clouds::probeShadowLengthTex() const { return mImpl->pShadowLength.id; }
GLuint Clouds::shadowMapArray() const { return mImpl->shadowOutput().id; }
S32 Clouds::cascadeCount() const { return (S32)mImpl->shadowMaps.cascades.size(); }
LLVector2 Clouds::shadowMapSize() const { return LLVector2((F32)mImpl->shadowMaps.mapSize, (F32)mImpl->shadowMaps.mapSize); }
const F32* Clouds::shadowIntervals() const { return mImpl->intervals; }
const F32* Clouds::shadowMatrices() const { return mImpl->matrices; }
const F32* Clouds::inverseShadowMatrices() const { return mImpl->inverseMatrices; }
F32 Clouds::shadowFar() const { return (F32)mImpl->shadowMaps.far; }
F32 Clouds::shadowTopHeight() const { return mImpl->topHeight; }
F32 Clouds::shadowBottomHeight() const { return mImpl->bottomHeight; }
bool Clouds::lightShafts() const { return mImpl->cLightShafts; }

void Clouds::release()
{
    Impl& m = *mImpl;
    for (Tex* t : { &m.shapeTex, &m.shapeDetailTex, &m.localWeatherTex, &m.turbulenceTex, &m.shadowCurrentTex, &m.shadowResolveTex, &m.shadowHistoryTex,
                    &m.cColor, &m.cDepthVelocity, &m.cShadowLength, &m.rColor[0], &m.rColor[1], &m.rShadowLength[0], &m.rShadowLength[1],
                    &m.pColor, &m.pDepthVelocity, &m.pShadowLength })
    {
        t->release();
    }
    m.fbo.release();
    m.proceduralDone = false;
    m.proceduralFailed = false;
    m.shadowCascadeCount = m.shadowMapSizeV = 0;
    m.cWidth = m.cHeight = 0;
    m.havePrev = false;
    m.lastQuality = -1;
}
}
