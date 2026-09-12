/**
 * @file wolfmeshupload.h
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

#ifndef WOLF_MESH_UPLOAD_H
#define WOLF_MESH_UPLOAD_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "stdtypes.h"
#include "lluuid.h"
#include "v2math.h"
#include "v3math.h"
#include "v4color.h"

// Source: wolfstorm/js/ui/floaters/floater_mesh_upload.js — the same uploader, same numbers,
// same wire body, same grid-side endpoint. WolfStorm uploads a model differently from
// Firestorm: no LOD sliders, no physics analysis, no streaming-cost negotiation and no
// NewFileAgentInventory round trip for the mesh itself. The client parses the file into
// per-material submeshes (source Y-up -> SL Z-up, each prim normalised into a unit box) and the
// Wolf Territories Rust proxy's POST /upload_mesh builds the .llmesh asset, wraps it in a
// SceneObjectGroup and writes both plus an inventory item through the grid's ROBUST services
// (rust_proxy/src/main.rs:1993-2067 handle_upload_mesh).
//
// TEXTURES are the one part the proxy does NOT do: it never talks to the sim, so it cannot
// charge for or create texture assets. Each distinct texture is uploaded here through the
// region's ordinary NewFileAgentInventory capability — the same path and the same cost as any
// other texture upload — and the resulting asset ids are packed into a TextureEntry that rides
// along in the request (main.rs:1667-1671 MeshPrimIn::texture_entry, base64).
//
// FORMATS: OBJ, glTF and GLB, which is exactly WolfStorm's set. COLLADA (.dae) is not handled
// here; it belongs to Firestorm's own uploader, which the floater reaches with its "Old Upload"
// button. This is deliberate — Firestorm's DAE path carries rigging, LOD generation and physics
// decomposition that this uploader has no equivalent for.
namespace WolfMeshUpload
{
    /** Source: llmodel.h:43 `#define MAX_MODEL_FACES 8` — submeshes per mesh PRIM, not per model.
     *  Materials past this become extra linked prims (see Model::finalise). */
    constexpr U32 MAX_SUBMESHES = 8;
    /** Source: rust_proxy/src/main.rs:1749 encode_submesh — the .llmesh TriangleList is u16-indexed. */
    constexpr U32 MAX_VERTS = 65535;
    /** Source: rust_proxy/src/main.rs:2011-2015 — the proxy's own bound on linkset size. */
    constexpr U32 MAX_PRIMS = 32;

    /** One material's geometry, in SL space (metres, Z-up), before normalisation. */
    struct Submesh
    {
        std::vector<LLVector3> mPositions;
        std::vector<LLVector3> mNormals;    // empty, or exactly mPositions.size()
        std::vector<LLVector2> mTexCoords;  // empty, or exactly mPositions.size()
        std::vector<U32>       mIndices;    // triples into mPositions

        std::string            mMaterialName;
        /** Identifies the IMAGE, not the material: several materials can share one image and
         *  Firestorm uploads each distinct texture exactly once
         *  (llmeshrepository.cpp:2952-2963 keys texture_index on the texture). Empty = no texture. */
        std::string            mTextureKey;
        /** Where the image bytes live. Exactly one of these is set when mTextureKey is not empty:
         *  a path already on disk (OBJ's map_Kd, a .gltf's external image), or bytes lifted out of
         *  a GLB's binary chunk, which are written to a temp file at upload time. */
        std::string            mTexturePath;
        std::vector<U8>        mTextureBytes;
        std::string            mTextureExtension;  // "png", "jpg", ... for the temp file
        /** Source: llmeshrepository.cpp:2974 face_entry["diffuse_color"] — the material's own
         *  colour rides in the TextureEntry alongside the image. */
        LLColor4               mDiffuseColor { 1.f, 1.f, 1.f, 1.f };
        /** Map slots this material actually populated, used only to explain a "no textures"
         *  result to the user (normal/roughness/emissive have no SL diffuse equivalent). */
        std::vector<std::string> mMaterialSlots;
    };

    /** One PRIM of the finished object: up to MAX_SUBMESHES faces normalised to its own bbox. */
    struct Prim
    {
        LLVector3             mScale;    // this prim's bbox size in metres, clamped
        LLVector3             mOffset;   // this prim's bbox centre minus the ROOT prim's
        std::vector<Submesh>  mFaces;    // positions rewritten into [-0.5, 0.5]
    };

    /** A parsed and finalised model: what the info panel describes and the uploader sends. */
    class Model
    {
    public:
        /** Parse a .obj, .gltf or .glb into per-material submeshes in SL space.
         *  Safe to call off the main thread: touches only the filesystem and tinygltf. */
        bool load(const std::string& path, std::string& error);

        /** Enforce the limits, split oversized materials, group into prims and normalise.
         *  Must be called after load() and before upload(). */
        bool finalise(std::string& error);

        /** One line for the info panel, in the shape WolfStorm's _renderInfo produces. */
        std::string summary(bool include_textures) const;
        /** True when the model carries at least one usable diffuse texture. */
        bool hasTextures() const;
        /** Number of distinct textures that would be uploaded. */
        U32 distinctTextureCount() const;

        const std::vector<Prim>& prims() const { return mPrims; }
        const LLVector3& overallScale() const { return mScale; }

    private:
        bool loadObj(const std::string& path, std::string& error);
        bool loadGltf(const std::string& path, std::string& error);

        std::vector<Submesh> mSubmeshes;   // cleared by finalise() once grouped into mPrims
        std::vector<Prim>    mPrims;
        LLVector3            mScale { 1.f, 1.f, 1.f };
        /** Images the parser found but could not use, so "no textures" can say why. */
        std::vector<std::string> mTextureErrors;
        bool                 mFinalised = false;
    };

    typedef std::shared_ptr<Model> model_ptr_t;

    struct Options
    {
        std::string mName;
        std::string mDescription;
        bool        mIncludeTextures = true;
        /** Where the object item is created. Null means the user's Objects folder, which is
         *  what the menu route wants; the inventory gallery's "Upload > Model" passes the
         *  folder it was opened from (llinventoryfunctions.cpp:4228) and that must be honoured. */
        LLUUID      mFolderId;
        // Source: wolfai.cpp meshCoro captures the initiating login; manual uploads retain defaults.
        bool        mSessionBound = false;
        LLUUID      mAgentId, mSessionId;
    };

    /** Progress line for the floater. Always called on the main thread. */
    typedef std::function<void(const std::string& status, bool is_error)> progress_fn;
    /** Final outcome. Always called on the main thread, exactly once. */
    typedef std::function<void(bool ok, const std::string& message, const LLUUID& item_id)> done_fn;

    /**
     * Upload a finalised model: textures first (one inventory upload per distinct image),
     * then one POST to the proxy that creates the mesh assets, the object and the inventory
     * item. Runs as a coroutine and returns immediately.
     */
    void upload(const model_ptr_t& model, const Options& options, progress_fn progress, done_fn done);

    /** Wolf Territories, logged in, with a region that can take texture uploads. */
    bool isAvailable();

    /** True for the extensions this uploader handles (lower or mixed case, with or without dot). */
    bool isSupportedExtension(const std::string& extension);
}

#endif // WOLF_MESH_UPLOAD_H
