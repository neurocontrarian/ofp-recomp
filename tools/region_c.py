#!/usr/bin/env python3
"""Translates one region of ColdWarAssault.exe (a hot loop with one entry and
one exit) into C that keeps the function's stack slots and the x87 stack in
host variables, for the regions where the lifted code is too slow: there every
[esp+X] is a guest memory access the compiler cannot keep in a register (any
guest store may alias it), every x87 value goes through the ring, and every
compare writes flags.

What it relies on, checked here or at run time (OFP_NATIVE=check):
  * the region is entered at START only and left at END only (jumps into or
    out of it are refused);
  * esp is the frame: every push in the region is an argument of the next
    call, which pops them all (stdcall/thiscall), so esp is back at the
    region's level after each call;
  * a stack slot whose address is taken (lea r, [esp+X]) is memory, with the
    0x20 bytes after it, and so are the arguments pushed and anything below
    the region's esp or past the frame (FRAME);
  * the x87 stack is empty at START, END and every call; a call followed by
    a pop of an empty stack returns a float (it is taken from the ring);
  * a jump reads the flags of the last flag-setting instruction before it in
    its block.

usage: region_c.py START END FRAME NAME > file.c   (addresses in hex)
The C function is uint32_t NAME(void): registers and esp in g_*, returns END.
"""
import functools
import os
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import (X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_AH, X86_REG_AL, X86_REG_AX,
                          X86_REG_BH, X86_REG_BL, X86_REG_BX, X86_REG_CH, X86_REG_CL, X86_REG_CX,
                          X86_REG_DH, X86_REG_DL, X86_REG_DX, X86_REG_EAX, X86_REG_EBP, X86_REG_EBX,
                          X86_REG_ECX, X86_REG_EDI, X86_REG_EDX, X86_REG_ESI, X86_REG_ESP,
                          X86_REG_INVALID, X86_REG_SI, X86_REG_DI, X86_REG_BP, X86_REG_ST0)

EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'work', 'ColdWarAssault.exe')
R32 = {X86_REG_EAX: 'eax', X86_REG_ECX: 'ecx', X86_REG_EDX: 'edx', X86_REG_EBX: 'ebx',
       X86_REG_ESI: 'esi', X86_REG_EDI: 'edi', X86_REG_EBP: 'ebp'}
R16 = {X86_REG_AX: 'eax', X86_REG_CX: 'ecx', X86_REG_DX: 'edx', X86_REG_BX: 'ebx',
       X86_REG_SI: 'esi', X86_REG_DI: 'edi', X86_REG_BP: 'ebp'}
R8L = {X86_REG_AL: 'eax', X86_REG_CL: 'ecx', X86_REG_DL: 'edx', X86_REG_BL: 'ebx'}
R8H = {X86_REG_AH: 'eax', X86_REG_CH: 'ecx', X86_REG_DH: 'edx', X86_REG_BH: 'ebx'}
REGS = ('eax', 'ecx', 'edx', 'ebx', 'esi', 'edi', 'ebp')
FPOP = {'fstp', 'fistp', 'faddp', 'fsubp', 'fsubrp', 'fmulp', 'fdivp', 'fdivrp', 'fcomp'}
FPOP2 = {'fcompp'}
FPUSH = {'fld', 'fild', 'fld1', 'fldz'}
JCC = {'je', 'jne', 'jl', 'jge', 'jle', 'jg', 'jb', 'jae', 'jbe', 'ja', 'js', 'jns', 'jp', 'jnp'}
FLAGSET = {'cmp', 'test', 'add', 'sub', 'and', 'or', 'xor', 'inc', 'dec', 'neg', 'sar', 'shl', 'shr', 'imul', 'sbb'}


MEMN = {1: 'MEM8', 2: 'MEM16', 4: 'MEM32'}


@functools.lru_cache(maxsize=None)
def exe_bytes():
    return open(EXE, 'rb').read()


def load(s, e):
    d = exe_bytes()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    ns = struct.unpack_from('<H', d, pe + 6)[0]
    osz = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    so = pe + 24 + osz
    for i in range(ns):
        _, vs, va, rs, ra = struct.unpack_from('<8sIIII', d, so + 40 * i)
        if base + va <= s < base + va + vs:
            return d[ra + s - base - va: ra + e - base - va]
    raise SystemExit('START not in a section')


