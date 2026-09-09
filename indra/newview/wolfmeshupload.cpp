/**
 * @file wolfmeshupload.cpp
 * @brief WolfViewer: WolfStorm's model uploader — straight to inventory, no LODs, no physics.
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

#include "wolfmeshupload.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

#include <boost/json.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

// Declarations only. TINYGLTF_IMPLEMENTATION is defined exactly once in the viewer, by
// indra/newview/gltf/llgltfloader.cpp:32 — this translation unit must not define it again.
#include "tinygltf/tiny_gltf.h"

#include "indra_constants.h"
#include "llagent.h"
#include "llagentbenefits.h"
#include "llagentdata.h"
#include "llbase64.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldatapacker.h"
#include "lldir.h"
#include "llfile.h"
#include "llfloaterperms.h"
#include "llhttpconstants.h"
#include "llinventorymodel.h"
#include "llprimitive.h"
#include "llsdjson.h"
#include "llviewerassetupload.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "wolfgrid.h"

// Source: wolfstorm/js/ui/floaters/floater_mesh_upload.js. Every number, every conversion and
// every field of the request body below is that file's, and the citations point at it.

namespace
{
    /** The capability every texture upload goes through — the ordinary one, so a texture that
     *  rides along with a model costs and behaves exactly like any other texture upload.
     *  Source: llmeshrepository.cpp:2707 mWholeModelFeeCapability uses the same name. */
    const char* const TEXTURE_UPLOAD_CAP = "NewFileAgentInventory";

    /** Source: rust_proxy/src/main.rs:3896 — the route is POST /upload_mesh on the proxy's
     *  TLS HTTP port (main.rs:46-92 HTTP_PORT = 8080). */
    std::string uploadMeshUrl()
    {
        return std::string(WolfGrid::MESH_API_BASE) + "/upload_mesh";
    }

    // ───────────────────────────── small helpers ─────────────────────────────

    /** A JSON string literal, escaped by Boost.Json rather than by hand. */
    std::string jsonString(const std::string& s)
    {
        return boost::json::serialize(boost::json::value(s));
    }

    /** Six decimals. The proxy quantises positions to u16 over [-0.5, 0.5]
     *  (main.rs:1740-1745 quant_u16_le), a step of 1.5e-5, so six decimals is finer than the
     *  format can represent and no precision is lost by writing the body as text. */
    void appendF32(std::ostringstream& o, F32 v)
    {
        o << llformat("%.6f", (F64)v);
    }

    void appendVec3(std::ostringstream& o, const LLVector3& v)
    {
        o << '[';
        appendF32(o, v.mV[VX]); o << ',';
        appendF32(o, v.mV[VY]); o << ',';
        appendF32(o, v.mV[VZ]);
        o << ']';
    }

    bool isFinite3(const LLVector3& v)
    {
        return std::isfinite(v.mV[VX]) && std::isfinite(v.mV[VY]) && std::isfinite(v.mV[VZ]);
    }

    /** Prim scale limits on an OpenSim grid.
     *  Source: xform.h:47-48 OS_DEFAULT_MAX_PRIM_SCALE = 256.f, OS_MIN_PRIM_SCALE = 0.001f,
     *  applied by llworld.cpp:170-171. The 0.01/64 pair is Second Life's, not OpenSim's. */
    F32 clampPrimScale(F32 v)
    {
        return llclamp(v, OS_MIN_PRIM_SCALE, OS_DEFAULT_MAX_PRIM_SCALE);
    }

    /** Decide a file extension from the image's own bytes.
     *  LLNewFileResourceUploadInfo::exportTempFile (llviewerassetupload.cpp:411-424) picks the
     *  codec from the FILE EXTENSION, so a texture lifted out of a GLB's binary chunk has to be
     *  written to a temp file whose name says what it is. Sniffing the magic beats trusting a
     *  glTF mimeType, which exporters get wrong. */
    std::string sniffImageExtension(const std::vector<U8>& bytes)
    {
        // PNG signature — RFC 2083 §3.1: 89 50 4E 47 0D 0A 1A 0A
        static const U8 PNG_SIG[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
        if (bytes.size() >= 8 && memcmp(bytes.data(), PNG_SIG, 8) == 0)
        {
            return "png";
        }
        // JPEG SOI + first marker — ITU-T T.81: FF D8 FF
        if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
        {
            return "jpg";
        }
        // BMP file header — "BM"
        if (bytes.size() >= 2 && bytes[0] == 0x42 && bytes[1] == 0x4D)
        {
            return "bmp";
        }
        return std::string();
    }

    /** Materials that carry a map SL has no face slot for, named so a "no textures" result can
     *  say what the model does have instead of just what it lacks. */
    void noteMaterialSlot(WolfMeshUpload::Submesh& sub, const char* slot)
    {
        sub.mMaterialSlots.push_back(slot);
    }

    /** The .llmesh carries a submesh's Normal and TexCoord0 arrays whole or not at all
     *  (rust_proxy/src/main.rs:1748-1781 encode_submesh), so an array that does not cover every
     *  vertex is dropped rather than padded. Source: floater_mesh_upload.js:771-772 (hasN/hasU). */
    void dropPartialArrays(WolfMeshUpload::Submesh& sub)
    {
        if (sub.mNormals.size() != sub.mPositions.size())
        {
            sub.mNormals.clear();
        }
        if (sub.mTexCoords.size() != sub.mPositions.size())
        {
            sub.mTexCoords.clear();
        }
    }
}

namespace WolfMeshUpload
{

bool isSupportedExtension(const std::string& extension)
{
    std::string ext = extension;
    LLStringUtil::toLower(ext);
    if (!ext.empty() && ext[0] == '.')
    {
        ext.erase(0, 1);
    }
    return ext == "obj" || ext == "gltf" || ext == "glb";
}

bool isAvailable()
{
    if (!WolfGrid::isWolfTerritories())
    {
        return false;
    }
    if (gAgentID.isNull())
    {
        return false;
    }
    return gAgent.getRegion() != NULL;
}

// ───────────────────────────────── OBJ ─────────────────────────────────

namespace
{
    /** One .mtl material: the diffuse map's basename and the Kd colour. */
    struct MtlEntry
    {
        std::string mMapKd;                    // resolved absolute path, empty if unresolved
        std::string mMapKdRaw;                 // as written in the .mtl, for the error message
        LLColor4    mKd { 1.f, 1.f, 1.f, 1.f };
        bool        mHasKd = false;
    };

    /** Read the .mtl an OBJ names with `mtllib`, resolved next to the OBJ.
     *  WolfStorm matches map_Kd against the files the user picked in the browser
     *  (floater_mesh_upload.js:307-329 _parseMtl); a desktop viewer has the real directory, so
     *  the path is resolved relative to the OBJ instead. Same result, no second file picker. */
    void parseMtl(const std::string& mtl_path, std::map<std::string, MtlEntry>& out)
    {
        llifstream in(mtl_path.c_str());
        if (!in.is_open())
        {
            return;
        }
        const std::string dir = gDirUtilp->getDirName(mtl_path);
        const std::string& delim = gDirUtilp->getDirDelimiter();
        MtlEntry* cur = NULL;
        std::string line;
        while (std::getline(in, line))
        {
            LLStringUtil::trim(line);
            if (line.empty() || line[0] == '#')
            {
                continue;
            }
            std::istringstream ls(line);
            std::string tag;
            ls >> tag;
            std::string tag_lc = tag;
            LLStringUtil::toLower(tag_lc);
            if (tag_lc == "newmtl")
            {
                std::string name;
                std::getline(ls, name);
                LLStringUtil::trim(name);
                cur = &out[name];
            }
            else if (!cur)
            {
                continue;
            }
            else if (tag_lc == "map_kd")
            {
                // map_Kd may carry options (-s, -o, -bm …) before the filename; the path is the
                // last token. Source: floater_mesh_upload.js:319-323, same rule.
                std::string tok, last;
                while (ls >> tok)
                {
                    last = tok;
                }
                if (last.empty())
                {
                    continue;
                }
                cur->mMapKdRaw = last;
                std::replace(last.begin(), last.end(), '\\', '/');
                const size_t slash = last.find_last_of('/');
                const std::string base = (slash == std::string::npos) ? last : last.substr(slash + 1);
                const std::string candidate = dir + delim + base;
                if (LLFile::isfile(candidate))
                {
                    cur->mMapKd = candidate;
                }
            }
            else if (tag_lc == "kd")
            {
                F32 r = 1.f, g = 1.f, b = 1.f;
                if (ls >> r >> g >> b)
                {
                    cur->mKd.set(r, g, b, 1.f);
                    cur->mHasKd = true;
                }
            }
        }
    }

