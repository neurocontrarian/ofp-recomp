#!/usr/bin/env python3
"""Operation Flashpoint's binarized config ("\\0raP", version 4) to config text.
usage: derap.py RESOURCE.BIN > resource.cpp

Layout (worked out from BIN/RESOURCE.BIN):
  "\\0raP" u32 4, the source's name (name 0), then the root class body.
  a class body: its base's name (asciiz, empty for none), entry count
  (compressed), entries.
  an entry: u8 type then a name -- 0 class (its body follows), 1 value
  (u8 kind, then the value), 2 array (count, then u8 kind + value each).
  kinds: 0 string (by the name table), 1 float (f32), 2 int (i32), 3 array.
  a name: a compressed index into a table every name is added to the first
  time it appears (inline, asciiz, when the index is the table's length);
  index 0 is the source's name.
  compressed: 7 bits a byte, low first, the top bit set while more follow.
  after the root: the enum table, u32 count then (asciiz name, i32) each.
"""
import struct
import sys


class Rap:
    def __init__(self, data):
        self.d, self.o, self.names = data, 0, []

    def u8(self):
        v = self.d[self.o]
        self.o += 1
        return v

    def cint(self):
        v, sh = 0, 0
        while True:
            b = self.u8()
            v |= (b & 0x7F) << sh
            sh += 7
            if not b & 0x80:
                return v

    def asciiz(self):
        e = self.d.index(b"\0", self.o)
        s = self.d[self.o:e].decode("latin-1")
        self.o = e + 1
        return s

    def name(self):
        i = self.cint()
        if i == len(self.names):
            self.names.append(self.asciiz())
        return self.names[i]

    def value(self, kind):
        if kind == 0:                          # strings go through the name table too
            return '"%s"' % self.name().replace('"', '""')
        if kind == 1:
            v = struct.unpack_from("<f", self.d, self.o)[0]
            self.o += 4
            return repr(float("%.7g" % v))
        if kind == 2:
            v = struct.unpack_from("<i", self.d, self.o)[0]
            self.o += 4
            return str(v)
        if kind == 3:
            return self.array()
        raise ValueError("value kind %d at %#x" % (kind, self.o))

    def array(self):
        n = self.cint()
        return "{" + ",".join(self.value(self.u8()) for _ in range(n)) + "}"

    def body(self, out, depth):
        base = self.asciiz()                   # the base class, by its name (empty: none)
        n = self.cint()
        return base, [self.entry(out, depth) for _ in range(n)]

    def entry(self, out, depth):
        t = self.u8()
        pad = "\t" * depth
        if t == 0:
            nm = self.name()
            sub = []
            base, _ = self.body(sub, depth + 1)
            out.append("%sclass %s%s\n%s{\n" % (pad, nm, " : " + base if base else "", pad))
            out.extend(sub)
            out.append("%s};\n" % pad)
        elif t == 1:
            kind = self.u8()
            nm = self.name()
            out.append("%s%s=%s;\n" % (pad, nm, self.value(kind)))
        elif t == 2:
            nm = self.name()
            out.append("%s%s[]=%s;\n" % (pad, nm, self.array()))
        else:
            raise ValueError("entry type %d at %#x" % (t, self.o - 1))


def main():
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"\0raP":
        sys.exit("not a binarized config")
    r = Rap(data)
    r.o = 8
    r.name()                       # the source file's name: name index 0
    out = []
    _, _ = r.body(out, 0)
    if r.o + 4 <= len(data):                       # the enum table: u32 count, (asciiz, i32) each
        n = struct.unpack_from("<I", data, r.o)[0]
        r.o += 4
        out.append("enum\n{\n")
        for k in range(n):
            nm = r.asciiz()
            v = struct.unpack_from("<i", data, r.o)[0]
            r.o += 4
            out.append("\t%s=%d%s\n" % (nm, v, "," if k + 1 < n else ""))
        out.append("};\n")
    if r.o != len(data):
        sys.stderr.write("note: %d bytes left at the end\n" % (len(data) - r.o))
    sys.stdout.write("".join(out))


if __name__ == "__main__":
    main()
