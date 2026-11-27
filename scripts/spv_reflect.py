#!/usr/bin/env python3
"""
[W114-VULKAN spv2 2026-11-26] A small SPIR-V reader for the build's Vulkan shader step (shader_vulkan.py): which
resource variables (samplers, uniform blocks) a module DECLARES and which it STATICALLY USES, and a pass that removes
the declared-but-unused ones. Standard library only.

Static use, Vulkan spec 1.4.363 chapters/shaders.adoc "Static Use": "A SPIR-V module declares a global object in memory
using the OpVariable instruction ... A variable is statically used by an entry point if that entry point, or any
function in its call tree, contains an instruction that references the variable ... A shader entry point also
statically uses any variables explicitly declared in its interface." The viewer's reader (indra/llrender/vk/
wolfvkspirv.cpp reflect(): mUsedIds from OpLoad / Op*AccessChain / OpCopyObject operand 3, every OpFunctionCall
argument, and the OpEntryPoint interface ids) is rule "viewer" below; rule "calltree" is the spec's sentence taken
literally (any operand of any instruction of a function reachable from the entry point). check_module() reports a
module where the two disagree, so the viewer's cheaper rule is proven on every module the build makes.

SPIR-V facts used (SPIR-V 1.6 specification, unified1):
  2.3 "Physical Layout of a SPIR-V Module and Instruction": words 0-4 = magic 0x07230203, version, generator, bound,
      schema; each instruction's first word = word count << 16 | opcode.
  3.x opcodes (spirv.hpp numbers, the same the viewer cites): OpName 5, OpMemberName 6, OpEntryPoint 15, OpTypeImage 25,
      OpTypeSampledImage 27, OpTypeArray 28, OpTypeStruct 30, OpTypePointer 32, OpConstant 43, OpFunction 54,
      OpFunctionEnd 56, OpFunctionCall 57, OpVariable 59, OpLoad 61, OpAccessChain 65, OpInBoundsAccessChain 66,
      OpPtrAccessChain 67, OpDecorate 71, OpMemberDecorate 72, OpCopyObject 83; decorations Block 2, Binding 33,
      DescriptorSet 34; storage classes UniformConstant 0, Uniform 2.
"""

import struct

MAGIC = 0x07230203
OP_NAME, OP_MEMBER_NAME, OP_ENTRY_POINT = 5, 6, 15
OP_TYPE_IMAGE, OP_TYPE_SAMPLED_IMAGE, OP_TYPE_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER = 25, 27, 28, 30, 32
OP_CONSTANT, OP_FUNCTION, OP_FUNCTION_END, OP_FUNCTION_CALL, OP_VARIABLE = 43, 54, 56, 57, 59
OP_LOAD, OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN, OP_PTR_ACCESS_CHAIN = 61, 65, 66, 67
OP_DECORATE, OP_MEMBER_DECORATE, OP_COPY_OBJECT = 71, 72, 83
DEC_BLOCK, DEC_BINDING, DEC_SET = 2, 33, 34
SC_UNIFORM_CONSTANT, SC_UNIFORM = 0, 2
VIEWER_USE_OPS = (OP_LOAD, OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN, OP_PTR_ACCESS_CHAIN, OP_COPY_OBJECT)