    /** An OBJ face vertex reference: v, v/vt, v//vn or v/vt/vn, 1-based, negative = from end. */
    struct ObjRef
    {
        S32 mV = -1;
        S32 mT = -1;
        S32 mN = -1;
    };

    S32 objIndex(const std::string& token, size_t count)
    {
        if (token.empty())
        {
            return -1;
        }
        const S32 n = atoi(token.c_str());
        if (n == 0)
        {
            return -1;
        }
        return (n < 0) ? (S32)count + n : n - 1;
    }

    ObjRef parseObjRef(const std::string& token, size_t nv, size_t nt, size_t nn)
    {
        ObjRef r;
        std::string parts[3];
        S32 p = 0;
        for (char c : token)
        {
            if (c == '/')
            {
                if (++p > 2)
                {
                    break;
                }
            }
            else
            {
                parts[p] += c;
            }
        }
        r.mV = objIndex(parts[0], nv);
        r.mT = objIndex(parts[1], nt);
        r.mN = objIndex(parts[2], nn);
        return r;
    }
}

/**
 * OBJ → per-material vertex/triangle lists in SL space.
 *
 * Source: floater_mesh_upload.js:243-305 _parseObj. Vertices are converted Y-up → SL Z-up here:
 * (x, y, z) → (x, -z, y). `usemtl` names the material; the .mtl named by `mtllib` supplies its
 * `map_Kd`, the OBJ equivalent of LLImportMaterial::mDiffuseMapFilename
 * (llmeshrepository.cpp:2919-2926). OBJ texture coordinates run bottom-up, the same way the
 * .llmesh TexCoord0 domain does, so V is NOT flipped here — only the glTF path flips it.
 */
bool Model::loadObj(const std::string& path, std::string& error)
{
    llifstream in(path.c_str());
    if (!in.is_open())
    {
        error = "Could not open " + gDirUtilp->getBaseFileName(path) + ".";
        return false;
    }

    const std::string dir = gDirUtilp->getDirName(path);
    const std::string& delim = gDirUtilp->getDirDelimiter();

    std::vector<LLVector3> V;
    std::vector<LLVector2> VT;
    std::vector<LLVector3> VN;
    std::map<std::string, MtlEntry> materials;

    // material name -> index into groups; a group is one submesh under construction
    struct Group
    {
        Submesh                        mSub;
        std::map<std::string, U32>     mRemap;   // "v/vt/vn" token -> local vertex index
        // A submesh keeps its normals/UVs only if EVERY vertex had one, so track the gaps as
        // they happen rather than inferring them afterwards.
        bool                           mMissingNormal = false;
        bool                           mMissingUV = false;
    };
    std::vector<Group> groups;
    std::map<std::string, size_t> group_of;

    auto ensureGroup = [&](const std::string& name) -> Group&
    {
        auto it = group_of.find(name);
        if (it != group_of.end())
        {
            return groups[it->second];
        }
        group_of[name] = groups.size();
        groups.emplace_back();
        groups.back().mSub.mMaterialName = name;
        return groups.back();
    };

    Group* cur = &ensureGroup("default");

    std::string line;
    while (std::getline(in, line))
    {
        LLStringUtil::trim(line);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;

        if (tag == "v")
        {
            F32 x = 0.f, y = 0.f, z = 0.f;
            ls >> x >> y >> z;
            V.emplace_back(x, y, z);
        }
        else if (tag == "vt")
        {
            F32 u = 0.f, v = 0.f;
            ls >> u >> v;
            VT.emplace_back(u, v);
        }
        else if (tag == "vn")
        {
            F32 x = 0.f, y = 0.f, z = 0.f;
            ls >> x >> y >> z;
            VN.emplace_back(x, y, z);
        }
        else if (tag == "mtllib")
        {
            std::string rest;
            std::getline(ls, rest);
            LLStringUtil::trim(rest);
            if (!rest.empty())
            {
                std::replace(rest.begin(), rest.end(), '\\', '/');
                const size_t slash = rest.find_last_of('/');
                const std::string base = (slash == std::string::npos) ? rest : rest.substr(slash + 1);
                parseMtl(dir + delim + base, materials);
            }
        }
        else if (tag == "usemtl")
        {
            std::string name;
            std::getline(ls, name);
            LLStringUtil::trim(name);
            cur = &ensureGroup(name.empty() ? std::string("default") : name);
        }
        else if (tag == "f")
        {
            std::vector<U32> corners;
            std::string token;
            while (ls >> token)
            {
                auto found = cur->mRemap.find(token);
                if (found != cur->mRemap.end())
                {
                    corners.push_back(found->second);
                    continue;
                }
                const ObjRef r = parseObjRef(token, V.size(), VT.size(), VN.size());
                if (r.mV < 0 || (size_t)r.mV >= V.size())
                {
                    continue;   // a face referring to a vertex that does not exist
                }
                const LLVector3& p = V[r.mV];
                // Y-up → SL Z-up: (x, y, z) → (x, -z, y). Source: floater_mesh_upload.js:252.
                const U32 local = (U32)cur->mSub.mPositions.size();
                cur->mSub.mPositions.emplace_back(p.mV[VX], -p.mV[VZ], p.mV[VY]);
                if (r.mN >= 0 && (size_t)r.mN < VN.size())
                {
                    const LLVector3& n = VN[r.mN];
                    cur->mSub.mNormals.emplace_back(n.mV[VX], -n.mV[VZ], n.mV[VY]);
                }
                else
                {
                    // Keep the arrays index-aligned so a later vertex still lines up; the whole
                    // array is dropped below once we know it is incomplete.
                    cur->mSub.mNormals.emplace_back(0.f, 0.f, 0.f);
                    cur->mMissingNormal = true;
                }
                if (r.mT >= 0 && (size_t)r.mT < VT.size())
                {
                    cur->mSub.mTexCoords.push_back(VT[r.mT]);
                }
                else
                {
                    cur->mSub.mTexCoords.emplace_back(0.f, 0.f);
                    cur->mMissingUV = true;
                }
                cur->mRemap[token] = local;
                corners.push_back(local);
            }
            // Fan-triangulate the polygon, keeping the OBJ winding.
            // Source: floater_mesh_upload.js:299-300.
            for (size_t i = 1; i + 1 < corners.size(); ++i)
            {
                cur->mSub.mIndices.push_back(corners[0]);
                cur->mSub.mIndices.push_back(corners[i]);
                cur->mSub.mIndices.push_back(corners[i + 1]);
            }
        }
    }

    for (Group& g : groups)
    {
        if (g.mSub.mIndices.empty())
        {
            continue;
        }
        if (g.mMissingNormal)
        {
            g.mSub.mNormals.clear();
        }
        if (g.mMissingUV)
        {
            g.mSub.mTexCoords.clear();
        }
        dropPartialArrays(g.mSub);

        auto m = materials.find(g.mSub.mMaterialName);
        if (m != materials.end())
        {
            if (!m->second.mMapKd.empty())
            {
                g.mSub.mTexturePath = m->second.mMapKd;
                g.mSub.mTextureKey = "file:" + m->second.mMapKd;
            }
            else if (!m->second.mMapKdRaw.empty())
            {
                mTextureErrors.push_back(m->second.mMapKdRaw);
            }
            if (m->second.mHasKd)
            {
                g.mSub.mDiffuseColor = m->second.mKd;
            }
        }
        mSubmeshes.push_back(std::move(g.mSub));
    }

    if (mSubmeshes.empty())
    {
        error = "No triangles found in that OBJ.";
        return false;
    }
    return true;
}

// ──────────────────────────────── glTF / GLB ────────────────────────────────

namespace
{
    /**
     * Does an accessor's last element actually lie inside the buffer?
     *
     * Every term here is attacker-controlled: a glTF file states `count`, `byteOffset` and
     * `byteStride` as plain JSON numbers and tinygltf does not check them against the buffer it
     * loaded. The obvious form of this test — `base + (count - 1) * stride + elem > size` —
     * WRAPS for a large count and then passes, after which the caller sizes a vector from the
     * same count and throws std::length_error or std::bad_alloc out of the parse. So no addition
     * or multiplication here may overflow: each step is bounded by division against what is
     * actually left.
     */
    bool accessorFits(size_t buffer_size, size_t view_offset, size_t accessor_offset,
                      size_t count, size_t stride, size_t elem_bytes)
    {
        if (stride == 0 || elem_bytes == 0)
        {
            return false;
        }
        if (view_offset > buffer_size || accessor_offset > buffer_size - view_offset)
        {
            return false;
        }
        const size_t base = view_offset + accessor_offset;
        const size_t avail = buffer_size - base;
        if (count == 0)
        {
            return true;
        }
        // The last element begins at (count - 1) * stride; that product must not overflow AND
        // must leave room for one whole element.
        if ((count - 1) > avail / stride)
        {
            return false;
        }
        const size_t last = (count - 1) * stride;
        return elem_bytes <= avail - last;
    }

