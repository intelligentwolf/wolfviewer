/**
 * @file wolfvkspirv.cpp
 * @brief SPIR-V reflection (see wolfvkspirv.h for the specification references and the enumerant citations).
 *
 * [VULKAN BACKEND 2026-11-08]
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

#include "linden_common.h"
#include "wolfvkspirv.h"
#include <unordered_map>
#include <map>
#include <cstring>
#include <algorithm>

namespace
{
    // spirv.hpp (SPIRV-Cross 1.4.363) values, cited in wolfvkspirv.h
    constexpr uint32_t MAGIC = 0x07230203u;   // SPIR-V spec 3.1 "Magic Number"
    enum Op : uint32_t
    {
        OpName = 5, OpMemberName = 6, OpEntryPoint = 15,
        OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24,
        OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeRuntimeArray = 29,
        OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpDecorate = 71, OpMemberDecorate = 72,
    };
    enum Decoration : uint32_t
    {
        DecBlock = 2, DecRowMajor = 4, DecColMajor = 5, DecArrayStride = 6, DecMatrixStride = 7, DecBuiltIn = 11,
        DecLocation = 30, DecBinding = 33, DecDescriptorSet = 34, DecOffset = 35,
    };
    enum StorageClass : uint32_t
    {
        ScUniformConstant = 0, ScInput = 1, ScUniform = 2, ScOutput = 3, ScPushConstant = 9,
    };

    struct Type
    {
        uint32_t mOp = 0;
        uint32_t mWidth = 0;          // int/float bit width
        uint32_t mSigned = 0;         // OpTypeInt signedness
        uint32_t mComponent = 0;      // vector: component type id; matrix: column type id; array: element type id; pointer: pointee; sampled image: image type
        uint32_t mCount = 0;          // vector: components; matrix: columns; array: length constant id
        uint32_t mStorage = 0;        // pointer storage class
        std::vector<uint32_t> mMembers;   // struct member type ids
        // image
        uint32_t mDim = 0, mDepth = 0, mArrayed = 0, mMS = 0;
    };

    struct Decorations
    {
        bool mBlock = false;
        bool mHasLocation = false, mHasBinding = false, mHasSet = false, mBuiltIn = false;
        uint32_t mLocation = 0, mBinding = 0, mSet = 0, mArrayStride = 0;
    };

    struct MemberDecorations
    {
        uint32_t mOffset = 0, mMatrixStride = 0;
        bool mRowMajor = false, mHasOffset = false, mBuiltIn = false;
    };

    struct Parser
    {
        std::unordered_map<uint32_t, Type> mTypes;
        std::unordered_map<uint32_t, std::string> mNames;
        std::map<std::pair<uint32_t, uint32_t>, std::string> mMemberNames;
        std::unordered_map<uint32_t, Decorations> mDecorations;
        std::map<std::pair<uint32_t, uint32_t>, MemberDecorations> mMemberDecorations;
        std::unordered_map<uint32_t, uint32_t> mConstants;   // id -> value (32-bit)
        std::unordered_map<uint32_t, uint32_t> mArrayStrides; // array type id -> stride
        struct Var { uint32_t mId, mTypeId, mStorage; };
        std::vector<Var> mVariables;

        const Type* type(uint32_t id) const
        {
            auto it = mTypes.find(id);
            return it == mTypes.end() ? nullptr : &it->second;
        }

        WolfVkSpirv::BaseType base(const Type& t) const
        {
            switch (t.mOp)
            {
            case OpTypeBool: return WolfVkSpirv::BaseType::BOOL;
            case OpTypeInt: return t.mSigned ? WolfVkSpirv::BaseType::INT : WolfVkSpirv::BaseType::UINT;
            case OpTypeFloat: return t.mWidth == 64 ? WolfVkSpirv::BaseType::DOUBLE : WolfVkSpirv::BaseType::FLOAT;
            default: return WolfVkSpirv::BaseType::UNKNOWN;
            }
        }

        uint32_t arrayLength(const Type& t) const
        {
            if (t.mOp != OpTypeArray) return 1;
            auto it = mConstants.find(t.mCount);
            return it == mConstants.end() ? 1 : it->second;
        }

        // Fills member's base/rows/columns from a scalar, vector or matrix type.
        void describe(const Type& t, WolfVkSpirv::BlockMember& m) const
        {
            if (t.mOp == OpTypeMatrix)
            {
                m.mColumns = t.mCount;
                const Type* col = type(t.mComponent);
                if (col && col->mOp == OpTypeVector)
                {
                    m.mRows = col->mCount;
                    const Type* s = type(col->mComponent);
                    if (s) m.mBase = base(*s);
                }
            }
            else if (t.mOp == OpTypeVector)
            {
                m.mRows = t.mCount;
                const Type* s = type(t.mComponent);
                if (s) m.mBase = base(*s);
            }
            else
            {
                m.mBase = base(t);
            }
        }

        // Flattens the members of struct_id (with its decorations) into out, prefix for nested structs.
        void flatten(uint32_t struct_id, const std::string& prefix, uint32_t base_offset, std::vector<WolfVkSpirv::BlockMember>& out) const
        {
            const Type* st = type(struct_id);
            if (!st) return;
            for (uint32_t i = 0; i < st->mMembers.size(); ++i)
            {
                auto nm = mMemberNames.find({ struct_id, i });
                auto dm = mMemberDecorations.find({ struct_id, i });
                const std::string name = prefix + (nm != mMemberNames.end() ? nm->second : "");
                const uint32_t offset = base_offset + (dm != mMemberDecorations.end() ? dm->second.mOffset : 0);
                const Type* mt = type(st->mMembers[i]);
                if (!mt) continue;

                uint32_t array_size = 1;
                uint32_t array_stride = 0;
                const Type* elem = mt;
                if (mt->mOp == OpTypeArray)
                {
                    array_size = arrayLength(*mt);
                    auto as = mArrayStrides.find(st->mMembers[i]);
                    array_stride = as == mArrayStrides.end() ? 0 : as->second;
                    elem = type(mt->mComponent);
                    if (!elem) continue;
                }
                if (elem->mOp == OpTypeStruct)
                {
                    // nested struct (or array of structs): flatten each element as name[i].member
                    for (uint32_t e = 0; e < array_size; ++e)
                    {
                        const std::string p = array_size > 1 ? name + "[" + std::to_string(e) + "]." : name + ".";
                        const uint32_t inner_struct = (mt->mOp == OpTypeArray) ? mt->mComponent : st->mMembers[i];
                        flatten(inner_struct, p, offset + e * array_stride, out);
                    }
                    continue;
                }
                WolfVkSpirv::BlockMember m;
                m.mName = name;
                m.mOffset = offset;
                m.mArraySize = array_size;
                m.mArrayStride = array_stride;
                describe(*elem, m);
                if (dm != mMemberDecorations.end())
                {
                    m.mMatrixStride = dm->second.mMatrixStride;
                    m.mRowMajor = dm->second.mRowMajor;
                }
                out.push_back(m);
            }
        }
    };
}

namespace WolfVkSpirv
{

uint32_t memberElementSize(const BlockMember& m)
{
    const uint32_t scalar = (m.mBase == BaseType::DOUBLE) ? 8 : 4;
    if (m.mColumns > 1)
    {
        // a matrix element spans (columns - 1) strides plus one column vector
        const uint32_t stride = m.mMatrixStride ? m.mMatrixStride : m.mRows * scalar;
        return m.mRowMajor ? (m.mRows - 1) * stride + m.mColumns * scalar : (m.mColumns - 1) * stride + m.mRows * scalar;
    }
    return m.mRows * scalar;
}

bool reflect(const uint32_t* words, size_t word_count, Module& out, std::string& error)
{
    out = Module();
    // SPIR-V 2.3 Physical Layout: magic, version, generator, bound, schema, then instructions.
    if (!words || word_count < 5)
    {
        error = "SPIR-V module too short";
        return false;
    }
    if (words[0] != MAGIC)
    {
        error = "not a SPIR-V module (bad magic number)";
        return false;
    }

    Parser p;
    size_t i = 5;
    while (i < word_count)
    {
        // 2.2 Terms: first word = word count (high 16 bits) | opcode (low 16 bits)
        const uint32_t first = words[i];
        const uint32_t count = first >> 16;
        const uint32_t op = first & 0xFFFFu;
        if (count == 0 || i + count > word_count)
        {
            error = "SPIR-V instruction runs past the end of the module";
            return false;
        }
        const uint32_t* w = words + i;

        auto literal_string = [&](uint32_t from_word) -> std::string
        {
            // a nul-terminated UTF-8 string packed little-endian into words
            std::string s;
            for (uint32_t k = from_word; k < count; ++k)
            {
                const uint32_t v = w[k];
                for (int b = 0; b < 4; ++b)
                {
                    const char c = static_cast<char>((v >> (8 * b)) & 0xFFu);
                    if (c == 0) return s;
                    s.push_back(c);
                }
            }
            return s;
        };

        switch (op)
        {
        case OpEntryPoint:   // OpEntryPoint ExecutionModel EntryPoint Name ...
            if (count >= 4)
            {
                out.mExecutionModel = w[1];
                out.mEntryPoint = literal_string(3);
            }
            break;
        case OpName:         // OpName Target Name
            if (count >= 3) p.mNames[w[1]] = literal_string(2);
            break;
        case OpMemberName:   // OpMemberName Type Member Name
            if (count >= 4) p.mMemberNames[{ w[1], w[2] }] = literal_string(3);
            break;
        case OpDecorate:     // OpDecorate Target Decoration [literals]
            if (count >= 3)
            {
                Decorations& d = p.mDecorations[w[1]];
                switch (w[2])
                {
                case DecBlock: d.mBlock = true; break;
                case DecLocation: if (count >= 4) { d.mHasLocation = true; d.mLocation = w[3]; } break;
                case DecBinding: if (count >= 4) { d.mHasBinding = true; d.mBinding = w[3]; } break;
                case DecDescriptorSet: if (count >= 4) { d.mHasSet = true; d.mSet = w[3]; } break;
                case DecArrayStride: if (count >= 4) { p.mArrayStrides[w[1]] = w[3]; } break;
                case DecBuiltIn: d.mBuiltIn = true; break;
                default: break;
                }
            }
            break;
        case OpMemberDecorate:   // OpMemberDecorate StructureType Member Decoration [literals]
            if (count >= 4)
            {
                MemberDecorations& d = p.mMemberDecorations[{ w[1], w[2] }];
                switch (w[3])
                {
                case DecOffset: if (count >= 5) { d.mHasOffset = true; d.mOffset = w[4]; } break;
                case DecMatrixStride: if (count >= 5) { d.mMatrixStride = w[4]; } break;
                case DecRowMajor: d.mRowMajor = true; break;
                case DecColMajor: d.mRowMajor = false; break;
                case DecBuiltIn: d.mBuiltIn = true; break;
                default: break;
                }
            }
            break;
        case OpTypeVoid: case OpTypeBool: case OpTypeSampler:
            if (count >= 2) { Type t; t.mOp = op; p.mTypes[w[1]] = t; }
            break;
        case OpTypeInt:      // OpTypeInt Result Width Signedness
            if (count >= 4) { Type t; t.mOp = op; t.mWidth = w[2]; t.mSigned = w[3]; p.mTypes[w[1]] = t; }
            break;
        case OpTypeFloat:    // OpTypeFloat Result Width
            if (count >= 3) { Type t; t.mOp = op; t.mWidth = w[2]; p.mTypes[w[1]] = t; }
            break;
        case OpTypeVector:   // OpTypeVector Result ComponentType ComponentCount
        case OpTypeMatrix:   // OpTypeMatrix Result ColumnType ColumnCount
            if (count >= 4) { Type t; t.mOp = op; t.mComponent = w[2]; t.mCount = w[3]; p.mTypes[w[1]] = t; }
            break;
        case OpTypeImage:    // OpTypeImage Result SampledType Dim Depth Arrayed MS Sampled ImageFormat [AccessQualifier]
            if (count >= 9) { Type t; t.mOp = op; t.mComponent = w[2]; t.mDim = w[3]; t.mDepth = w[4]; t.mArrayed = w[5]; t.mMS = w[6]; p.mTypes[w[1]] = t; }
            break;
        case OpTypeSampledImage:   // OpTypeSampledImage Result ImageType
            if (count >= 3) { Type t; t.mOp = op; t.mComponent = w[2]; p.mTypes[w[1]] = t; }
            break;
        case OpTypeArray:    // OpTypeArray Result ElementType Length(constant id)
            if (count >= 4) { Type t; t.mOp = op; t.mComponent = w[2]; t.mCount = w[3]; p.mTypes[w[1]] = t; }
            break;
        case OpTypeRuntimeArray:   // OpTypeRuntimeArray Result ElementType
            if (count >= 3) { Type t; t.mOp = op; t.mComponent = w[2]; p.mTypes[w[1]] = t; }
            break;
        case OpTypeStruct:   // OpTypeStruct Result Member0Type ...
            if (count >= 2)
            {
                Type t; t.mOp = op;
                for (uint32_t k = 2; k < count; ++k) t.mMembers.push_back(w[k]);
                p.mTypes[w[1]] = t;
            }
            break;
        case OpTypePointer:  // OpTypePointer Result StorageClass Type
            if (count >= 4) { Type t; t.mOp = op; t.mStorage = w[2]; t.mComponent = w[3]; p.mTypes[w[1]] = t; }
            break;
        case OpConstant:     // OpConstant ResultType Result Value...
            if (count >= 4) p.mConstants[w[2]] = w[3];
            break;
        case OpVariable:     // OpVariable ResultType Result StorageClass [Initializer]
            if (count >= 4) p.mVariables.push_back({ w[2], w[1], w[3] });
            break;
        default:
            break;
        }
        i += count;
    }

    // Walk the variables.
    for (const Parser::Var& v : p.mVariables)
    {
        const Type* ptr = p.type(v.mTypeId);
        if (!ptr || ptr->mOp != OpTypePointer) continue;
        const Type* t = p.type(ptr->mComponent);
        if (!t) continue;
        auto dit = p.mDecorations.find(v.mId);
        const Decorations dec = dit == p.mDecorations.end() ? Decorations() : dit->second;
        auto nit = p.mNames.find(v.mId);
        const std::string var_name = nit == p.mNames.end() ? std::string() : nit->second;

        if (v.mStorage == ScUniform || v.mStorage == ScPushConstant)
        {
            // a uniform block: pointer -> struct decorated Block
            if (t->mOp != OpTypeStruct) continue;
            auto sdit = p.mDecorations.find(ptr->mComponent);
            if (sdit == p.mDecorations.end() || !sdit->second.mBlock) continue;
            UniformBlock b;
            auto sn = p.mNames.find(ptr->mComponent);
            b.mName = sn == p.mNames.end() ? var_name : sn->second;
            b.mSet = dec.mSet;
            b.mBinding = dec.mBinding;
            b.mPushConstant = v.mStorage == ScPushConstant;
            p.flatten(ptr->mComponent, "", 0, b.mMembers);
            uint32_t end = 0;
            for (const BlockMember& m : b.mMembers)
            {
                const uint32_t elem = memberElementSize(m);
                const uint32_t span = m.mArraySize > 1 ? (m.mArraySize - 1) * m.mArrayStride + elem : elem;
                end = std::max(end, m.mOffset + span);
            }
            b.mSize = (end + 15u) & ~15u;
            out.mBlocks.push_back(b);
        }
        else if (v.mStorage == ScUniformConstant)
        {
            // samplers: pointer -> (array of) sampled image
            const Type* elem = t;
            uint32_t array_size = 1;
            if (t->mOp == OpTypeArray)
            {
                array_size = p.arrayLength(*t);
                elem = p.type(t->mComponent);
                if (!elem) continue;
            }
            if (elem->mOp != OpTypeSampledImage && elem->mOp != OpTypeImage) continue;
            const Type* img = elem->mOp == OpTypeSampledImage ? p.type(elem->mComponent) : elem;
            if (!img) continue;
            Sampler s;
            s.mName = var_name;
            s.mSet = dec.mSet;
            s.mBinding = dec.mBinding;
            s.mArraySize = array_size;
            s.mDim = img->mDim;
            s.mArrayed = img->mArrayed != 0;
            s.mShadow = img->mDepth == 1;
            s.mMultisampled = img->mMS != 0;
            out.mSamplers.push_back(s);
        }
        else if (v.mStorage == ScInput)
        {
            if (dec.mBuiltIn || !dec.mHasLocation) continue;   // gl_VertexIndex and friends, or interface blocks
            Input in;
            in.mName = var_name;
            in.mLocation = dec.mLocation;
            const Type* elem = t;
            uint32_t arr = 1;
            if (t->mOp == OpTypeArray) { arr = p.arrayLength(*t); elem = p.type(t->mComponent); if (!elem) continue; }
            BlockMember m;
            p.describe(*elem, m);
            in.mBase = m.mBase;
            in.mComponents = m.mRows * m.mColumns;
            in.mLocations = m.mColumns * arr;
            out.mInputs.push_back(in);
        }
        else if (v.mStorage == ScOutput)
        {
            if (dec.mBuiltIn || !dec.mHasLocation) continue;
            Output o;
            o.mName = var_name;
            o.mLocation = dec.mLocation;
            o.mArraySize = t->mOp == OpTypeArray ? p.arrayLength(*t) : 1;
            out.mOutputs.push_back(o);
        }
    }
    return true;
}

} // namespace WolfVkSpirv
