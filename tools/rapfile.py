"""Operation Flashpoint's binarized config ("\\0raP", version 4): read into a
tree, write back. The layout is in derap.py's notes.

A class is [name, base, entries]; an entry is one of
  ("class", [name, base, entries])
  ("value", kind, name, value)      kind 0 string, 1 float, 2 int
  ("array", name, items)            items: [(kind, value)], kind 3 a nested list
"""
import struct


class Reader:
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
        s = self.d[self.o:e]
        self.o = e + 1
        return s

    def name(self):
        i = self.cint()
        if i == len(self.names):
            self.names.append(self.asciiz())
        return self.names[i]

    def value(self, kind):
        if kind == 0:
            return self.name()
        if kind in (1, 2):
            v = struct.unpack_from("<f" if kind == 1 else "<i", self.d, self.o)[0]
            self.o += 4
            return v
        if kind == 3:
            return self.items()
        raise ValueError("value kind %d at %#x" % (kind, self.o))

    def items(self):
        out = []
        for _ in range(self.cint()):
            k = self.u8()
            out.append((k, self.value(k)))
        return out

    def body(self, name):
        base = self.asciiz()
        entries = []
        for _ in range(self.cint()):
            t = self.u8()
            if t == 0:
                nm = self.name()
                entries.append(("class", self.body(nm)))
            elif t == 1:
                k = self.u8()
                nm = self.name()
                entries.append(("value", k, nm, self.value(k)))
            elif t == 2:
                nm = self.name()
                entries.append(("array", nm, self.items()))
            else:
                raise ValueError("entry type %d at %#x" % (t, self.o - 1))
        return [name, base, entries]


def read(data):
    """(source name, root class, enums [(name, value)])"""
    if data[:4] != b"\0raP":
        raise ValueError("not a binarized config")
    r = Reader(data)
    r.o = 8
    src = r.name()
    root = r.body(b"")
    enums = []
    if r.o + 4 <= len(data):
        n = struct.unpack_from("<I", data, r.o)[0]
        r.o += 4
        for _ in range(n):
            nm = r.asciiz()
            enums.append((nm, struct.unpack_from("<i", data, r.o)[0]))
            r.o += 4
    return src, root, enums


class Writer:
    def __init__(self):
        self.out, self.names = bytearray(), {}

    def cint(self, v):
        while True:
            b = v & 0x7F
            v >>= 7
            self.out.append(b | (0x80 if v else 0))
            if not v:
                return

    def name(self, s):
        if s in self.names:
            self.cint(self.names[s])
        else:
            self.names[s] = len(self.names)
            self.cint(self.names[s])
            self.out += s + b"\0"

    def value(self, kind, v):
        if kind == 0:
            self.name(v)
        elif kind == 1:
            self.out += struct.pack("<f", v)
        elif kind == 2:
            self.out += struct.pack("<i", v)
        elif kind == 3:
            self.items(v)

    def items(self, items):
        self.cint(len(items))
        for k, v in items:
            self.out.append(k)
            self.value(k, v)

    def body(self, cls):
        _, base, entries = cls
        self.out += base + b"\0"
        self.cint(len(entries))
        for e in entries:
            if e[0] == "class":
                self.out.append(0)
                self.name(e[1][0])
                self.body(e[1])
            elif e[0] == "value":
                _, k, nm, v = e
                self.out += bytes((1, k))
                self.name(nm)
                self.value(k, v)
            else:
                _, nm, items = e
                self.out.append(2)
                self.name(nm)
                self.items(items)


def write(src, root, enums):
    w = Writer()
    w.out += b"\0raP" + struct.pack("<I", 4)
    w.name(src)
    w.body(root)
    w.out += struct.pack("<I", len(enums))
    for nm, v in enums:
        w.out += nm + b"\0" + struct.pack("<i", v)
    return bytes(w.out)


def find(cls, name):
    """the class entry named name directly in cls"""
    for e in cls[2]:
        if e[0] == "class" and e[1][0] == name:
            return e[1]
    raise KeyError(name)