    /** Read one scalar component out of an accessor's buffer and return it as F32.
     *  Source: glTF 2.0 §3.6.2.2 — the accessor component types and the normalized rule
     *  (unsigned normalized: v / MAX; signed normalized: max(v / MAX, -1)). */
    F32 readComponent(const U8* p, S32 component_type, bool normalized)
    {
        switch (component_type)
        {
        case TINYGLTF_COMPONENT_TYPE_FLOAT:
        {
            F32 v;
            memcpy(&v, p, sizeof(F32));
            return v;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        {
            const U8 v = *p;
            return normalized ? (F32)v / 255.f : (F32)v;
        }
        case TINYGLTF_COMPONENT_TYPE_BYTE:
        {
            S8 v;
            memcpy(&v, p, sizeof(S8));
            return normalized ? llmax((F32)v / 127.f, -1.f) : (F32)v;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        {
            U16 v;
            memcpy(&v, p, sizeof(U16));
            return normalized ? (F32)v / 65535.f : (F32)v;
        }
        case TINYGLTF_COMPONENT_TYPE_SHORT:
        {
            S16 v;
            memcpy(&v, p, sizeof(S16));
            return normalized ? llmax((F32)v / 32767.f, -1.f) : (F32)v;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        {
            U32 v;
            memcpy(&v, p, sizeof(U32));
            return (F32)v;
        }
        default:
            return 0.f;
        }
    }

    /** Every element of an accessor, flattened to F32, `components` values per element.
     *  Honours byteStride (Accessor::ByteStride, tiny_gltf.h:868-898). Sparse accessors are
     *  rejected rather than silently mis-read — see the caller's message. */
    bool readAccessor(const tinygltf::Model& model, S32 accessor_index,
                      S32 want_components, std::vector<F32>& out, size_t& count)
    {
        if (accessor_index < 0 || (size_t)accessor_index >= model.accessors.size())
        {
            return false;
        }
        const tinygltf::Accessor& acc = model.accessors[accessor_index];
        if (acc.sparse.isSparse)
        {
            return false;
        }
        if (acc.bufferView < 0 || (size_t)acc.bufferView >= model.bufferViews.size())
        {
            return false;
        }
        const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
        if (bv.buffer < 0 || (size_t)bv.buffer >= model.buffers.size())
        {
            return false;
        }
        const std::vector<unsigned char>& data = model.buffers[bv.buffer].data;

        const S32 comps = tinygltf::GetNumComponentsInType((U32)acc.type);
        const S32 csize = tinygltf::GetComponentSizeInBytes((U32)acc.componentType);
        const S32 stride = acc.ByteStride(bv);
        if (comps <= 0 || csize <= 0 || stride <= 0)
        {
            return false;
        }
        // OVERFLOW-SAFE BOUNDS CHECK. acc.count comes straight out of the file's JSON with no
        // relation to the buffer's real size (tinygltf ParseAccessor does not cross-check it), so
        // computing `base + (count-1)*stride + comps*csize` and comparing wraps for a large
        // enough count and lets a malformed model through — after which `out.assign(count * 3)`
        // asks for petabytes and throws out of the parse. Divide instead of multiply.
        if (!accessorFits(data.size(), bv.byteOffset, acc.byteOffset, acc.count,
                          (size_t)stride, (size_t)comps * (size_t)csize))
        {
            return false;
        }
        const size_t base = bv.byteOffset + acc.byteOffset;

        count = acc.count;
        out.assign(acc.count * (size_t)want_components, 0.f);
        for (size_t i = 0; i < acc.count; ++i)
        {
            const U8* elem = data.data() + base + i * (size_t)stride;
            for (S32 c = 0; c < want_components; ++c)
            {
                out[i * (size_t)want_components + c] = (c < comps)
                    ? readComponent(elem + c * csize, acc.componentType, acc.normalized)
                    : 0.f;
            }
        }
        return true;
    }

    /**
     * An index accessor, read as integers.
     *
     * Deliberately NOT routed through readAccessor's F32 pipe: glTF index accessors may be
     * UNSIGNED_INT (glTF 2.0 §3.6.2.3), and an F32 cannot hold an integer above 2^24 exactly, so
     * a large mesh would come back with silently wrong triangles.
     */
    bool readIndices(const tinygltf::Model& model, S32 accessor_index, std::vector<U32>& out)
    {
        if (accessor_index < 0 || (size_t)accessor_index >= model.accessors.size())
        {
            return false;
        }
        const tinygltf::Accessor& acc = model.accessors[accessor_index];
        if (acc.sparse.isSparse || acc.bufferView < 0 || (size_t)acc.bufferView >= model.bufferViews.size())
        {
            return false;
        }
        const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
        if (bv.buffer < 0 || (size_t)bv.buffer >= model.buffers.size())
        {
            return false;
        }
        const std::vector<unsigned char>& data = model.buffers[bv.buffer].data;
        const S32 csize = tinygltf::GetComponentSizeInBytes((U32)acc.componentType);
        const S32 stride = acc.ByteStride(bv);
        if (csize <= 0 || stride <= 0)
        {
            return false;
        }
        // Same overflow-safe check as readAccessor.
        if (!accessorFits(data.size(), bv.byteOffset, acc.byteOffset, acc.count,
                          (size_t)stride, (size_t)csize))
        {
            return false;
        }
        const size_t base = bv.byteOffset + acc.byteOffset;

        out.clear();
        out.reserve(acc.count);
        for (size_t i = 0; i < acc.count; ++i)
        {
            const U8* p = data.data() + base + i * (size_t)stride;
            switch (acc.componentType)
            {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                out.push_back((U32)*p);
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            {
                U16 v;
                memcpy(&v, p, sizeof(U16));
                out.push_back((U32)v);
                break;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            {
                U32 v;
                memcpy(&v, p, sizeof(U32));
                out.push_back(v);
                break;
            }
            default:
                return false;   // signed component types are not legal for indices
            }
        }
        return true;
    }

    /** A node's local transform: `matrix` if present, otherwise T * R * S.
     *  Source: glTF 2.0 §3.5.2 — matrix is column-major, which is glm's layout too. */
    glm::mat4 nodeLocalMatrix(const tinygltf::Node& node)
    {
        if (node.matrix.size() == 16)
        {
            glm::mat4 m(1.f);
            F32* dst = glm::value_ptr(m);
            for (S32 i = 0; i < 16; ++i)
            {
                dst[i] = (F32)node.matrix[i];
            }
            return m;
        }
        glm::mat4 m(1.f);
        if (node.translation.size() == 3)
        {
            m = glm::translate(glm::mat4(1.f),
                               glm::vec3((F32)node.translation[0], (F32)node.translation[1], (F32)node.translation[2]));
        }
        if (node.rotation.size() == 4)
        {
            // glTF stores the quaternion xyzw; glm::quat's constructor takes wxyz.
            const glm::quat q((F32)node.rotation[3], (F32)node.rotation[0],
                              (F32)node.rotation[1], (F32)node.rotation[2]);
            m = m * glm::mat4_cast(q);
        }
        if (node.scale.size() == 3)
        {
            m = glm::scale(m, glm::vec3((F32)node.scale[0], (F32)node.scale[1], (F32)node.scale[2]));
        }
        return m;
    }
}

/**
 * glTF / GLB → per-primitive vertex/triangle lists in SL space.
 *
 * Source: floater_mesh_upload.js:451-600 _parseGltf. Three things this has to get right, and
 * they are the same three there:
 *  - NODE TRANSFORMS. A primitive's positions are in its node's local space, so the geometry is
 *    baked through the node's world matrix, and normals through that matrix's normal matrix.
 *  - HANDEDNESS. glTF is Y-up, so the same (x, y, z) → (x, -z, y) the OBJ path uses converts to
 *    SL Z-up.
 *  - UV ORIGIN. glTF UVs run top-down (glTF 2.0 §3.7.2.1) while the .llmesh TexCoord0 domain
 *    runs bottom-up, so V is flipped: v' = 1 - v. Source: floater_mesh_upload.js:465-467, 549.
 *
 * Images are taken from the container as BYTES and never decoded here. tinygltf's default image
 * loader decodes every image with stb_image, which for a 4096x4096 RGBA PNG is 67 MB of pixels
 * this uploader has no use for — the texture upload path wants the original file. A no-op image
 * loader is installed instead and the bytes are captured as they go past. Same reasoning as
 * floater_mesh_upload.js:340-372 _gltfBaseColorBytes.
 */
bool Model::loadGltf(const std::string& path, std::string& error)
{
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    // image index -> the file's own bytes, captured instead of decoded.
    std::map<S32, std::vector<U8>> image_bytes;
    loader.SetImageLoader(
        [&image_bytes](tinygltf::Image* image, const int image_idx, std::string* /*err*/,
                       std::string* /*warn*/, int /*req_width*/, int /*req_height*/,
                       const unsigned char* bytes, int size, void* /*user*/) -> bool
        {
            if (bytes && size > 0)
            {
                image_bytes[image_idx].assign(bytes, bytes + size);
            }
            // Nothing is decoded, so leave the pixel fields as tinygltf initialised them.
            // Returning true keeps a model whose images we do not need from failing to load.
            (void)image;
            return true;
        },
        NULL);

    std::string ext = gDirUtilp->getExtension(path);
    const bool ok = (ext == "glb")
        ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
        : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!warn.empty())
    {
        LL_WARNS("WolfMeshUpload") << "glTF warning: " << warn << LL_ENDL;
    }
    if (!ok)
    {
        error = err.empty() ? std::string("That glTF file could not be read.") : ("glTF parse failed: " + err);
        return false;
    }

    // Draco and meshopt are extensions tinygltf does not decompress; a model needing one loads
    // with empty accessors and would otherwise upload as nothing. Say which, do not swallow it.
    for (const std::string& e : model.extensionsRequired)
    {
        if (e == "KHR_draco_mesh_compression")
        {
            error = "This model uses Draco compression, which WolfViewer cannot decode. Re-export without Draco.";
            return false;
        }
        if (e == "EXT_meshopt_compression")
        {
            error = "This model uses meshopt compression, which WolfViewer cannot decode. Re-export without it.";
            return false;
        }
    }

    // Which glTF IMAGE each material's base colour comes from, so two materials sharing one
    // image upload it once. Source: floater_mesh_upload.js:439-447, keyed on the image.
    auto baseColorImage = [&model](S32 material_index) -> S32
    {
        if (material_index < 0 || (size_t)material_index >= model.materials.size())
        {
            return -1;
        }
        const S32 tex = model.materials[material_index].pbrMetallicRoughness.baseColorTexture.index;
        if (tex < 0 || (size_t)tex >= model.textures.size())
        {
            return -1;
        }
        return model.textures[tex].source;
    };

    // Walk the scene so node transforms are applied; a node reached from no scene is not drawn
    // and is not uploaded either.
    struct Pending
    {
        S32       mNode;
        glm::mat4 mParent;
    };
    std::vector<Pending> stack;
    const S32 scene_index = (model.defaultScene >= 0 && (size_t)model.defaultScene < model.scenes.size())
        ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
    if (scene_index >= 0)
    {
        for (S32 n : model.scenes[scene_index].nodes)
        {
            stack.push_back({ n, glm::mat4(1.f) });
        }
    }
    else
    {
        // No scene at all: treat every node as a root, which is what a viewer does with a
        // scene-less asset rather than showing nothing.
        for (size_t n = 0; n < model.nodes.size(); ++n)
        {
            stack.push_back({ (S32)n, glm::mat4(1.f) });
        }
    }

    std::set<S32> visited;
    U32 skipped_sparse = 0;
    while (!stack.empty())
    {
        const Pending p = stack.back();
        stack.pop_back();
        if (p.mNode < 0 || (size_t)p.mNode >= model.nodes.size() || !visited.insert(p.mNode).second)
        {
            continue;
        }
        const tinygltf::Node& node = model.nodes[p.mNode];
        const glm::mat4 world = p.mParent * nodeLocalMatrix(node);
        for (S32 child : node.children)
        {
            stack.push_back({ child, world });
        }
        if (node.mesh < 0 || (size_t)node.mesh >= model.meshes.size())
        {
            continue;
        }
        const glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(world)));

        for (const tinygltf::Primitive& prim : model.meshes[node.mesh].primitives)
        {
            // Points, lines, strips and fans have no place in a mesh asset. -1 is tinygltf's
            // struct default (tiny_gltf.h:973); its parser substitutes the glTF default of
            // TRIANGLES (tiny_gltf.h:5105), so -1 only survives on a primitive nothing parsed.
            if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1)
            {
                continue;
            }
            auto pos_it = prim.attributes.find("POSITION");
            if (pos_it == prim.attributes.end())
            {
                continue;
            }
            std::vector<F32> pos;
            size_t vert_count = 0;
            if (!readAccessor(model, pos_it->second, 3, pos, vert_count) || vert_count == 0)
            {
                ++skipped_sparse;
                continue;
            }

            std::vector<F32> nrm;
            size_t nrm_count = 0;
            auto nrm_it = prim.attributes.find("NORMAL");
            const bool has_normals = (nrm_it != prim.attributes.end())
                && readAccessor(model, nrm_it->second, 3, nrm, nrm_count)
                && nrm_count == vert_count;

            std::vector<F32> uv;
            size_t uv_count = 0;
            auto uv_it = prim.attributes.find("TEXCOORD_0");
            const bool has_uvs = (uv_it != prim.attributes.end())
                && readAccessor(model, uv_it->second, 2, uv, uv_count)
                && uv_count == vert_count;

            // Indices, or an implicit 0..n-1 run when the primitive has none (glTF 2.0 §3.7.2).
            std::vector<U32> indices;
            if (prim.indices >= 0)
            {
                if (!readIndices(model, prim.indices, indices))
                {
                    ++skipped_sparse;
                    continue;
                }
            }
            else
            {
                indices.reserve(vert_count);
                for (size_t i = 0; i < vert_count; ++i)
                {
                    indices.push_back((U32)i);
                }
            }

            Submesh sub;
            sub.mPositions.reserve(vert_count);
            if (has_normals)
            {
                sub.mNormals.reserve(vert_count);
            }
            if (has_uvs)
            {
                sub.mTexCoords.reserve(vert_count);
            }
            for (size_t i = 0; i < vert_count; ++i)
            {
                const glm::vec4 wp = world * glm::vec4(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2], 1.f);
                // Y-up → SL Z-up, same as the OBJ path.
                sub.mPositions.emplace_back(wp.x, -wp.z, wp.y);
                if (has_normals)
                {
                    glm::vec3 wn = normal_matrix * glm::vec3(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]);
                    const F32 len = glm::length(wn);
                    if (len > 0.f)
                    {
                        wn /= len;
                    }
                    sub.mNormals.emplace_back(wn.x, -wn.z, wn.y);
                }
                if (has_uvs)
                {
                    sub.mTexCoords.emplace_back(uv[i * 2], 1.f - uv[i * 2 + 1]);
                }
            }
            // Drop any triangle that points outside the vertex array rather than truncating the
            // whole primitive: a single bad index in an otherwise good export is common.
            sub.mIndices.reserve(indices.size());
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                if (indices[i] < vert_count && indices[i + 1] < vert_count && indices[i + 2] < vert_count)
                {
                    sub.mIndices.push_back(indices[i]);
                    sub.mIndices.push_back(indices[i + 1]);
                    sub.mIndices.push_back(indices[i + 2]);
                }
            }
            if (sub.mIndices.empty())
            {
                continue;
            }

