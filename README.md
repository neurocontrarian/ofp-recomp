# OFP Recomp: Operation Flashpoint / Arma: Cold War Assault on Linux

A static recompilation of **Arma: Cold War Assault 1.99** (Operation Flashpoint: Cold War
Crisis, as re-released by Bohemia Interactive) into a native 64-bit Linux program, built on
[pcrecomp](https://github.com/sp00nznet/pcrecomp). No emulator, no Wine: `ColdWarAssault.exe`
is translated to C (25,665 functions), compiled, and runs on pcrecomp's Win32 layer
(`win32hle`) with host code for what the game needs beyond it.

**No game file is in this repository.** You need your own copy of the game.

## State (November 2026)

- Menus, single missions and campaign missions run. Multiplayer: the LAN and Internet server
  lists show (with ping and players); joining or hosting a game has not been tried.
- On a 2017 desktop CPU (i5-8600K) the game holds about 60 fps at the longest view distance
  and finest terrain grid most of the time; terrain building stutters went from 8.8 to 2.1
  frames of 100 ms or more a minute during the last week's work.
- Played on Linux Mint 22 (AMD GPU, Vulkan and OpenGL); benchmarked on Ubuntu 24.04 (NVIDIA,
  virtual screen).

## What is host code

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
measurements, is in [`docs/plan-acceleration.md`](docs/plan-acceleration.md):

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

## Building and playing

See **[INSTALL.md](INSTALL.md)**. In short, on Ubuntu 24.04 or Linux Mint 22, with your own
`ColdWarAssault.exe` 1.99 and the game's data:

```sh
tools/prepare.sh /path/to/ColdWarAssault.exe   # pcrecomp, analysis, lifted C (10-30 min)
./build-linux64.sh -j4                         # build-linux64/ofp (20-60 min)
build-linux64/ofp --run --game <folder with the game's data and ColdWarAssault.exe>
```

`tools/make_package.sh` makes a self-contained folder (program, data, libraries) for another
Linux PC.

## License and thanks

This project is under the [PolyForm Noncommercial License 1.0.0](LICENSE.md): you may use,
change and share it for any noncommercial purpose; commercial use, direct or indirect, is not
allowed. The patches to pcrecomp (`patches/pcrecomp-ofp.patch` and `patches/upstream/`) are
offered under pcrecomp's own MIT license, so that they can go upstream.

The game and its data belong to Bohemia Interactive; none of it is in this repository. Thanks
to sp00nznet for pcrecomp.