@functools.lru_cache(maxsize=None)
def callee_rets(va):
    """the N of every ret N reachable in the function at VA"""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    by = {j.address: j for j in md.disasm(load(va, va + 0x4000), va)}
    seen, work, rs = set(), [va], set()
    while work:
        a = work.pop()
        while a not in seen and a in by:
            seen.add(a)
            j = by[a]
            if j.mnemonic in JCC or j.mnemonic == 'jmp':
                if j.operands[0].type == X86_OP_IMM:
                    work.append(j.operands[0].imm)
                if j.mnemonic == 'jmp':
                    break
            if j.mnemonic == 'ret':
                rs.add(j.operands[0].imm if j.operands else 0)
                break
            a += j.size
    return tuple(sorted(rs))


class Fail(Exception):
    pass


def main():
    start, end, name = int(sys.argv[1], 16), int(sys.argv[2], 16), sys.argv[4]
    # FRAME "func": the region is a whole leaf function, entered with esp at
    # its return address and left by ret
    func = sys.argv[3] == 'func'
    frame = 0 if func else int(sys.argv[3], 16)
    rets = {}
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    if func and end == 0:
        # a whole function: the instructions reachable from its entry
        lin = list(md.disasm(load(start, start + 0x4000), start))
        byaddr = {i.address: i for i in lin}
        seen, work = set(), [start]
        while work:
            a = work.pop()
            while a not in seen:
                if a not in byaddr:
                    raise Fail('%08X: not an instruction boundary' % a)
                seen.add(a)
                i = byaddr[a]
                m = i.mnemonic
                if m in JCC or m == 'jmp':
                    op = i.operands[0]
                    if op.type != X86_OP_IMM:
                        raise Fail('%08X: indirect jump' % a)
                    work.append(op.imm)
                    if m == 'jmp':
                        break
                if m == 'ret':
                    break
                a += i.size
        ins = [byaddr[a] for a in sorted(seen)]
        end = ins[-1].address + ins[-1].size
    else:
        ins = list(md.disasm(load(start, end), start))
    if not ins or ins[-1].address + ins[-1].size != end:
        raise Fail('region does not end on an instruction boundary')
    at = {i.address: k for k, i in enumerate(ins)}

    def callee_pops(i, pend):
        """what a call pops: a direct callee's ret N; an indirect one, the
        arguments pushed for it unless the caller pops them (add esp after)"""
        op = i.operands[0]
        nxt = ins[at[i.address] + 1] if at[i.address] + 1 < len(ins) else None
        if op.type == X86_OP_IMM:
            rs = set(callee_rets(op.imm))
            if len(rs) != 1:
                raise Fail('%08X: callee %08X returns with %s' % (i.address, op.imm, sorted(rs)))
            n = rs.pop()
            if n > 4 * pend:
                raise Fail('%08X: callee pops %d, %d pushed' % (i.address, n, 4 * pend))
            return n
        if nxt is not None and nxt.mnemonic == 'add' and nxt.operands[0].type == X86_OP_REG \
                and nxt.operands[0].reg == X86_REG_ESP:
            return 0
        return 4 * pend

    def target(i):
        op = i.operands[0]
        return op.imm if op.type == X86_OP_IMM else None

    leaders = {start}
    for k, i in enumerate(ins):
        if i.mnemonic in JCC or i.mnemonic == 'jmp':
            t = target(i)
            if t is None or not (start <= t < end) or t not in at:
                raise Fail('%08X: jump out of the region or indirect' % i.address)
            leaders.add(t)
            if k + 1 < len(ins):
                leaders.add(ins[k + 1].address)

    # esp delta and x87 depth before each instruction (dataflow from START)
    floatret = set()
    while True:
        dl, fd = {}, {}
        work = [(start, 0, 0, 0)]
        redo = False
        pops = {}
        while work and not redo:
            a, d, f, pend = work.pop()
            k = at[a]
            while True:
                i = ins[k]
                if i.address in dl:
                    if (dl[i.address], fd[i.address]) != (d, f):
                        raise Fail('%08X: esp %d / x87 %d here, %d / %d by another path'
                                   % (i.address, d, f, dl[i.address], fd[i.address]))
                    break
                dl[i.address], fd[i.address] = d, f
                m = i.mnemonic
                if m == 'push':
                    d -= 4
                    pend += 1
                elif m == 'pop':
                    d += 4
                    pend = max(0, pend - 1)
                elif func and m in ('sub', 'add') and i.operands[0].type == X86_OP_REG and i.operands[0].reg == X86_REG_ESP \
                        and i.operands[1].type == X86_OP_IMM:
                    d += -i.operands[1].imm if m == 'sub' else i.operands[1].imm
                elif m == 'ret':
                    if not func:
                        raise Fail('%08X: ret in a loop region' % i.address)
                    if d != 0 or f > 1:
                        raise Fail('%08X: ret with esp %d, x87 %d' % (i.address, d, f))
                    rets[i.address] = (i.operands[0].imm if i.operands else 0, f)
                    break
                elif m == 'call':
                    if f:
                        raise Fail('%08X: call with %d x87 values' % (i.address, f))
                    pops[i.address] = n = callee_pops(i, pend)
                    d += n
                    pend = 0
                    if i.address in floatret:
                        f = 1
                elif m in FPUSH:
                    f += 1
                elif m in FPOP2:
                    if f < 2:
                        raise Fail('%08X: fcompp with %d values' % (i.address, f))
                    f -= 2
                elif m in FPOP:
                    if f == 0:
                        p = k - 1
                        while p >= 0 and ins[p].mnemonic != 'call' and not ins[p].mnemonic.startswith('f'):
                            p -= 1
                        if p < 0 or ins[p].mnemonic != 'call':
                            raise Fail('%08X: x87 pop of an empty stack' % i.address)
                        floatret.add(ins[p].address)
                        redo = True
                        break
                    f -= 1
                elif m in ('add', 'sub', 'lea', 'mov') and i.operands[0].type == X86_OP_REG and i.operands[0].reg == X86_REG_ESP \
                        and not (m == 'lea' and i.operands[1].mem.base == X86_REG_ESP and i.operands[1].mem.disp == 0):
                    raise Fail('%08X: esp changed' % i.address)
                if f < 0 or f > 8:
                    raise Fail('%08X: x87 depth %d' % (i.address, f))
                if m in JCC:
                    work.append((target(i), d, f, pend))
                elif m == 'jmp':
                    work.append((target(i), d, f, pend))
                    break
                k += 1
                if k == len(ins):
                    if func:
                        raise Fail('the function runs past its end')
                    if (d, f) != (0, 0):
                        raise Fail('END reached with esp %d, x87 %d' % (d, f))
                    break
        if not redo:
            break
    for i in ins:
        if i.address not in dl:
            raise Fail('%08X: not reached' % i.address)
        if not func and i.address in leaders and dl[i.address] != 0:
            raise Fail('%08X: block starts with pushed arguments' % i.address)

    # stack slots: offsets from the region's esp; promoted unless taken
    takeninfo = {}
    acc = {}
    for i in ins:
        for op in i.operands:
            if op.type != X86_OP_MEM or op.mem.base != X86_REG_ESP:
                continue
            off = dl[i.address] + op.mem.disp
            if i.mnemonic == 'lea':
                if op.mem.index != X86_REG_INVALID:
                    raise Fail('%08X: indexed stack address' % i.address)
                if not (i.operands[0].type == X86_OP_REG and i.operands[0].reg == X86_REG_ESP):
                    nxt = next((j for j in ins[at[i.address]:] if j.mnemonic == 'call'), None)
                    takeninfo.setdefault(off, set()).add(nxt.op_str if nxt else '-')
                continue
            if op.mem.index != X86_REG_INVALID:
                raise Fail('%08X: indexed stack access' % i.address)
            acc.setdefault(off, set()).add(op.size)
    pushed = {dl[i.address] - 4 for i in ins if i.mnemonic == 'push'}
    # an object whose address is taken: REGION_TAKEN's size for it, else up to
    # the next object taken (or the frame's end: what a callee may write)
    ext = {}
    for x in (os.environ.get('REGION_TAKEN') or '').split(','):
        if x:
            o, n = x.split(':')
            ext[int(o, 0)] = int(n, 0)
    taken = sorted(takeninfo)
    for k, t in enumerate(taken):
        if t not in ext:
            ext[t] = (taken[k + 1] if k + 1 < len(taken) else max(frame, t + 0x20)) - t
        sys.stderr.write('  taken: esp+0x%X (%s bytes) -> %s\n' % (t, ext[t], ', '.join(sorted(takeninfo[t]))))
    mem = set()
    for off, sizes in acc.items():
        outside = (off < 4 or off in pushed) if func else (off < 0 or off + max(sizes) > frame)
        if func and off < 0 and off not in pushed:
            outside = False                       # a local
        if outside or any(t <= off < t + ext[t] for t in taken):
            mem.add(off)
    for off in list(acc):
        if off in mem:
            continue
        s = max(acc[off])
        if any(o != off and o < off + s and off < o + max(acc[o]) for o in acc):
            mem.add(off)
    slots = {off: max(acc[off]) for off in acc if off not in mem}
    if os.environ.get('REGION_DEBUG'):
        for off in sorted(mem):
            sys.stderr.write('  memory: esp+0x%X sizes %s\n' % (off, sorted(acc[off])))
    written = set()

    def cty(n):
        return {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}[n]

    def addr(i, op):
        mm = op.mem
        if mm.base == X86_REG_ESP:
            return '(E + %d)' % (dl[i.address] + mm.disp)
        parts = []
        if mm.base != X86_REG_INVALID:
            parts.append(R32[mm.base])
        if mm.index != X86_REG_INVALID:
            parts.append('%s * %d' % (R32[mm.index], mm.scale) if mm.scale > 1 else R32[mm.index])
        if mm.disp:
            parts.append('0x%Xu' % (mm.disp & 0xFFFFFFFF))
        return '(uint32_t)(%s)' % (' + '.join(parts) or '0')

    def sname(off):
        return 's_%X' % off if off >= 0 else 's_m%X' % -off

    def slot(i, op):
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_ESP:
            off = dl[i.address] + op.mem.disp
            if off in slots:
                return sname(off)
        return None

    def rd(i, op, signed=False):
        """an integer operand's value, as uint32_t (sign-extended if signed)"""
        if op.type == X86_OP_IMM:
            return '0x%Xu' % (op.imm & 0xFFFFFFFF)
        if op.type == X86_OP_REG:
            r = op.reg
            if r in R32:
                return R32[r]
            if r in R16:
                return '(uint32_t)(int32_t)(int16_t)%s' % R16[r] if signed else '(%s & 0xFFFFu)' % R16[r]
            if r in R8L:
                return '(uint32_t)(int32_t)(int8_t)%s' % R8L[r] if signed else '(%s & 0xFFu)' % R8L[r]
            if r in R8H:
                return '(uint32_t)(int32_t)(int8_t)(%s >> 8)' % R8H[r] if signed else '((%s >> 8) & 0xFFu)' % R8H[r]
            raise Fail('%08X: register %s' % (i.address, i.reg_name(r)))
        s = slot(i, op)
        n = op.size
        if s:
            v = s if n == slots[dl[i.address] + op.mem.disp] else '(%s)%s' % (cty(n), s)
        else:
            v = MEMN[n] + '(%s)' % addr(i, op)
        if n == 4:
            return '(uint32_t)%s' % v
        return ('(uint32_t)(int32_t)(%s)%s' % ({1: 'int8_t', 2: 'int16_t'}[n], v)) if signed else '(uint32_t)%s' % v

    def wr(i, op, val):
        if op.type == X86_OP_REG:
            r = op.reg
            if r in R32:
                return '%s = %s;' % (R32[r], val)
            if r in R16:
                return '%s = (%s & 0xFFFF0000u) | ((%s) & 0xFFFFu);' % (R16[r], R16[r], val)
            if r in R8L:
                return '%s = (%s & 0xFFFFFF00u) | ((%s) & 0xFFu);' % (R8L[r], R8L[r], val)
            if r in R8H:
                return '%s = (%s & 0xFFFF00FFu) | (((%s) & 0xFFu) << 8);' % (R8H[r], R8H[r], val)
            raise Fail('%08X: register %s' % (i.address, i.reg_name(r)))
        s = slot(i, op)
        if s:
            written.add(s)
            full = slots[dl[i.address] + op.mem.disp]
            if op.size == full:
                return '%s = (%s)(%s);' % (s, cty(op.size), val)
            mask = (1 << (8 * op.size)) - 1
            return '%s = (%s)((%s & ~0x%Xu) | ((%s) & 0x%Xu));' % (s, cty(full), s, mask, val, mask)
        return '%s(%s) = (%s)(%s);' % (MEMN[op.size], addr(i, op), cty(op.size), val)

    def fsrc(i, op):
        """an x87 memory operand as a double"""
        s = slot(i, op)
        if op.size == 4:
            return 'u2f(%s)' % (s if s else 'MEM32(%s)' % addr(i, op))
        if op.size == 8:
            if s:
                raise Fail('%08X: 8-byte slot' % i.address)
            return 'u2d(MEM64(%s))' % addr(i, op)
        raise Fail('%08X: x87 operand of %d bytes' % (i.address, op.size))

    def st(i, n, depth=None):
        f = fd[i.address] if depth is None else depth
        if n >= f:
            raise Fail('%08X: st(%d) of %d' % (i.address, n, f))
        return 'x%d' % (f - 1 - n)

    # flags: each consumer reads its block's last producer, whose operands are
    # kept in fa/fb/fr (and its kind decides the condition)
    producer = {}
    last = None
    for i in ins:
        if i.address in leaders:
            last = None
        m = i.mnemonic
        if m in JCC or m in ('sbb', 'adc'):
            if last is None:
                raise Fail('%08X: flags from another block' % i.address)
            producer[i.address] = last
        if m in FLAGSET:
            last = i
        if m == 'call':
            last = None
    used = {p.address for p in producer.values()}

    def cond(i):
        p = producer[i.address]
        m, k = i.mnemonic, p.mnemonic
        if k in ('cmp', 'sub'):
            c = {'je': 'fa == fb', 'jne': 'fa != fb', 'jl': '(int32_t)fa < (int32_t)fb',
                 'jge': '(int32_t)fa >= (int32_t)fb', 'jle': '(int32_t)fa <= (int32_t)fb',
                 'jg': '(int32_t)fa > (int32_t)fb', 'jb': 'fa < fb', 'jae': 'fa >= fb',
                 'jbe': 'fa <= fb', 'ja': 'fa > fb', 'js': '(int32_t)(fa - fb) < 0',
                 'jns': '(int32_t)(fa - fb) >= 0'}
            return c[m]
        if k in ('sar', 'shl', 'shr') and m not in ('je', 'jne', 'js', 'jns'):
            raise Fail('%08X: %s after %s' % (i.address, m, k))
        if k in ('test', 'and', 'or', 'xor', 'sar', 'shl', 'shr'):   # OF = 0
            c = {'je': 'fr == 0', 'jne': 'fr != 0', 'js': '(int32_t)fr < 0', 'jns': '(int32_t)fr >= 0',
                 'jl': '(int32_t)fr < 0', 'jge': '(int32_t)fr >= 0', 'jle': '(int32_t)fr <= 0',
                 'jg': '(int32_t)fr > 0'}
            if p.operands[0].size == 1:       # the parity of the low byte (here the top one)
                c['jp'] = '!__builtin_parity(fr >> 24)'
                c['jnp'] = '__builtin_parity(fr >> 24)'
            if m not in c:
                raise Fail('%08X: %s after %s' % (i.address, m, k))
            return c[m]
        if k in ('inc', 'dec', 'add', 'neg'):
            c = {'je': 'fr == 0', 'jne': 'fr != 0', 'js': '(int32_t)fr < 0', 'jns': '(int32_t)fr >= 0'}
            if k in ('inc', 'dec', 'add'):    # SF != OF: the exact signed result's sign
                sign = '+' if k != 'dec' else '-'
                c.update({'jl': '((int64_t)(int32_t)fa %s (int64_t)(int32_t)fb) < 0' % sign,
                          'jge': '((int64_t)(int32_t)fa %s (int64_t)(int32_t)fb) >= 0' % sign,
                          'jle': '((int64_t)(int32_t)fa %s (int64_t)(int32_t)fb) <= 0' % sign,
                          'jg': '((int64_t)(int32_t)fa %s (int64_t)(int32_t)fb) > 0' % sign})
            if m not in c:
                raise Fail('%08X: %s after %s' % (i.address, m, k))
            return c[m]
        raise Fail('%08X: %s after %s' % (i.address, m, k))

    body = []
    b = body.append
    for k, i in enumerate(ins):
        a = i.address
        m = i.mnemonic
        ops = i.operands
        if a in leaders:
            b('L_%08X:' % a)
        c = '/* %08X: %s %s */' % (a, m, i.op_str)
        keep = a in used

        sh = (32 - 8 * ops[0].size) if ops and ops[0].type in (X86_OP_REG, X86_OP_MEM) else 0

        def flags(x, y, r):
            if not keep:
                return ''
            if sh:
                return ' fa = (%s) << %d; fb = (%s) << %d; fr = (%s) << %d;' % (x, sh, y, sh, r, sh)
            return ' fa = %s; fb = %s; fr = %s;' % (x, y, r)

        if m == 'nop' or (m == 'lea' and ops[0].type == X86_OP_REG and ops[0].reg == X86_REG_ESP) \
                or (func and m in ('sub', 'add') and ops[0].type == X86_OP_REG and ops[0].reg == X86_REG_ESP):
            b('    ' + c)
        elif m in ('mov', 'movzx', 'movsx'):
            b('    %s %s' % (wr(i, ops[0], rd(i, ops[1], signed=m == 'movsx')), c))
        elif m == 'lea':
            mm = ops[1]
            if mm.mem.base == X86_REG_ESP:
                b('    %s %s' % (wr(i, ops[0], '(E + %d)' % (dl[a] + mm.mem.disp)), c))
            else:
                b('    %s %s' % (wr(i, ops[0], addr(i, mm)), c))
        elif m in ('add', 'sub', 'and', 'or', 'xor'):
            x, y = rd(i, ops[0]), rd(i, ops[1])
            if m == 'xor' and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG and ops[0].reg == ops[1].reg:
                b('    { %s%s } %s' % (wr(i, ops[0], '0u'), flags('0u', '0u', '0u'), c))
            else:
                op = {'add': '+', 'sub': '-', 'and': '&', 'or': '|', 'xor': '^'}[m]
                b('    { uint32_t a_ = %s, b_ = %s, r_ = a_ %s b_; %s%s } %s'
                  % (x, y, op, wr(i, ops[0], 'r_'), flags('a_', 'b_', 'r_'), c))
        elif m in ('inc', 'dec', 'neg', 'not'):
            x = rd(i, ops[0])
            r = {'inc': 'a_ + 1u', 'dec': 'a_ - 1u', 'neg': '0u - a_', 'not': '~a_'}[m]
            fb = {'inc': '1u', 'dec': '1u', 'neg': '0u', 'not': '0u'}[m]
            b('    { uint32_t a_ = %s, r_ = %s; %s%s } %s' % (x, r, wr(i, ops[0], 'r_'),
                                                         flags('a_', fb, 'r_') + (' fc = a_ != 0;' if m == 'neg' and keep else ''), c))
        elif m == 'sbb':
            p = producer[a]
            if p.mnemonic != 'neg' or not (ops[0].type == ops[1].type == X86_OP_REG and ops[0].reg == ops[1].reg):
                raise Fail('%08X: sbb other than neg; sbb r, r' % a)
            b('    { uint32_t r_ = fc ? 0xFFFFFFFFu : 0u; %s%s } %s' % (wr(i, ops[0], 'r_'), flags('r_', '0u', 'r_'), c))
        elif m in ('shl', 'sar', 'shr'):
            if ops[1].type != X86_OP_IMM:
                raise Fail('%08X: shift by a register' % a)
            n = ops[1].imm & 31
            x = rd(i, ops[0])
            r = {'shl': 'a_ << %d' % n, 'shr': 'a_ >> %d' % n, 'sar': '(uint32_t)((int32_t)a_ >> %d)' % n}[m]
            b('    { uint32_t a_ = %s, r_ = %s; %s%s } %s' % (x, r, wr(i, ops[0], 'r_'), flags('a_', '0u', 'r_'), c))
        elif m == 'imul':
            if len(ops) == 2:
                x, y = rd(i, ops[0]), rd(i, ops[1])
            elif len(ops) == 3:
                x, y = rd(i, ops[1]), rd(i, ops[2])
            else:
                raise Fail('%08X: one-operand imul' % a)
            if keep:
                raise Fail('%08X: flags of imul' % a)
            b('    %s %s' % (wr(i, ops[0], '(uint32_t)((int32_t)%s * (int32_t)%s)' % (x, y)), c))
        elif m in ('cmp', 'test'):
            x, y = rd(i, ops[0]), rd(i, ops[1])
            if keep:
                if m == 'cmp':
                    b('    fa = (%s) << %d; fb = (%s) << %d; %s' % (x, sh, y, sh, c))
                else:
                    b('    fr = (%s & %s) << %d; %s' % (x, y, sh, c))
            else:
                b('    ' + c)
        elif m in JCC:
            back = 'BACKEDGE(); ' if target(i) <= a else ''
            b('    if (%s) { %sgoto L_%08X; } %s' % (cond(i), back, target(i), c))
        elif m == 'jmp':
            back = 'BACKEDGE(); ' if target(i) <= a else ''
            b('    %sgoto L_%08X; %s' % (back, target(i), c))
        elif m == 'push':
            b('    MEM32(E + %d) = %s; %s' % (dl[a] - 4, rd(i, ops[0]), c))
        elif m == 'pop':
            b('    %s %s' % (wr(i, ops[0], 'MEM32(E + %d)' % dl[a]), c))
        elif m == 'ret':
            n, depth = rets[a]
            b('    REGS_OUT(E + %d);%s return; %s' % (4 + n, ' fp_push_ring(x0, 0);' if depth else '', c))
        elif m == 'call':
            nargs = -dl[a] // 4
            op = ops[0]
            if op.type == X86_OP_IMM:
                fn = 'sub_%08X()' % op.imm
                decl.add('void sub_%08X(void);' % op.imm)
            else:
                fn = 'RCALL(%s)' % rd(i, op)
            after = dl[a] + pops[a]
            call = ('REGS_OUT(E + %d); MEM32(g_esp -= 4) = RECOMP_RETADDR; %s; REGS_IN(); '
                    'if (CHECK && g_esp != E + %d) region_bad(0x%08Xu, g_esp - E);' % (dl[a], fn, after, a))
            if a in floatret:
                call += ' x0 = fp_pop_ring();'
            if op.type != X86_OP_IMM and a in floatret and nargs == 1:
                # a virtual that may be one returning its float argument (fld [esp+4];
                # ret 4: TextureD3D__virtual_15): that one is done here
                b('    { uint32_t t_ = %s; if (IDENTITY_F1(t_)) x0 = (double)u2f(MEM32(E + %d)); else { %s } } %s'
                  % (rd(i, op), dl[a], call.replace(rd(i, op), 't_'), c))
            else:
                b('    %s %s' % (call, c))
        elif m in ('fld', 'fild'):
            op = ops[0]
            dst = 'x%d' % fd[a]
            if op.type == X86_OP_REG:
                b('    %s = %s; %s' % (dst, st(i, op.reg - X86_REG_ST0), c))
            elif m == 'fld':
                b('    %s = %s; %s' % (dst, fsrc(i, op), c))
            else:
                if op.size != 4:
                    raise Fail('%08X: fild of %d bytes' % (a, op.size))
                b('    %s = (double)(int32_t)%s; %s' % (dst, rd(i, op), c))
        elif m in ('fst', 'fstp'):
            op = ops[0]
            if op.type == X86_OP_REG:
                b('    %s = %s; %s' % (st(i, op.reg - X86_REG_ST0), st(i, 0), c))
            elif op.size == 4:
                b('    %s %s' % (wr(i, op, 'f2u((float)%s)' % st(i, 0)), c))
            else:
                raise Fail('%08X: fst of %d bytes' % (a, op.size))
        elif m in ('fadd', 'fsub', 'fmul', 'fdiv', 'fsubr', 'fdivr'):
            sym = {'fadd': '+', 'fsub': '-', 'fmul': '*', 'fdiv': '/', 'fsubr': '-', 'fdivr': '/'}[m]
            rev = m in ('fsubr', 'fdivr')
            if len(ops) == 1 and ops[0].type == X86_OP_MEM:
                d, s = st(i, 0), fsrc(i, ops[0])
            elif len(ops) == 1 and ops[0].type == X86_OP_REG:     # st(0) op= st(i), as lift32
                d, s = st(i, 0), st(i, ops[0].reg - X86_REG_ST0)
            elif len(ops) == 2:
                d, s = st(i, ops[0].reg - X86_REG_ST0), st(i, ops[1].reg - X86_REG_ST0)
            else:
                raise Fail('%08X: %s %s' % (a, m, i.op_str))
            b('    %s = %s %s %s; %s' % (d, s if rev else d, sym, d if rev else s, c))
        elif m in ('fcom', 'fcomp', 'fcompp'):
            if m == 'fcompp':
                src = st(i, 1)
            elif len(ops) == 1 and ops[0].type == X86_OP_MEM:
                src = fsrc(i, ops[0])
            elif len(ops) == 1 and ops[0].type == X86_OP_REG:
                src = st(i, ops[0].reg - X86_REG_ST0)
            else:
                raise Fail('%08X: %s %s' % (a, m, i.op_str))
            b('    fca = %s; fcb = %s; %s' % (st(i, 0), src, c))
        elif m == 'fnstsw':
            if not (len(ops) == 1 and ops[0].type == X86_OP_REG and ops[0].reg == X86_REG_AX):
                raise Fail('%08X: fnstsw %s' % (a, i.op_str))
            b('    eax = (eax & 0xFFFF0000u) | FSW(fca, fcb); %s' % c)
        elif m == 'fxch':
            j = ops[-1].reg - X86_REG_ST0 if ops else 1    # capstone may list the implicit st(0) first
            b('    { double t_ = %s; %s = %s; %s = t_; } %s' % (st(i, 0), st(i, 0), st(i, j), st(i, j), c))
        elif m in ('rep movsd', 'rep movsb'):
            n = 4 if m == 'rep movsd' else 1
            b('    recomp_rep_movs(edi, esi, ecx, %du, 1); esi += ecx * %du; edi += ecx * %du; ecx = 0; %s' % (n, n, n, c))
        elif m in ('faddp', 'fmulp', 'fsubp'):
            # st(i) op= st(0), then pop (fsubp as lift32: st(i) - st(0); its
            # two-operand spelling is refused, the operand order being a trap)
            if len(ops) == 1 and ops[0].type == X86_OP_REG or \
                    m != 'fsubp' and len(ops) == 2 and ops[1].reg == X86_REG_ST0:
                d = st(i, ops[0].reg - X86_REG_ST0)
            else:
                raise Fail('%08X: %s %s' % (a, m, i.op_str))
            b('    %s = %s %s %s; %s' % (d, d, {'faddp': '+', 'fmulp': '*', 'fsubp': '-'}[m], st(i, 0), c))
        else:
            raise Fail('%08X: %s not handled' % (a, m))
    if not func:
        b('    nx = 0x%08Xu;' % end)

    print('/* %s: ColdWarAssault.exe 0x%08X-0x%08X, generated by tools/region_c.py */' % (name, start, end))
    print('#include "ofp_region.h"')
    for x in sorted(decl):
        print(x)
    print('#ifdef OFP_REGIONS')
    print('%s %s(void) {' % ('void' if func else 'uint32_t', name))
    print('    uint32_t E = g_esp, cf_ = g_cur_func, fa = 0, fb = 0, fr = 0, fc = 0;%s' % ('' if func else ' uint32_t nx;'))
    print('    double fca = 0, fcb = 0; (void)fca; (void)fcb;')
    print('    uint32_t %s;' % ', '.join('%s = g_%s' % (r, r) for r in REGS))
    print('    double %s;' % ', '.join('x%d = 0' % n for n in range(8)))
    for off in sorted(slots):
        print('    %s %s = %s(E + %d);' % (cty(slots[off]), sname(off), MEMN[slots[off]], off))
    print('    (void)fa; (void)fb; (void)fr; (void)fc;')
    for line in body:
        print(line)
    if not func:
        for off in sorted(slots):
            if sname(off) in written:
                print('    %s(E + %d) = %s;' % (MEMN[slots[off]], off, sname(off)))
        print('    REGS_OUT(E);')
        print('    return nx;')
    print('}')
    print('#else')
    print('%s %s(void) { %s} /* no x87 ring in this build: never called */' % ('void' if func else 'uint32_t', name, '' if func else 'return 0; '))
    print('#endif')
    sys.stderr.write('%s: %d instructions, %d slots in variables, %d in memory, %d calls (%d return a float)\n'
                     % (name, len(ins), len(slots), len(mem), sum(1 for i in ins if i.mnemonic == 'call'), len(floatret)))


decl = set()
if __name__ == '__main__':
    try:
        main()
    except Fail as e:
        sys.exit('region_c: %s' % e)