            const S32 mat_index = prim.material;
            sub.mMaterialName = (mat_index >= 0 && (size_t)mat_index < model.materials.size()
                                 && !model.materials[mat_index].name.empty())
                ? model.materials[mat_index].name
                : (node.mesh >= 0 && !model.meshes[node.mesh].name.empty()
                   ? model.meshes[node.mesh].name : "material");

            if (mat_index >= 0 && (size_t)mat_index < model.materials.size())
            {
                const tinygltf::Material& mat = model.materials[mat_index];
                // Source: llmeshrepository.cpp:2974 — the material's own colour rides in the
                // TextureEntry alongside the image.
                const std::vector<double>& bcf = mat.pbrMetallicRoughness.baseColorFactor;
                if (bcf.size() >= 3)
                {
                    sub.mDiffuseColor.set((F32)bcf[0], (F32)bcf[1], (F32)bcf[2], 1.f);
                }
                // Name what the material DOES carry, so "no diffuse texture" can say why.
                if (mat.normalTexture.index >= 0)                              noteMaterialSlot(sub, "normal");
                if (mat.emissiveTexture.index >= 0)                            noteMaterialSlot(sub, "emissive");
                if (mat.occlusionTexture.index >= 0)                           noteMaterialSlot(sub, "occlusion");
                if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) noteMaterialSlot(sub, "metallic-roughness");

