/**
 * @file wolfcore_api.h
 * @brief Public C ABI between WolfViewer (LGPL v2.1) and the optional WolfCore runtime library.
 *
 * Copyright (C) 2026 IntelligentWolf Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this interface header file (`wolfcore_api.h`) to use, copy, modify, merge,
 * publish, and distribute it under the terms of the MIT License or the GNU Lesser
 * General Public License version 2.1, so that open-source builds of WolfViewer
 * compile without requiring the proprietary WolfCore implementation.
 */

#ifndef WOLFCORE_API_H
#define WOLFCORE_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WOLFCORE_ABI_VERSION 1u

#if defined(_WIN32)
#  if defined(WOLFCORE_BUILDING_DLL)
#    define WOLFCORE_API __declspec(dllexport)
#  else
#    define WOLFCORE_API
#  endif
#  define WOLFCORE_CALL __cdecl
#else
#  if defined(WOLFCORE_BUILDING_DLL)
#    define WOLFCORE_API __attribute__((visibility("default")))
#  else
#    define WOLFCORE_API
#  endif
#  define WOLFCORE_CALL
#endif

/** Uniform value kinds passed to pack_uniform_member. */
enum WolfCoreUniformKind
{
    WC_UNIFORM_F1   = 0,
    WC_UNIFORM_F2   = 1,
    WC_UNIFORM_F3   = 2,
    WC_UNIFORM_F4   = 3,
    WC_UNIFORM_I1   = 4,
    WC_UNIFORM_I2   = 5,
    WC_UNIFORM_I4   = 6,
    WC_UNIFORM_U4   = 7,
    WC_UNIFORM_M2   = 8,
    WC_UNIFORM_M3   = 9,
    WC_UNIFORM_M3X4 = 10,
    WC_UNIFORM_M4   = 11
};

/** Reflected std140/std430 layout of a single uniform block member. */
struct WolfCoreMemberLayout
{
    uint32_t columns;        /* 1 for scalars/vectors, 2..4 for matrices */
    uint32_t rows;           /* 1..4 */
    uint32_t array_size;     /* 1 when not an array */
    uint32_t offset;         /* byte offset from start of uniform block */
    uint32_t array_stride;   /* byte stride between array elements (0 if not array) */
    uint32_t matrix_stride;  /* byte stride between matrix columns/rows (0 if non-matrix) */
    uint8_t  row_major;      /* 1 if RowMajor decorated, 0 if ColMajor */
    uint8_t  reserved[3];
};

/** Per-frame telemetry inputs for WolfCore's adaptive frame-pacing scheduler. */
struct WolfCoreFrameInputs
{
    float    last_frame_dt_sec;      /* duration of the previous rendered frame in seconds */
    uint32_t probe_frame_counter;    /* monotonic reflection-probe tick counter */
    uint32_t probe_interval_setting; /* user/configured interval (e.g. 3) */
    uint8_t  atmos_tables_ready;     /* 1 once initial sky LUT set is complete, 0 during login build */
    uint8_t  is_native_vulkan;       /* 1 when running on native Vulkan backend */
    uint8_t  reserved[2];
};

/** Frame-budget decisions computed by plan_frame_pacing. */
struct WolfCoreFramePlan
{
    uint32_t max_atmos_lut_steps;        /* max sky precompute slices to submit this frame */
    uint32_t probe_cloud_divisor;        /* resolution divisor for probe cloud raymarch (1 or 4) */
    double   undersea_scan_cooldown_sec; /* pause after a full obstacle scan pass */
    uint8_t  refresh_complete_probe;     /* 1 if already-complete probes may refresh a face this frame */
    uint8_t  reserved[7];
};

/**
 * Function table returned by wolfcore_get_api(WOLFCORE_ABI_VERSION).
 * New functions in future revisions MUST be appended at the end; check struct_size before calling
 * fields added after ABI v1.
 */
struct WolfCoreApi
{
    uint32_t    abi_version;
    uint32_t    struct_size;
    const char* version_string;

    /**
     * Pack uniform values into a CPU-side uniform block mirror with dirty detection.
     * Returns:
     *   1 if one or more bytes in [dst_image, dst_image + dst_size) changed,
     *   0 if the destination already held identical bytes (no GPU UBO upload needed),
     *  -1 if kind dimensions mismatch layout (columns/rows) or the write would exceed dst_size.
     */
    int32_t (WOLFCORE_CALL *pack_uniform_member)(
        uint8_t*                    dst_image,
        size_t                      dst_size,
        const WolfCoreMemberLayout* layout,
        uint32_t                    kind,
        uint32_t                    count,
        const void*                 src_data,
        uint8_t                     transpose);

    /**
     * Inspect a SPIR-V 1.0..1.6 binary module and collect sorted unique result IDs that are
     * statically used by the entry point's interface or by memory/call instructions.
     *
     * If out_ids is NULL or capacity is smaller than the number of used IDs, nothing is written
     * beyond capacity and the total count of statically-used IDs is returned so the caller can
     * allocate and call again. Returns UINT32_MAX if spv_words is not a valid SPIR-V module.
     */
    uint32_t (WOLFCORE_CALL *spv_collect_used_ids)(
        const uint32_t* spv_words,
        size_t          word_count,
        uint32_t*       out_ids,
        uint32_t        capacity);

    /**
     * Compute adaptive per-frame work budgets for native Vulkan & atmosphere/probe/world systems.
     */
    void (WOLFCORE_CALL *plan_frame_pacing)(
        const WolfCoreFrameInputs* inputs,
        WolfCoreFramePlan*         out_plan);
};

typedef const WolfCoreApi* (WOLFCORE_CALL *PFN_wolfcore_get_api)(uint32_t requested_abi_version);

WOLFCORE_API const WolfCoreApi* WOLFCORE_CALL wolfcore_get_api(uint32_t requested_abi_version);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WOLFCORE_API_H */
