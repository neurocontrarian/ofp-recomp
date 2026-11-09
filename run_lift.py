#!/usr/bin/env python3
"""Operation Flashpoint / Arma: Cold War Assault 1.99 lift driver.

work/analysis/functions.json (disasm32, seeded with RTTI) -> src/recomp/gen/.

Same shape as tiberiansun-recomp's run_lift.py, which drives pcrecomp's
tools/lift/generate.py + lift32:

* extents found by walking the branches (generate.true_extent);
* branch and call targets outside the catalog become entries, round after
  round, so every RECOMP_ITAIL / RECOMP_CALL can resolve;
* every vtable slot RTTI names is an entry (rtti_seeds.json);
* precise carry for adc/sbb.

    python run_lift.py --all
    python run_lift.py --check     # exit 0 if the lift is current
"""
import argparse
import hashlib
import json
import os
import re
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, 'third_party', 'pcrecomp')), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
from generate import (EXTENT_REACH, closure, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402

EXE = os.path.join(_HERE, 'work', 'ColdWarAssault.exe')
EXE_STAMP = 0x58BD672D   # ColdWarAssault.exe 1.99 (INSTALL.md)
CATALOG = os.path.join(_HERE, 'work', 'analysis', 'functions.json')
SEEDS = os.path.join(_HERE, 'work', 'analysis', 'rtti_seeds.json')
OUT = os.path.join(_HERE, 'src', 'recomp', 'gen')
STATS = os.path.join(_HERE, 'work', 'analysis', 'lift_stats.json')

# Entries the catalog misses, found by runs (fill in as bring-up finds them).
RUN_SEEDS = []

# Functions whose body is the host's: the host defines ofp_hook_XXXXXXXX(void),
# run like an import shim (arguments from g_esp, pops its own return).
HOOKS = set()

# C added after one instruction: fixes the original cannot have, and the entries
# of hot regions translated by tools/region_c.py (src/linux/ofp_region_*.c):
# the region runs natively, then the lifted code goes on at its end.
def region_hook(start, end):
    return ('{ int ofp_region_on(uint32_t); if (ofp_region_on(0x%08Xu)) { uint32_t ofp_region_%08X(void); '
            'RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT(); ofp_region_%08X(); RECOMP_REGS_IN(); _flag_k = FK_NONE; '
            'goto L_%08X; } }' % (start, start, start, end))


PATCHES = {
    0x00672990: region_hook(0x00672996, 0x006734DD),   # sub_00671950: a terrain segment's cells (after the jle)
    0x00672273: region_hook(0x00672277, 0x00672507),   # its normals (cross products of neighbours)
    0x0067373A: region_hook(0x00673741, 0x00673C09),   # its faces by section
}


def apply_patches(body, patches):
    """Add each patch's C after the lines that lift its instruction."""
    for va, c in patches.items():
        tag = '/* 0x%08X:' % va
        if any('L_%s:' % t not in body for t in re.findall(r'goto L_([0-9A-F]{8});', c)):
            continue                            # a body without the label it goes on at (a split entry)
        k = body.find(tag)
        if k != -1:
            s = body.rfind('\n', 0, k) + 1
            e = body.index('\n', k)
            depth = body[s:e].count('{') - body[s:e].count('}')
            while depth > 0:
                s, e = e + 1, body.index('\n', e + 1)
                depth += body[s:e].count('{') - body[s:e].count('}')
            body = body[:e + 1] + '    ' + c + '\n' + body[e + 1:]
    return body


def exe_stamp(path):
    with open(path, 'rb') as f:
        head = f.read(4096)
    nt = int.from_bytes(head[0x3C:0x40], 'little')
    return int.from_bytes(head[nt + 8:nt + 12], 'little')


def lifter_stamp():
    h = hashlib.sha1()
    for p in (os.path.join(_TOOLS, 'lift', 'lift32.py'), os.path.join(_TOOLS, 'lift', 'generate.py'),
              os.path.abspath(__file__)):
        h.update(open(p, 'rb').read().replace(b'\r\n', b'\n'))
    return h.hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=EXE)
    ap.add_argument('--catalog', default=CATALOG)
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--roots', default='', help='comma-separated extra root VAs')
    ap.add_argument('--max', type=int, default=3000, help='closure size cap')
    ap.add_argument('--all', action='store_true', help='lift every function')
    ap.add_argument('--split', type=int, default=400, help='functions per .c file')
    ap.add_argument('--seeds', default=os.path.join(_HERE, 'work', 'analysis', 'run_seeds.json'),
                    help='JSON list of {"address": N} found by runs')
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    if args.check:
        try:
            sys.exit(0 if open(os.path.join(args.out, 'lift_stamp.txt')).read().strip() == lifter_stamp() else 1)
        except OSError:
            sys.exit(1)

    stamp = exe_stamp(args.exe)
    if stamp != EXE_STAMP:
        sys.exit('%s: PE timestamp 0x%08X, this project lifts 0x%08X' % (args.exe, stamp, EXE_STAMP))
    info = analyze_pe(args.exe)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    print('[*] base=0x%08X code=0x%08X-0x%08X IAT=%d' % (info.image_base, cs, ce, len(iat)))

    cat = json.load(open(args.catalog))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce}
    print('[*] catalog: %d functions inside .text' % len(byaddr))

    want = {a for a in RUN_SEEDS if cs <= a < ce and a not in byaddr}
    for path in [SEEDS, args.seeds]:
        if path and os.path.exists(path):
            want |= {e['address'] for e in json.load(open(path))
                     if cs <= e['address'] < ce and e['address'] not in byaddr}
    known = sorted(set(byaddr) | want)
    nxt = {a: (known[i + 1] if i + 1 < len(known) else ce) for i, a in enumerate(known)}
    for a in want:
        byaddr[a] = {'address': a, 'end': min(nxt[a], a + 0x100), 'calls_to': [], 'entry_kind': 'start'}
    print('[*] seeds not in the catalog: %d injected' % len(want))

    entry = info.image_base + info.entry_point_rva
    roots = [entry] + [int(x, 0) for x in args.roots.split(',') if x.strip()]
    chosen = set(byaddr) if args.all else set(closure(byaddr, roots, args.max))
    print('[*] lifting %d of %d functions' % (len(chosen), len(byaddr)))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    text = [s for s in info.sections if s.name == '.text'][0]
    code = open(args.exe, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]

    lifter = Lifter(iat_map=iat, lifted=set(byaddr), precise_carry=True, precise_sbb=True)
    os.makedirs(args.out, exist_ok=True)
    for fn in os.listdir(args.out):
        if fn.startswith('recomp_') and fn.endswith('.c'):
            os.remove(os.path.join(args.out, fn))
    entries, chunk, idx, errors, dirty = [], [], 0, 0, 0
    ordered = sorted(byaddr)
    starts = {a for a in ordered if byaddr[a].get('entry_kind') != 'alias'}
    splits = find_splits(md, code, cs, ce, starts)
    starts -= splits
    print('[*] split entries: %d' % len(splits))
    t0 = time.time()

    def flush(force=False):
        nonlocal chunk, idx
        if chunk and (force or len(chunk) >= args.split):
            write_chunk(args.out, idx, chunk)
            idx += 1
            chunk = []

    def lift_one(addr, name, end, reached):
        nonlocal errors
        if addr in HOOKS:
            chunk.append(('extern void ofp_hook_%08X(void);\nvoid %s(void) { ofp_hook_%08X(); }\n'
                          % (addr, name, addr), addr, name))
            entries.append((addr, name))
            return
        try:
            lo = min(reached) if reached else addr
            insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                              if end > addr else ([], None))
            if leaders is not None:
                leaders.add(addr)
            body = (lift_function_linear(lifter, name, insns, leaders, addr) if insns
                    else 'void %s(void) { }\n' % name)
            body = apply_patches(body, {va: c for va, c in PATCHES.items() if reached and va in reached})
        except Exception as e:                      # noqa: BLE001 -- counted, not hidden
            body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
            errors += 1
        chunk.append((body, addr, name))
        entries.append((addr, name))
        if len(chunk) >= args.split:
            flush()
            print('[*]   %d/%d (%d err)' % (len(entries), len(chosen), errors), flush=True)

    todo, added = sorted(chosen), 0
    while todo:
        outside = set()
        for addr in todo:
            name = 'sub_%08X' % addr
            reached, behind, called = set(), set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind, called=called)
            outside |= {t for t in behind if t not in reached} | called
            dirty += not clean
            lift_one(addr, name, end, reached)
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t, 'end': ce, 'calls_to': [], 'entry_kind': 'start'}
            chosen.add(t)
        added += len(todo)
    print('[*] branch and call targets outside the catalog, added: %d' % added)

    stubs = [a for a in ordered if a not in chosen]
    for a in stubs:
        name = 'sub_%08X' % a
        chunk.append(('void %s(void) { RECOMP_NOT_LIFTED(0x%08Xu); }\n' % (name, a), a, name))
        entries.append((a, name))
        flush()
    flush(force=True)

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* ColdWarAssault.exe - AUTO-GENERATED by run_lift.py */\n#pragma once\n'
                '#include <stdint.h>\n\n'
                'void recomp_not_lifted(uint32_t va);\n'
                '#define RECOMP_NOT_LIFTED(va) recomp_not_lifted(va)\n\n')
        for a, n in entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* ColdWarAssault.exe - AUTO-GENERATED by run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n'
                'const uint32_t ofp_entry_va = 0x%08Xu;\n' % (len(entries), entry))

    try:
        import name_lift
    except ImportError:
        pass
    else:
        rtti_path = os.path.join(os.path.dirname(SEEDS), 'rtti.json')
        rtti = json.load(open(rtti_path)) if os.path.exists(rtti_path) else {}
        named = name_lift.name_lift(args.out, name_lift.Image(args.exe), rtti, {})
        print('[*] names: %d of %d functions named' % (sum(1 for f in named.values() if f.name), len(named)))

    lines = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8', errors='replace'))
                for fn in os.listdir(args.out))
    stats = {'lifted': len(chosen), 'stubs': len(stubs), 'errors': errors,
             'no_terminator': dirty, 'files': idx, 'lines': lines}
    json.dump(stats, open(STATS, 'w'), indent=1)
    open(os.path.join(args.out, 'lift_stamp.txt'), 'w').write(lifter_stamp() + '\n')
    print('=' * 60)
    print('  lifted %d   not-lifted stubs %d   errors %d   no terminator %d'
          % (len(chosen), len(stubs), errors, dirty))
    print('  %s lines of C in %d files, %.1fs' % (format(lines, ','), idx, time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