                const S32 image = baseColorImage(mat_index);
                if (image >= 0)
                {
                    auto bytes = image_bytes.find(image);
                    if (bytes != image_bytes.end() && !bytes->second.empty())
                    {
                        const std::string image_ext = sniffImageExtension(bytes->second);
                        if (image_ext.empty())
                        {
                            // A format the viewer's texture uploader has no codec for — KTX2 and
                            // Basis are the usual ones. Say so instead of uploading nothing.
                            const std::string& uri = model.images[image].uri;
                            mTextureErrors.push_back(uri.empty() ? llformat("image %d", image) : uri);
                        }
                        else
                        {
                            sub.mTextureBytes = bytes->second;
                            sub.mTextureExtension = image_ext;
                            sub.mTextureKey = llformat("gltfimg:%d", image);
                        }
                    }
                    else if ((size_t)image < model.images.size() && !model.images[image].uri.empty())
                    {
                        mTextureErrors.push_back(model.images[image].uri);
                    }
                }
            }
            dropPartialArrays(sub);
            mSubmeshes.push_back(std::move(sub));
        }
    }

    if (skipped_sparse)
    {
        LL_WARNS("WolfMeshUpload") << skipped_sparse
            << " primitive(s) skipped: sparse or unreadable accessors" << LL_ENDL;
    }
    if (mSubmeshes.empty())
    {
        error = "No triangles found in that glTF scene.";
        return false;
    }
    return true;
}

// ──────────────────────────── load / finalise ────────────────────────────

bool Model::load(const std::string& path, std::string& error)
{
    mSubmeshes.clear();
    mPrims.clear();
    mTextureErrors.clear();
    mFinalised = false;

    std::string ext = gDirUtilp->getExtension(path);
    if (ext == "obj")
    {
        return loadObj(path, error);
    }
    if (ext == "gltf" || ext == "glb")
    {
        return loadGltf(path, error);
    }
    error = "WolfViewer's model uploader reads OBJ, glTF and GLB. Use \"Old Upload\" for COLLADA (.dae).";
    return false;
}

namespace
{
    /**
     * Split any material whose vertex count exceeds the mesh format's 16-bit index limit into
     * several faces, re-indexed so each one stands alone.
     *
     * Source: floater_mesh_upload.js:606-676 _splitOversized.
     *
     * WHY THE LIMIT EXISTS. A submesh's TriangleList is packed as u16 indices
     * (rust_proxy/src/main.rs:1756-1758 encode_submesh), so a submesh can address at most 65536
     * vertices. lldaeloader.cpp is where Firestorm hits the same wall and simply refuses the
     * model.
     *
     * WHY WE SPLIT INSTEAD. The limit is per SUBMESH, not per model, and a mesh prim carries up
     * to MAX_MODEL_FACES = 8 of them (llmodel.h:43). A 300k-vertex material is therefore
     * perfectly representable as five faces, and the asset stays exactly the format every viewer
     * already reads. Each chunk inherits the material's texture and colour, so the seams are
     * invisible.
     */
    std::vector<WolfMeshUpload::Submesh> splitOversized(std::vector<WolfMeshUpload::Submesh>& subs)
    {
        std::vector<WolfMeshUpload::Submesh> out;
        for (WolfMeshUpload::Submesh& s : subs)
        {
            if (s.mPositions.size() <= WolfMeshUpload::MAX_VERTS)
            {
                out.push_back(std::move(s));
                continue;
            }

            const bool has_normals = (s.mNormals.size() == s.mPositions.size());
            const bool has_uvs = (s.mTexCoords.size() == s.mPositions.size());

            std::vector<WolfMeshUpload::Submesh> chunks;
            std::map<U32, U32> remap;

            auto startChunk = [&]() -> WolfMeshUpload::Submesh&
            {
                remap.clear();
                WolfMeshUpload::Submesh c;
                c.mMaterialName     = s.mMaterialName;
                c.mTextureKey       = s.mTextureKey;
                c.mTexturePath      = s.mTexturePath;
                c.mTextureBytes     = s.mTextureBytes;
                c.mTextureExtension = s.mTextureExtension;
                c.mDiffuseColor     = s.mDiffuseColor;
                c.mMaterialSlots    = s.mMaterialSlots;
                chunks.push_back(std::move(c));
                return chunks.back();
            };

            WolfMeshUpload::Submesh* chunk = &startChunk();
            for (size_t t = 0; t + 2 < s.mIndices.size(); t += 3)
            {
                const U32 tri[3] = { s.mIndices[t], s.mIndices[t + 1], s.mIndices[t + 2] };
                // At most 3 new vertices per triangle, checked BEFORE inserting any of them so a
                // triangle is never split across two chunks.
                U32 fresh = 0;
                for (U32 vi : tri)
                {
                    if (remap.find(vi) == remap.end())
                    {
                        ++fresh;
                    }
                }
                if (chunk->mPositions.size() + fresh > WolfMeshUpload::MAX_VERTS)
                {
                    chunk = &startChunk();
                }
                for (U32 vi : tri)
                {
                    auto it = remap.find(vi);
                    if (it == remap.end())
                    {
                        const U32 ni = (U32)chunk->mPositions.size();
                        remap[vi] = ni;
                        chunk->mPositions.push_back(s.mPositions[vi]);
                        if (has_normals)
                        {
                            chunk->mNormals.push_back(s.mNormals[vi]);
                        }
                        if (has_uvs)
                        {
                            chunk->mTexCoords.push_back(s.mTexCoords[vi]);
                        }
                        chunk->mIndices.push_back(ni);
                    }
                    else
                    {
                        chunk->mIndices.push_back(it->second);
                    }
                }
            }

            U32 kept = 0;
            for (WolfMeshUpload::Submesh& c : chunks)
            {
                if (!c.mIndices.empty())
                {
                    out.push_back(std::move(c));
                    ++kept;
                }
            }
            LL_INFOS("WolfMeshUpload") << "split \"" << s.mMaterialName << "\" ("
                << s.mPositions.size() << " verts) into " << kept << " faces" << LL_ENDL;
        }
        return out;
    }

    struct BBox
    {
        LLVector3 mCentre;
        LLVector3 mSize;
    };

    BBox bboxOf(const std::vector<WolfMeshUpload::Submesh>& list, size_t begin, size_t end)
    {
        LLVector3 lo(F32_MAX, F32_MAX, F32_MAX);
        LLVector3 hi(-F32_MAX, -F32_MAX, -F32_MAX);
        for (size_t i = begin; i < end; ++i)
        {
            for (const LLVector3& p : list[i].mPositions)
            {
                for (S32 a = 0; a < 3; ++a)
                {
                    lo.mV[a] = llmin(lo.mV[a], p.mV[a]);
                    hi.mV[a] = llmax(hi.mV[a], p.mV[a]);
                }
            }
        }
        BBox b;
        b.mCentre = (lo + hi) * 0.5f;
        for (S32 a = 0; a < 3; ++a)
        {
            // A flat piece (a plane, a decal) has zero extent on one axis. 1e-5 keeps the divide
            // finite; the prim's own scale clamp is what the sim sees.
            // Source: floater_mesh_upload.js:748-751.
            b.mSize.mV[a] = llmax(hi.mV[a] - lo.mV[a], 1e-5f);
        }
        return b;
    }
}

/**
 * Enforce the limits, normalise every vertex into the unit box the proxy's encoder expects, and
 * group the faces into prims.
 *
 * Source: floater_mesh_upload.js:682-817 _finalise.
 *
 * MORE THAN 8 MATERIALS BECOMES A LINKSET, NOT AN ERROR. A mesh PRIM carries at most
 * MAX_MODEL_FACES = 8 submeshes (llmodel.h:43), and that is a property of one prim, not of one
 * object: a linkset of three mesh prims shows 24 materials perfectly well. Firestorm's uploader
 * refuses instead, because SL's upload service builds one prim per upload; the proxy builds the
 * SceneObjectGroup itself (main.rs:1920-1931 build_sog_xml), so it can emit root + children and
 * hand back one inventory item.
 *
 * Each prim is normalised to ITS OWN bounding box, because the .llmesh position domain is the
 * unit box and the prim's Scale is what maps it back to metres. That in turn means each prim
 * needs an OffsetPosition — its own bbox centre relative to the ROOT prim's — or the pieces
 * would all stack up concentrically.
 */
