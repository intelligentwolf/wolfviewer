/**
 * @file wolfvkspirv.h
 * @brief SPIR-V reflection: descriptor bindings, uniform block members with offsets, vertex inputs, outputs.
 *
 * [VULKAN BACKEND 2026-11-08]
 *
 * A small reader of the SPIR-V binary module, enough for the backend's needs and nothing else, so no SPIRV-Cross or
 * SPIRV-Reflect library has to be packaged for three platforms. Everything decoded follows the SPIR-V specification
 * (https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html): the module header (section 2.3 "Physical Layout",
 * magic number 0x07230203), the instruction encoding (2.2: word count in the high 16 bits of the first word, opcode
 * in the low 16 bits), and the opcodes and enumerants below. The numeric values are those of the Khronos header
 * shipped with SPIRV-Cross 1.4.363 (/usr/include/spirv_cross/spirv.hpp, the same file as SPIRV-Headers' spirv.hpp):
 *   OpEntryPoint 15 (spirv.hpp:1733), OpName 5 (:1725), OpMemberName 6 (:1726), OpTypeVoid 19 (:1736), OpTypeBool 20
 *   (:1737), OpTypeInt 21 (:1738), OpTypeFloat 22 (:1739), OpTypeVector 23 (:1740), OpTypeMatrix 24 (:1741), OpTypeImage
 *   25 (:1742), OpTypeSampler 26 (:1743), OpTypeSampledImage 27 (:1744), OpTypeArray 28 (:1745), OpTypeRuntimeArray 29
 *   (:1746), OpTypeStruct 30 (:1747), OpTypePointer 32 (:1749), OpConstant 43 (:1759), OpVariable 59 (:1772),
 *   OpDecorate 71 (:1784), OpMemberDecorate 72 (:1785);
 *   Decoration Block 2 (:501), RowMajor 4 (:503), ColMajor 5 (:504), ArrayStride 6 (:505), MatrixStride 7 (:506),
 *   BuiltIn 11 (:510), Location 30 (:528), Binding 33 (:531), DescriptorSet 34 (:532), Offset 35 (:533);
 *   StorageClass UniformConstant 0 (:218), Input 1 (:219), Uniform 2 (:220), Output 3 (:221), PushConstant 9 (:227);
 *   ExecutionModel Vertex 0 (:66), Fragment 4 (:70); Dim 2D 1 (:261), 3D 2 (:262), Cube 3 (:263), Buffer 5 (:265).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * Copyright (C) 2026, IntelligentWolf Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_VK_SPIRV_H
#define WOLF_VK_SPIRV_H

#include <cstdint>
#include <string>
#include <vector>

namespace WolfVkSpirv
{
    enum class BaseType { UNKNOWN, BOOL, INT, UINT, FLOAT, DOUBLE };

    // One member of a uniform block, flattened: "name" for scalars/vectors/matrices/arrays of those; struct members
    // are flattened as "outer.inner" (GLSL's own naming for glGetUniformLocation).
    struct BlockMember
    {
        std::string mName;
        BaseType mBase = BaseType::UNKNOWN;
        uint32_t mColumns = 1;        // matrix columns (1 for non-matrices)
        uint32_t mRows = 1;           // vector size / matrix rows
        uint32_t mArraySize = 1;      // 1 when not an array
        uint32_t mOffset = 0;         // Decoration Offset, bytes from the start of the block
        uint32_t mArrayStride = 0;    // Decoration ArrayStride (0 when not an array)
        uint32_t mMatrixStride = 0;   // Decoration MatrixStride (0 when not a matrix)
        bool mRowMajor = false;
    };

    struct UniformBlock
    {
        std::string mName;            // the block's type name (OpName of the struct), e.g. "WolfUniformsV"
        uint32_t mSet = 0;
        uint32_t mBinding = 0;
        uint32_t mSize = 0;           // bytes: last member's offset + size, rounded to 16
        bool mPushConstant = false;
        std::vector<BlockMember> mMembers;
    };

    struct Sampler
    {
        std::string mName;
        uint32_t mSet = 0;
        uint32_t mBinding = 0;
        uint32_t mArraySize = 1;
        uint32_t mDim = 1;            // SPIR-V Dim: 1 = 2D, 2 = 3D, 3 = Cube, 5 = Buffer
        bool mArrayed = false;        // 2D array / cube array
        bool mShadow = false;         // depth comparison sampler (OpTypeImage Depth = 1)
        bool mMultisampled = false;
    };

    struct Input
    {
        std::string mName;
        uint32_t mLocation = 0;
        BaseType mBase = BaseType::FLOAT;
        uint32_t mComponents = 1;     // 1..4; a matrix input counts columns x rows
        uint32_t mLocations = 1;      // locations consumed (matrices, arrays)
    };

    struct Output
    {
        std::string mName;
        uint32_t mLocation = 0;
        uint32_t mArraySize = 1;
    };

    struct Module
    {
        uint32_t mExecutionModel = 0;   // 0 vertex, 4 fragment
        std::string mEntryPoint;
        std::vector<UniformBlock> mBlocks;
        std::vector<Sampler> mSamplers;
        std::vector<Input> mInputs;     // stage inputs with a Location (built-ins skipped)
        std::vector<Output> mOutputs;   // stage outputs with a Location
    };

    // Parses the module. Returns false with a reason when the words are not a SPIR-V module this reader understands.
    bool reflect(const uint32_t* words, size_t word_count, Module& out, std::string& error);

    // Size in bytes of one element of a member (a vec3 is 12, a mat4 column-major 64 with its stride, etc.)
    uint32_t memberElementSize(const BlockMember& m);
}

#endif // WOLF_VK_SPIRV_H
