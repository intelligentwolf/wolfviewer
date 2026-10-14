/**
 * @file wolftkgl.h
 * @brief WolfViewer: small GL helpers shared by the translated three-geospatial renderer
 *        (wolftkatmosphere.cpp, wolftkclouds.cpp, wolfenvironmentrender.cpp).
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

#ifndef WOLF_TK_GL_H
#define WOLF_TK_GL_H

#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llshadermgr.h"

/**
 * [WOLF ENV TAKRAM 2026-10-12] Texture binding rules for the translated passes (three.js binds a
 * sampler to whatever unit it likes; LL assigns units itself):
 *   - the five atmosphere tables and the clouds' shadow map use LL's RESERVED sampler names
 *     (transmittance_texture, scattering_texture, single_mie_scattering_texture, irradiance_texture,
 *     higher_order_scattering_texture, shadowBuffer): LL gives each program its own channel for them
 *     (LLGLSLShader::mapUniform), so they can also be bound into stock programs (softenLight, water, alpha)
 *     without colliding with LL's own textures. bindReserved() binds them there.
 *   - every other sampler of OUR OWN programs is set to an explicit unit from NAMED_UNIT_BASE up
 *     (bindNamed); LL's reserved channels in those programs are all below it (at most the six above plus
 *     LL's own few), and OpenGL 3.3 guarantees 16 units (GL_MAX_TEXTURE_IMAGE_UNITS >= 16).
 */
namespace WolfTk
{
    constexpr S32 NAMED_UNIT_BASE = 8;
    constexpr S32 NAMED_UNIT_LAST = 15;

    /** Binds raw texture `tex` (target GL_TEXTURE_2D, GL_TEXTURE_3D or GL_TEXTURE_2D_ARRAY) to the program's
     *  LL-assigned channel for reserved sampler `reserved`. Returns the channel, -1 if the program has none. */
    inline S32 bindReserved(LLGLSLShader& sh, S32 reserved, GLenum target, GLuint tex)
    {
        const S32 ch = sh.getTextureChannel(reserved);
        if (ch < 0) return -1;
        LLTexUnit* unit = gGL.getTexUnit(ch);
        if (target == GL_TEXTURE_3D)
        {
            unit->bindManual(LLTexUnit::TT_TEXTURE_3D, tex);
        }
        else if (target == GL_TEXTURE_2D)
        {
            unit->bindManual(LLTexUnit::TT_TEXTURE, tex);
        }
        else
        {
            // LLTexUnit has no 2D-array type: bound beside its own state (a different target on the unit, so the
            // unit's cached GL_TEXTURE_2D / 3D bindings stay true); left bound - no LL code samples 2D arrays
            unit->activate();
            glBindTexture(target, tex);
        }
        return ch;
    }

    /** Sets sampler uniform `name` of our own program to unit `unit` (NAMED_UNIT_BASE..LAST) and binds `tex`. */
    inline bool bindNamed(LLGLSLShader& sh, const char* name, S32 unit, GLenum target, GLuint tex)
    {
        llassert(unit >= NAMED_UNIT_BASE && unit <= NAMED_UNIT_LAST);
        const S32 loc = sh.getUniformLocation(LLStaticHashedString(name));
        if (loc < 0) return false;
        // the rule above, checked: LL's channels for this program's reserved samplers (LLGLSLShader::mTexture)
        // must all lie below the named units, or two samplers would share a unit
        if (sh.mActiveTextureChannels > NAMED_UNIT_BASE)
        {
            static bool s_warned = false;
            if (!s_warned)
            {
                LL_WARNS("WolfEnv") << sh.mName << ": " << sh.mActiveTextureChannels << " reserved texture channels reach the named units ("
                                    << NAMED_UNIT_BASE << "+); its samplers may collide" << LL_ENDL;
                s_warned = true;
            }
        }
        glUniform1i(loc, unit);
        LLTexUnit* u = gGL.getTexUnit(unit);
        if (target == GL_TEXTURE_3D) u->bindManual(LLTexUnit::TT_TEXTURE_3D, tex);
        else if (target == GL_TEXTURE_2D) u->bindManual(LLTexUnit::TT_TEXTURE, tex);
        else { u->activate(); glBindTexture(target, tex); }
        return true;
    }

    /** Unbinds what bindNamed / bindReserved put on `unit` for `target`. */
    inline void unbindUnit(S32 unit, GLenum target)
    {
        if (unit < 0) return;
        LLTexUnit* u = gGL.getTexUnit(unit);
        if (target == GL_TEXTURE_3D) u->unbind(LLTexUnit::TT_TEXTURE_3D);
        else if (target == GL_TEXTURE_2D) u->unbind(LLTexUnit::TT_TEXTURE);
        else { u->activate(); glBindTexture(target, 0); }
    }
}

#endif // WOLF_TK_GL_H