bool Model::finalise(std::string& error)
{
    if (mFinalised)
    {
        return true;
    }
    mSubmeshes = splitOversized(mSubmeshes);

    if (mSubmeshes.empty())
    {
        error = "That model has no triangles to upload.";
        return false;
    }

    const size_t max_faces = (size_t)MAX_SUBMESHES * MAX_PRIMS;
    if (mSubmeshes.size() > max_faces)
    {
        error = llformat("Model has %u materials, which would need %u linked prims (max %u). "
                         "Merge materials and retry.",
                         (U32)mSubmeshes.size(),
                         (U32)((mSubmeshes.size() + MAX_SUBMESHES - 1) / MAX_SUBMESHES),
                         (U32)MAX_PRIMS);
        return false;
    }

    // NON-FINITE GEOMETRY IS REJECTED HERE, NOT PASSED ON.
    //
    // A NaN or Infinity in a position poisons everything downstream silently: the bbox becomes
    // NaN, so every normalised position becomes NaN, so every quantised u16 becomes 0 (the
    // proxy's round-and-clamp maps NaN to 0, main.rs:1740-1745), and the object's <Scale> is
    // written into the asset XML as "NaN" — which is exactly the shape of an upload that reports
    // success, appears in inventory and then rezzes as nothing.
    // Source: floater_mesh_upload.js:707-722.
    for (Submesh& s : mSubmeshes)
    {
        if (s.mPositions.size() > MAX_VERTS)
        {
            error = llformat("A face still has %u verts (>%u) after splitting. Decimate and retry.",
                             (U32)s.mPositions.size(), (U32)MAX_VERTS);
            return false;
        }
        for (const LLVector3& p : s.mPositions)
        {
            if (!isFinite3(p))
            {
                error = "Model contains invalid (NaN/Infinity) vertex positions — the export is "
                        "corrupt. Re-export and retry.";
                return false;
            }
        }
        // A single bad normal or UV loses the whole array rather than the whole model: the
        // .llmesh carries the array or omits it, and omitting it is well defined.
        for (const LLVector3& n : s.mNormals)
        {
            if (!isFinite3(n))
            {
                s.mNormals.clear();
                break;
            }
        }
        for (const LLVector2& t : s.mTexCoords)
        {
            if (!std::isfinite(t.mV[VX]) || !std::isfinite(t.mV[VY]))
            {
                s.mTexCoords.clear();
                break;
            }
        }
    }

    const BBox whole = bboxOf(mSubmeshes, 0, mSubmeshes.size());
    const BBox root  = bboxOf(mSubmeshes, 0, llmin((size_t)MAX_SUBMESHES, mSubmeshes.size()));

    for (size_t start = 0; start < mSubmeshes.size(); start += MAX_SUBMESHES)
    {
        const size_t end = llmin(start + MAX_SUBMESHES, mSubmeshes.size());
        const BBox box = bboxOf(mSubmeshes, start, end);

        Prim prim;
        for (S32 a = 0; a < 3; ++a)
        {
            prim.mScale.mV[a] = clampPrimScale(box.mSize.mV[a]);
        }
        // Root is the frame, so its offset is zero by definition; the proxy forces this too
        // (main.rs:2044-2048). Children carry the delta between bbox centres.
        prim.mOffset = (start == 0) ? LLVector3::zero : (box.mCentre - root.mCentre);

        for (size_t i = start; i < end; ++i)
        {
            Submesh face = std::move(mSubmeshes[i]);
            for (LLVector3& p : face.mPositions)
            {
                for (S32 a = 0; a < 3; ++a)
                {
                    p.mV[a] = (p.mV[a] - box.mCentre.mV[a]) / box.mSize.mV[a];   // → [-0.5, 0.5]
                }
            }
            prim.mFaces.push_back(std::move(face));
        }
        mPrims.push_back(std::move(prim));
    }
    mSubmeshes.clear();

    for (S32 a = 0; a < 3; ++a)
    {
        mScale.mV[a] = clampPrimScale(whole.mSize.mV[a]);
    }

    if (mPrims.size() > 1)
    {
        LL_INFOS("WolfMeshUpload") << mPrims.size() << " prims in the linkset" << LL_ENDL;
    }
    mFinalised = true;
    return true;
}

// ──────────────────────────────── reporting ────────────────────────────────

U32 Model::distinctTextureCount() const
{
    std::set<std::string> keys;
    for (const Prim& p : mPrims)
    {
        for (const Submesh& f : p.mFaces)
        {
            if (!f.mTextureKey.empty())
            {
                keys.insert(f.mTextureKey);
            }
        }
    }
    return (U32)keys.size();
}

bool Model::hasTextures() const
{
    return distinctTextureCount() > 0;
}

std::string Model::summary(bool include_textures) const
{
    size_t faces = 0, verts = 0, tris = 0, textured = 0;
    for (const Prim& p : mPrims)
    {
        faces += p.mFaces.size();
        for (const Submesh& f : p.mFaces)
        {
            verts += f.mPositions.size();
            tris  += f.mIndices.size() / 3;
            if (!f.mTextureKey.empty())
            {
                ++textured;
            }
        }
    }

    std::ostringstream o;
    o << faces << " face(s), " << verts << " verts, " << tris << " tris — "
      << llformat("%.2f x %.2f x %.2f m", mScale.mV[VX], mScale.mV[VY], mScale.mV[VZ]);
    if (mPrims.size() > 1)
    {
        o << " — rezzes as a linkset of " << mPrims.size() << " prims";
    }
    o << "\n";

    const U32 distinct = distinctTextureCount();
    if (textured)
    {
        if (include_textures)
        {
            o << distinct << " texture(s) will be uploaded and applied to " << textured << " face(s).";
        }
        else
        {
            o << distinct << " texture(s) found — tick \"Include textures\" to upload them.";
        }
    }
    else if (!mTextureErrors.empty())
    {
        // Say WHY rather than "not found": the parse succeeded, the materials are there, and the
        // images are what failed. Source: floater_mesh_upload.js:219-224.
        o << mTextureErrors.size() << " texture(s) in this model could not be used, so there is "
             "nothing to upload. WolfViewer reads PNG, JPEG, BMP and TGA — re-export the textures "
             "in one of those (not KTX2/Basis or DDS).";
    }
    else
    {
        // Name what the materials DO carry. "No textures" is ambiguous between "the model has
        // none", "they are in a slot SL has no equivalent for" and "they failed to load".
        std::set<std::string> slots;
        for (const Prim& p : mPrims)
        {
            for (const Submesh& f : p.mFaces)
            {
                slots.insert(f.mMaterialSlots.begin(), f.mMaterialSlots.end());
            }
        }
        if (slots.empty())
        {
            o << "No textures found in this model.";
        }
        else
        {
            o << "No diffuse (base colour) texture found. The materials only carry: ";
            bool first = true;
            for (const std::string& s : slots)
            {
                o << (first ? "" : ", ") << s;
                first = false;
            }
            o << ". SL textures a face from the base colour map.";
        }
    }
    return o.str();
}

// ──────────────────────────────── uploading ────────────────────────────────

namespace
{
    /**
     * A texture upload that tells a waiting coroutine what asset id it produced.
     *
     * LLViewerAssetUpload::AssetInventoryUploadCoproc calls exactly one of finishUpload (success,
     * llviewerassetupload.cpp:986) or failedUpload (every error path, via HandleUploadError at
     * :1097) — so the promise is settled exactly once, and if the coprocedure is dropped without
     * running, destroying the promise breaks the future and the waiter wakes with an exception
     * rather than hanging.
     */
    class WolfTextureUpload : public LLNewFileResourceUploadInfo
    {
    public:
        typedef std::shared_ptr<LLCoros::Promise<LLUUID>> promise_ptr_t;

        /**
         * @param owned_temp_path a temp file written for this upload, deleted when this object
         *        is destroyed. The COROUTINE must not delete it: on a timeout the coroutine
         *        gives up while this upload is still queued, and prepareUpload has not
         *        necessarily read the file yet — deleting it there broke a live upload the user
         *        had already been charged for. Ownership belongs where the lifetime is.
         */
        WolfTextureUpload(const std::string& file_name, const std::string& name,
                          S32 expected_cost, const promise_ptr_t& promise,
                          const std::string& owned_temp_path)
            : LLNewFileResourceUploadInfo(
                  file_name, name, name, 0,
                  LLFolderType::FT_NONE, LLInventoryType::IT_NONE,
                  LLFloaterPerms::getNextOwnerPerms("Uploads"),
                  LLFloaterPerms::getGroupPerms("Uploads"),
                  LLFloaterPerms::getEveryonePerms("Uploads"),
                  expected_cost,
                  LLUUID::null,
                  false /* the model uploader shows its own progress */)
            , mPromise(promise)
            , mOwnedTempPath(owned_temp_path)
        {
        }

