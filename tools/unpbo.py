#!/usr/bin/env python3
"""Unpacks an Operation Flashpoint .pbo (uncompressed entries). usage: unpbo.py FILE.pbo OUTDIR"""
import struct, sys, os
d = open(sys.argv[1], 'rb').read(); out = sys.argv[2]; o = 0; ents = []
while True:
    e = d.index(b'\0', o); name = d[o:e].decode('latin1'); o = e + 1
    meth, orig, res, ts, size = struct.unpack_from('<5I', d, o); o += 20
    if meth == 0x56657273:  # 'Vers' + properties
        while True:
            e = d.index(b'\0', o); k = d[o:e]; o = e + 1
            if not k: break
            e = d.index(b'\0', o); o = e + 1
        continue
    if not name: break
    ents.append((name, meth, orig, size))
for name, meth, orig, size in ents:
    p = os.path.join(out, name.replace('\\', '/')); os.makedirs(os.path.dirname(p) or out, exist_ok=True)
    if meth: print("compressed, skipped:", name, meth)
    else: open(p, 'wb').write(d[o:o + size])
    o += size
print(len(ents), "files")
