#!/usr/bin/env python3
"""The game's menus with the recomp's options in them: BIN/RESOURCE.BIN with
a "Linux options" button in the Options screen (RscDisplayOptions), idc
107; the host opens its options on that click (ofp_native.c wraps the
screen's button handler). Written binarized (rapfile.py: the text form's
enum block is not one the game parses). The game reads
RES\\bin\\resource.cpp, then RES\\bin\\resource.bin, before bin\\resource.bin.
usage: make_resource.py BIN/RESOURCE.BIN OUT/RES/bin/resource.bin
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rapfile  # noqa: E402


def button():
    entries = [("value", 2, b"idc", 107), ("value", 1, b"x", 0.425), ("value", 1, b"y", 0.9),
               ("value", 1, b"w", 0.25), ("value", 1, b"h", 0.05), ("value", 0, b"text", b"Linux options")]
    return ("class", [b"ButtonRecompOptions", b"RscActiveMenu", entries])


def main():
    src, dst = sys.argv[1], sys.argv[2]
    name, root, enums = rapfile.read(open(src, "rb").read())
    opts = rapfile.find(root, b"RscDisplayOptions")
    for i, e in enumerate(opts[2]):
        if e[0] == "array" and e[1] == b"controls":
            items = [it for it in e[2] if it != (0, b"ButtonRecompOptions")]
            at = items.index((0, b"ButtonClose")) if (0, b"ButtonClose") in items else len(items)
            items.insert(at, (0, b"ButtonRecompOptions"))     # between Credits and Close, as on screen
            opts[2][i] = ("array", b"controls", items)
            break
    else:
        sys.exit("RscDisplayOptions has no controls[]")
    if not any(e[0] == "class" and e[1][0] == b"ButtonRecompOptions" for e in opts[2]):
        opts[2].append(button())
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "wb") as f:
        f.write(rapfile.write(name, root, enums))
    stale = os.path.join(os.path.dirname(dst), "resource.cpp")   # read first: it would win
    if os.path.exists(stale):
        os.remove(stale)


if __name__ == "__main__":
    main()