        ~WolfTextureUpload() override
        {
            if (!mOwnedTempPath.empty())
            {
                LLFile::remove(mOwnedTempPath);
            }
        }

        LLUUID finishUpload(LLSD& result) override
        {
            const LLUUID item = LLNewFileResourceUploadInfo::finishUpload(result);
            settle(result["new_asset"].asUUID());
            return item;
        }

        bool failedUpload(LLSD& /*result*/, std::string& /*reason*/) override
        {
            settle(LLUUID::null);
            // HandleUploadError has already put a notification up (llviewerassetupload.cpp:1095);
            // returning true says the failure is handled and stops the snapshot-floater
            // bookkeeping that follows it from running for a texture we did not come from there.
            return true;
        }

    private:
        void settle(const LLUUID& id)
        {
            if (mSettled)
            {
                return;
            }
            mSettled = true;
            mPromise->set_value(id);
        }

        promise_ptr_t mPromise;
        std::string   mOwnedTempPath;
        bool          mSettled = false;
    };

    /**
     * Build one prim's TextureEntry.
     *
     * Per-face fields are the same subset Firestorm's uploader writes
     * (llmeshrepository.cpp:2965-2975): image, scales/scalet = 1, offsets/offsett = 0,
     * imagerot = 0, diffuse_color. Packing is the viewer's own LLPrimitive::packTEMessage
     * (llprimitive.cpp:1301-1381) rather than a reimplementation of packTEField's variable-length
     * exception-run encoding, so the blob is byte-for-byte what every other TextureEntry on the
     * wire is — which is what OpenSim's Primitive.TextureEntry(data, 0, len) reads back
     * (SceneObjectSerializer.cs:1247-1250).
     *
     * The face with no texture of its own gets plywood, not one of the uploaded textures:
     * DEFAULT_OBJECT_TEXTURE is what an untextured face is meant to show
     * (indra_constants.cpp:75), and anything else would paint it with another face's image.
     *
     * @return base64 of the packed blob, or an empty string when the prim needs no TextureEntry.
     */
    std::string buildTextureEntry(const WolfMeshUpload::Prim& prim,
                                  const std::map<std::string, LLUUID>& uuid_by_key)
    {
        const U8 num_faces = (U8)prim.mFaces.size();
        if (num_faces == 0)
        {
            return std::string();
        }

        LLPrimitive te_prim;
        te_prim.setNumTEs(num_faces);

        bool any = false;
        for (U8 i = 0; i < num_faces; ++i)
        {
            const WolfMeshUpload::Submesh& f = prim.mFaces[i];

            LLUUID image = DEFAULT_OBJECT_TEXTURE;
            if (!f.mTextureKey.empty())
            {
                auto it = uuid_by_key.find(f.mTextureKey);
                if (it != uuid_by_key.end() && it->second.notNull())
                {
                    image = it->second;
                    any = true;
                }
            }
            te_prim.setTETexture(i, image);
            te_prim.setTEScale(i, 1.f, 1.f);
            te_prim.setTEOffset(i, 0.f, 0.f);
            te_prim.setTERotation(i, 0.f);

            LLColor4 colour = f.mDiffuseColor;
            colour.mV[VW] = 1.f;
            if (colour.mV[VX] < 0.999f || colour.mV[VY] < 0.999f || colour.mV[VZ] < 0.999f)
            {
                any = true;
            }
            te_prim.setTEColor(i, colour);
        }

        if (!any)
        {
            // Every face is plain plywood at full white, which is exactly what the object asset
            // gets with no <TextureEntry> at all. Send nothing rather than a no-op blob.
            return std::string();
        }

        // MAX_TE_BUFFER in packTEMessage is 4096 for the blob itself (llprimitive.cpp:1317);
        // packBinaryData prefixes a 4-byte length (lldatapacker.cpp:271-289), so allow for both.
        U8 buffer[4096 + 4];
        LLDataPackerBinaryBuffer dp(buffer, sizeof(buffer));
        if (!te_prim.packTEMessage(dp))
        {
            LL_WARNS("WolfMeshUpload") << "TextureEntry did not fit in the pack buffer for "
                << (S32)num_faces << " faces" << LL_ENDL;
            return std::string();
        }
        const S32 written = dp.getCurrentSize();
        if (written <= 4)
        {
            return std::string();
        }
        // Strip the 4-byte length packBinaryData wrote: OpenSim reads the raw blob.
        return LLBase64::encode(buffer + 4, (size_t)(written - 4));
    }

    /** The request body of POST /upload_mesh.
     *  Source: rust_proxy/src/main.rs:1640-1694 — MeshSubmeshIn, MeshPrimIn, UploadMeshRequest.
     *  Field names are the Rust field names verbatim; unknown fields are ignored by serde, and
     *  fields with #[serde(default)] must be omitted rather than sent as null. */
    std::string buildRequestBody(const WolfMeshUpload::Model& model,
                                 const WolfMeshUpload::Options& options,
                                 const LLUUID& folder_id,
                                 const std::map<std::string, LLUUID>& uuid_by_key)
    {
        std::ostringstream o;
        o << '{';
        o << "\"name\":" << jsonString(options.mName);
        o << ",\"description\":" << jsonString(options.mDescription);
        o << ",\"agent_id\":" << jsonString(gAgentID.asString());
        o << ",\"folder_id\":" << jsonString(folder_id.asString());
        o << ",\"scale\":";
        appendVec3(o, model.overallScale());
        o << ",\"prims\":[";

        bool first_prim = true;
        for (const WolfMeshUpload::Prim& prim : model.prims())
        {
            o << (first_prim ? "" : ",") << '{';
            first_prim = false;
            o << "\"scale\":";
            appendVec3(o, prim.mScale);
            o << ",\"offset\":";
            appendVec3(o, prim.mOffset);
            o << ",\"submeshes\":[";
            bool first_sub = true;
            for (const WolfMeshUpload::Submesh& f : prim.mFaces)
            {
                o << (first_sub ? "" : ",") << '{';
                first_sub = false;

                o << "\"positions\":[";
                for (size_t i = 0; i < f.mPositions.size(); ++i)
                {
                    if (i) o << ',';
                    appendVec3(o, f.mPositions[i]);
                }
                o << ']';

                o << ",\"triangles\":[";
                for (size_t i = 0; i + 2 < f.mIndices.size(); i += 3)
                {
                    if (i) o << ',';
                    o << '[' << f.mIndices[i] << ',' << f.mIndices[i + 1] << ',' << f.mIndices[i + 2] << ']';
                }
                o << ']';

                if (f.mNormals.size() == f.mPositions.size() && !f.mNormals.empty())
                {
                    o << ",\"normals\":[";
                    for (size_t i = 0; i < f.mNormals.size(); ++i)
                    {
                        if (i) o << ',';
                        appendVec3(o, f.mNormals[i]);
                    }
                    o << ']';
                }
                if (f.mTexCoords.size() == f.mPositions.size() && !f.mTexCoords.empty())
                {
                    o << ",\"uvs\":[";
                    for (size_t i = 0; i < f.mTexCoords.size(); ++i)
                    {
                        if (i) o << ',';
                        o << '[';
                        appendF32(o, f.mTexCoords[i].mV[VX]);
                        o << ',';
                        appendF32(o, f.mTexCoords[i].mV[VY]);
                        o << ']';
                    }
                    o << ']';
                }
                o << '}';
            }
            o << ']';

            if (options.mIncludeTextures)
            {
                const std::string te = buildTextureEntry(prim, uuid_by_key);
                if (!te.empty())
                {
                    o << ",\"texture_entry\":" << jsonString(te);
                }
            }
            o << '}';
        }
        o << "]}";
        return o.str();
    }