def words_of(data):
    if len(data) < 20 or len(data) % 4:
        raise ValueError("not a SPIR-V module (size %d)" % len(data))
    w = list(struct.unpack("<%dI" % (len(data) // 4), data))
    if w[0] != MAGIC:
        raise ValueError("not a SPIR-V module (bad magic number)")
    return w


def instructions(w):
    """[(opcode, [operand words], word offset)] of the module's instructions."""
    out = []
    i = 5
    while i < len(w):
        count, op = w[i] >> 16, w[i] & 0xFFFF
        if count == 0 or i + count > len(w):
            raise ValueError("SPIR-V instruction runs past the end of the module")
        out.append((op, w[i + 1:i + count], i))
        i += count
    return out


def _string(ops, start):
    """The literal string starting at operand `start` and the index of the operand after it."""
    b = bytearray()
    k = start
    while k < len(ops):
        word = ops[k]
        k += 1
        for shift in (0, 8, 16, 24):
            c = (word >> shift) & 0xFF
            if c == 0:
                return b.decode("utf-8", "replace"), k
            b.append(c)
    return b.decode("utf-8", "replace"), k


class Resource:
    def __init__(self):
        self.id = 0
        self.name = ""
        self.kind = ""          # "sampler" | "block"
        self.set = 0
        self.binding = 0
        self.count = 1          # descriptors: an array's length
        self.used_viewer = False
        self.used_calltree = False


def reflect(data):
    """The module's samplers and uniform blocks with both static-use answers. Returns [Resource] in id order."""
    w = words_of(data)
    ins = instructions(w)
    names, types, consts, decs, variables = {}, {}, {}, {}, []
    used_viewer = set()
    functions = {}   # function id -> (set of every id operand its instructions carry, set of called function ids)
    entry_functions = []
    current = None
    for op, ops, _ in ins:
        if op == OP_NAME:
            names[ops[0]] = _string(ops, 1)[0]
        elif op == OP_ENTRY_POINT:
            entry_functions.append(ops[1])
            _, after = _string(ops, 2)
            used_viewer.update(ops[after:])
        elif op == OP_DECORATE:
            decs.setdefault(ops[0], {})[ops[1]] = ops[2] if len(ops) > 2 else True
        elif op in (OP_TYPE_IMAGE, OP_TYPE_SAMPLED_IMAGE, OP_TYPE_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER):
            types[ops[0]] = (op, ops[1:])
        elif op == OP_CONSTANT:
            consts[ops[1]] = ops[2]
        elif op == OP_FUNCTION:
            current = ops[1]
            functions[current] = (set(), set())
        elif op == OP_FUNCTION_END:
            current = None
        elif op == OP_VARIABLE and current is None:
            variables.append((ops[1], ops[0], ops[2]))
        if current is not None and op != OP_FUNCTION:
            functions[current][0].update(ops)   # every operand word: a superset of the ids referenced, never a miss
            if op == OP_FUNCTION_CALL:
                functions[current][1].add(ops[2])
                used_viewer.update(ops[3:])
            elif op in VIEWER_USE_OPS and len(ops) >= 3:
                used_viewer.add(ops[2])
    reach, todo = set(), list(entry_functions)
    while todo:
        f = todo.pop()
        if f in reach or f not in functions:
            continue
        reach.add(f)
        todo.extend(functions[f][1])
    used_calltree = set(used_viewer & set())   # empty
    for f in reach:
        used_calltree |= functions[f][0]
    for op, ops, _ in ins:
        if op == OP_ENTRY_POINT:
            _, after = _string(ops, 2)
            used_calltree.update(ops[after:])
    out = []
    for var_id, ptr_type, storage in variables:
        ptr = types.get(ptr_type)
        if not ptr or ptr[0] != OP_TYPE_POINTER:
            continue
        pointee = ptr[1][1]
        t = types.get(pointee)
        r = Resource()
        r.id = var_id
        r.name = names.get(var_id, "")
        d = decs.get(var_id, {})
        r.set, r.binding = d.get(DEC_SET, 0), d.get(DEC_BINDING, 0)
        if storage == SC_UNIFORM_CONSTANT and t:
            if t[0] == OP_TYPE_ARRAY:
                r.count = consts.get(t[1][1], 1)
                t = types.get(t[1][0])
            if not t or t[0] not in (OP_TYPE_SAMPLED_IMAGE, OP_TYPE_IMAGE):
                continue
            r.kind = "sampler"
        elif storage == SC_UNIFORM and t and t[0] == OP_TYPE_STRUCT and DEC_BLOCK in decs.get(pointee, {}):
            r.kind = "block"
            r.name = names.get(pointee, r.name)
        else:
            continue
        r.used_viewer = var_id in used_viewer
        r.used_calltree = var_id in used_calltree
        out.append(r)
    return out


def check_module(data):
    """Names of resources where the viewer's static-use rule and the call-tree rule disagree (empty = they agree)."""
    return ["%s %s (viewer %s, call tree %s)" % (r.kind, r.name, r.used_viewer, r.used_calltree)
            for r in reflect(data) if r.used_viewer != r.used_calltree]


def strip_unused(data):
    """The module without the resource variables (samplers, uniform blocks) it does not statically use: their OpVariable
    and the OpName / OpDecorate lines that name them go; nothing else changes (types stay: an unreferenced type is
    valid SPIR-V), so every kept variable keeps its set and binding. Returns (new bytes, [removed names])."""
    gone = {r.id: r.name for r in reflect(data) if not r.used_calltree}
    if not gone:
        return data, []
    w = words_of(data)
    out = w[:5]
    for op, ops, at in instructions(w):
        if op in (OP_NAME, OP_DECORATE) and ops[0] in gone:
            continue
        if op == OP_VARIABLE and ops[1] in gone:
            continue
        out.extend(w[at:at + 1 + len(ops)])
    return struct.pack("<%dI" % len(out), *out), sorted(gone.values())


if __name__ == "__main__":
    import sys
    for path in sys.argv[1:]:
        with open(path, "rb") as fh:
            blob = fh.read()
        for r in reflect(blob):
            print("%s %-9s set %d binding %2d x%d  %-32s used: viewer %s, call tree %s" % (
                path, r.kind, r.set, r.binding, r.count, r.name, r.used_viewer, r.used_calltree))
