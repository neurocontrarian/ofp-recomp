# How OFP Recomp is made

A static recompilation built on [pcrecomp](https://github.com/sp00nznet/pcrecomp):
`ColdWarAssault.exe` 1.99 is translated to C (25,619 functions), compiled as a 64-bit Linux
program, and runs on pcrecomp's Win32 layer (`win32hle`) with host code for what the game
needs beyond it. Building: [INSTALL.md](../INSTALL.md).

## Host code

| Piece | Where |
| --- | --- |
| Direct3D 8 → Vulkan 1.3 (VMA) or OpenGL 2.1: fixed-function T&L, multitexture, fog, render targets, MSAA | `src/linux/d3d8*.c` |
| DirectInput 8 (keyboard, mouse) on SDL2 | `src/linux/dinput8.c` |
| Intel JPEG Library 1.5 on stb_image, Immersion IFC22 as an absent device | `src/linux/ijl15.c`, `ifc22.c` |
| Internet server list (the community master server) | `src/linux/ofp_net.c` |
| Performance log (time per import, game code share, slow frames and their causes) | `src/linux/perf.c` |
| Hand-written replacements of a few lifted functions, each checked against the lifted code | `src/linux/ofp_native.c` |

pcrecomp itself is used at upstream `ef3f0da` with `patches/pcrecomp-ofp.patch` on top
(DirectSound 3D, Winsock `select`, a 4 GB address space, an x87 stack kept in locals, faster
indirect calls...). Those changes are offered upstream one by one (`patches/upstream/`).

## Making it fast

The lifted code started at a few frames per second in heavy scenes. What helped most, with
measurements, is in [`plan-acceleration.md`](plan-acceleration.md):

- the x87 stack as a moving pointer and in local variables (+47 % frames);
- a per-call-site cache for indirect calls (C++ virtual calls, about two million a second),
  imports looked up first;
- profile-guided optimisation and `-march=x86-64-v3` on the generated C;
- for the terrain builder, which caused the stutters when turning: hand-written vertex search
  and index-buffer code, and **`tools/region_c.py`**, which translates one hot loop of a
  lifted function again with the function's stack slots and the x87 stack in C variables
  (a slot whose address is taken stays in guest memory). A terrain segment went from 16.9 ms
  to 8.7 ms to build.

Every native replacement has a check mode (`OFP_NATIVE=check`) that runs it next to the
lifted code and compares the results, and `OFP_TRACE_HASH=1` fingerprints every terrain vertex
and face built, so a translated region can be compared with the lifted code over millions of
calls.