    /** Inside a coroutine: POST a JSON body, return the HTTP status and the raw reply.
     *  Source: wolfspeech.cpp:52-86 postRaw — the established WolfViewer proxy POST. */
    S32 postJson(const std::string& url, const std::string& body, LLSD::Binary& reply, std::string& error)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfMeshUpload", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t opts = std::make_shared<LLCore::HttpOptions>();
        // The proxy zlib-compresses three blocks per prim and writes two assets plus an
        // inventory item through ROBUST; its own outbound client allows 60 s (main.rs:238-246).
        opts->setTimeout(180);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
        // /upload_mesh does not check these today (only /stt and /tts call speech_authorise,
        // main.rs:3768, :3830), but every Wolf proxy call sends the pair so the endpoint can be
        // gated later without a viewer release.
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());

        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(body.data(), body.size());

        LLSD result = adapter->postRawAndSuspend(request, url, raw, opts, headers);
        LLSD http_results = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);
        reply.clear();
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            reply = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        }
        error = status ? std::string() : status.toString();
        return status.getType();
    }

    /** Parse the proxy's reply. Source: main.rs:3910-3917 — success and failure are BOTH
     *  HTTP 200; `success` is the field that decides. */
    bool parseReply(const LLSD::Binary& bytes, LLUUID& item_id, std::string& error)
    {
        const std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec || !v.is_object())
        {
            error = "The mesh service sent a reply that was not JSON.";
            return false;
        }
        LLSD sd = LlsdFromJson(v);
        if (!sd["success"].asBoolean())
        {
            error = sd.has("error") ? sd["error"].asString() : std::string("upload failed");
            return false;
        }
        item_id = sd["item_id"].asUUID();
        return true;
    }

    /** Write a texture that lives inside the model file to a temp file the uploader can read. */
    bool writeTempTexture(const WolfMeshUpload::Submesh& face, std::string& path_out)
    {
        const std::string ext = face.mTextureExtension.empty() ? std::string("png") : face.mTextureExtension;
        path_out = gDirUtilp->getTempFilename() + "." + ext;
        LLFILE* fp = LLFile::fopen(path_out, "wb");
        if (!fp)
        {
            return false;
        }
        const size_t written = fwrite(face.mTextureBytes.data(), 1, face.mTextureBytes.size(), fp);
        LLFile::close(fp);
        if (written != face.mTextureBytes.size())
        {
            LLFile::remove(path_out);
            return false;
        }
        return true;
    }

    void uploadCoro(WolfMeshUpload::model_ptr_t model, WolfMeshUpload::Options options,
                    WolfMeshUpload::progress_fn progress, WolfMeshUpload::done_fn done)
    {
        // NOTE: temp files written for embedded textures are owned by the WolfTextureUpload that
        // reads them and are deleted in its destructor — see the note on its constructor. This
        // coroutine must not delete them, because it can give up (timeout) while an upload is
        // still queued and holding the path.

        // The Objects folder the item is written into. The proxy writes it straight through
        // ROBUST XInventory (main.rs:1955-1990 robust_add_item), so this must be a folder that
        // really belongs to this avatar.
        // Source: llmeshrepository.cpp:2989 uses the same lookup for a model upload.
        const LLUUID folder_id = options.mFolderId.notNull()
            ? options.mFolderId
            : gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_OBJECT);
        if (folder_id.isNull())
        {
            done(false, "Your Objects inventory folder has not loaded yet. Open Inventory and try again.", LLUUID::null);
            return;
        }

        std::map<std::string, LLUUID> uuid_by_key;
        if (options.mIncludeTextures)
        {
            const std::string cap = gAgent.getRegionCapability(TEXTURE_UPLOAD_CAP);
            if (cap.empty())
            {
                done(false, "This region cannot take texture uploads right now. Untick \"Include textures\" "
                            "to upload the model without them.", LLUUID::null);
                return;
            }

            // Each DISTINCT texture is uploaded once for the whole model — keyed on the texture,
            // not the face or the prim. Source: llmeshrepository.cpp:2952-2963, and
            // floater_mesh_upload.js:827-853 _uploadTextures.
            std::vector<const WolfMeshUpload::Submesh*> distinct;
            std::set<std::string> seen;
            for (const WolfMeshUpload::Prim& prim : model->prims())
            {
                for (const WolfMeshUpload::Submesh& f : prim.mFaces)
                {
                    if (!f.mTextureKey.empty() && seen.insert(f.mTextureKey).second)
                    {
                        distinct.push_back(&f);
                    }
                }
            }

            U32 n = 0;
            for (const WolfMeshUpload::Submesh* face : distinct)
            {
                ++n;
                progress(llformat("Uploading texture %u/%u…", n, (U32)distinct.size()), false);

                // An OBJ's map_Kd and a .gltf's external image are already files on disk and are
                // NOT ours to delete; a texture embedded in a GLB has to be written out first,
                // and that one is handed to the upload object to own.
                std::string file_path = face->mTexturePath;
                std::string owned_temp;
                if (file_path.empty())
                {
                    if (!writeTempTexture(*face, file_path))
                    {
                        done(false, "Could not write a temporary file for one of the model's textures.", LLUUID::null);
                        return;
                    }
                    owned_temp = file_path;
                }

                std::string tex_name = options.mName + " - " + face->mMaterialName;
                if (tex_name.size() > 63)
                {
                    tex_name.resize(63);
                }

                auto promise = std::make_shared<LLCoros::Promise<LLUUID>>();
                LLCoros::Future<LLUUID> future = LLCoros::getFuture(*promise);
                LLResourceUploadInfo::ptr_t info = std::make_shared<WolfTextureUpload>(
                    file_path, tex_name,
                    LLAgentBenefitsMgr::current().getTextureUploadCost(),
                    promise, owned_temp);
                LLViewerAssetUpload::EnqueueInventoryUpload(cap, info);

                LLUUID asset_id;
                try
                {
                    if (future.wait_for(std::chrono::seconds(300)) != boost::fibers::future_status::ready)
                    {
                        std::string msg = "A texture upload did not finish within five minutes, "
                                          "so the model was not uploaded.";
                        if (n > 1)
                        {
                            msg += llformat(" The %u texture(s) that uploaded before it are in your "
                                            "Textures folder and have been charged for.", n - 1);
                        }
                        done(false, msg, LLUUID::null);
                        return;
                    }
                    asset_id = future.get();
                }
                catch (const std::exception& e)
                {
                    LL_WARNS("WolfMeshUpload") << "texture upload abandoned: " << e.what() << LL_ENDL;
                    asset_id.setNull();
                }

                if (asset_id.isNull())
                {
                    // The texture upload has already said why. Stop here rather than build a model
                    // whose faces reference an asset that does not exist.
                    //
                    // Textures are uploaded one at a time through the ordinary cap, so any that
                    // already succeeded are in inventory AND have been charged for. There is no
                    // way to un-upload them, so say so plainly instead of letting the user
                    // discover it on their balance.
                    std::string msg = "A texture failed to upload, so the model was not uploaded either.";
                    if (n > 1)
                    {
                        msg += llformat(" The %u texture(s) that uploaded before it are in your "
                                        "Textures folder and have been charged for.", n - 1);
                    }
                    done(false, msg, LLUUID::null);
                    return;
                }
                uuid_by_key[face->mTextureKey] = asset_id;
            }
        }

        progress("Uploading model…", false);
        const std::string body = buildRequestBody(*model, options, folder_id, uuid_by_key);
        LL_INFOS("WolfMeshUpload") << "POST /upload_mesh, " << body.size() << " bytes, "
            << model->prims().size() << " prim(s), " << uuid_by_key.size() << " texture(s)" << LL_ENDL;

        LLSD::Binary reply;
        std::string transport_error;
        const S32 status = postJson(uploadMeshUrl(), body, reply, transport_error);

        // The proxy answers HTTP 200 for a rejected upload too and puts the reason in the body
        // (main.rs:3915), so the body is read first whatever the status was; only a transport
        // failure with no usable body falls back to describing the status code.
        LLUUID item_id;
        std::string error;
        const bool body_says_ok = parseReply(reply, item_id, error);
        if (!body_says_ok)
        {
            // A transport failure has no useful body to explain itself, so name the status
            // instead of reporting "reply was not JSON".
            if (status < 200 || status >= 300)
            {
                error = llformat("the mesh service returned HTTP %d", status);
                if (!transport_error.empty())
                {
                    error += " (" + transport_error + ")";
                }
            }
            done(false, "Upload failed: " + error, LLUUID::null);
            return;
        }
        if (status < 200 || status >= 300)
        {
            // A success body under a failure status should not happen; trust the status.
            done(false, llformat("Upload failed: the mesh service returned HTTP %d.", status), LLUUID::null);
            return;
        }

        // The item is created directly through ROBUST XInventory, so the sim never sends a
        // BulkUpdateInventory and the viewer's copy of the folder is stale. Force a refetch the
        // way the mesh repository does after it creates a folder
        // (llmeshrepository.cpp:4657 setVersion(VERSION_UNKNOWN)).
        LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
        if (cat)
        {
            cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
        }
        gInventory.fetchDescendentsOf(folder_id);
        gInventory.notifyObservers();

        done(true, "Uploaded.", item_id);
    }
}

void upload(const model_ptr_t& model, const Options& options, progress_fn progress, done_fn done)
{
    if (!model || model->prims().empty())
    {
        done(false, "There is no model to upload.", LLUUID::null);
        return;
    }
    LLCoros::instance().launch("WolfMeshUpload",
        [model, options, progress, done]() { uploadCoro(model, options, progress, done); });
}

} // namespace WolfMeshUpload
