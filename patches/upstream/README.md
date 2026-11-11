# Proposals for pcrecomp (sp00nznet/pcrecomp)

Prepared on 2026-11-07 (4 and 6 rewritten on 2026-11-08) on top of upstream `2adc02f` (#165), still its head on 2026-11-08. One branch per
patch, author neurocontrarian. **Sent on 2026-11-08** from the fork neurocontrarian/pcrecomp:
PRs #168 (7), #169 (1), #170 (5), #171 (8), #172 (2), #173 (6), #174 (4), #175 (9), #176 (3),
and the offer issue #177. The offered host files are also under MIT (LICENSE-MIT.md).

**2026-11-10**: #168-#175 merged upstream (91998d6). **2026-11-11**: #176
rebased on main (93a7432) with the review fixes (listener keeps its own
reference, a 3D interface holds its buffer, no pan on a 3D buffer) and a
`dsound_selftest.c`; 15/15 selftests in both modes. #177 has no answer yet.

Optional follow-ups the maintainer suggested in his merge comments (not done):
- #169: `WaitForMultipleObjects` wait-all still returns WAIT_FAILED when a
  handle is closed mid-wait; a selftest for VirtualFree(MEM_RELEASE) inside a
  reservation.
- #170: with `hle_vm_any_hi` raised, GetSystemInfo should report the higher
  top address.
- #171: check the unconditional `SDL_vulkan.h` include still builds in the
  Emscripten host (#148).
- #172: clamp `select`'s timeout (huge tv_sec overflows; negative → 0); a
  UDP-pair selftest.
- #173: a shim that throws leaves `hle_in_shim_main` set (profiling only).
- #174: say in a comment/README that "imports first" is always on in win32hle
  and that the caches assume `recomp_lookup_manual` answers never change.
- #175: make `RECOMP_X87_RING` a build error outside win32hle (native32,
  bridge, dos32 still use `g_st`/`g_fp_top`); a ring selftest across both
  buffer ends plus the locals path.

| # | Branch / patch | What it does |
|---|---|---|
| 1 | `win32hle-virtualfree-wait` | VirtualFree(MEM_RELEASE) only on a whole reservation; a wait survives its handle being closed |
| 2 | `win32hle-winsock-select` | Winsock `select` (ordinal 18) |
| 3 | `win32hle-dsound3d` | DirectSound 3D (buffer + listener) and DirectSound 8 |
| 4 | `recomp32-resolve-cache` | faster indirect calls: opt-in caches (global, per site), imports first, optional trace ring |
| 5 | `win32hle-address-space` | hosts can give the guest 4 GB (`hle_vm_any_hi`); allocation-failure hook |
| 6 | `win32hle-import-timing` | per-import time on the main thread, for a host's profiler (TSC clock) |
| 7 | `win32hle-winsock-broadcast-hook` | hook after a guest's UDP broadcast (OFP's Internet server list) |
| 8 | `win32hle-screen-gl` | a window for a host's own OpenGL or Vulkan renderer |
| 9 | `recomp32-x87-ring` | x87 stack as a moving pointer, and in locals (opt-in, +47 % frames in OFP) |

Everything is opt-in or a fix: with no new define and no hook set, existing
builds behave as before. Everything that was tested in the game is offered
and the maintainer chooses; what was not tested is said plainly (network: only the server list was shown, no game was joined).

Each patch adds its own CHANGELOG entry (Fixed or Added under [Unreleased]).
If one is merged first, the others may need a trivial CHANGELOG rebase.

## Before sending

1. `git clone https://github.com/sp00nznet/pcrecomp` and check that `main`
   has not moved under the patches: `git am patches/upstream/0N-*.patch` on a
   branch per patch (rebase if needed).
2. Run the runtime's selftests on each branch:
   `runtime/win32hle/build_selftests.sh`. It needs the 32-bit SDL2 and
   SDL2_ttf dev packages, which were missing on the first PC, so only
   compile checks (32-bit flat and 64-bit based) were done there.
   **Done 2026-11-08** (Ubuntu server, in a throwaway ubuntu:24.04 container
   with gcc-multilib and SDL2/SDL2_ttf dev for i386 and amd64): every branch
   applies cleanly on `2adc02f` and passes 12/12 selftests in both modes
   (32-bit flat; 64-bit based with HLE_MEM_BASE=0x100000000000ull), same as
   `main`.
3. Patch 4 (rewritten 2026-11-08: site cache, no trace ring, imports first,
   all measured in OFP): the code OFP runs is the same logic, with
   `recomp_resolve` and the import test in host_lite.c instead of the header
   and a function pointer upstream. **Done 2026-11-08**: both rewritten
   branches pass the selftests (12/12, 32 and 64 bits) with and without
   `-DRECOMP_RESOLVE_CACHE=1024 -DRECOMP_ICALL_SITE_CACHE -DRECOMP_NO_ICALL_TRACE`,
   and a lifted-style `RECOMP_ICALL`/`RECOMP_ITAIL` compiles with each set
   (-m32; -m64 based; -m64 based with RECOMP_LOCAL_REGS). Patch 6 (rewritten the same day): the TSC clock, as OFP runs it.
4. Fork to neurocontrarian (`gh repo fork
   sp00nznet/pcrecomp --clone=false`), push the branches to the fork, then
   `gh pr create -R sp00nznet/pcrecomp --head neurocontrarian:<branch>` with
   the texts below. Send 1-4 as separate PRs, smallest first.
5. Afterwards, OFP should move to upstream's versions as they are merged, so
   that `patches/pcrecomp-ofp.patch` shrinks.

Not a patch: the host-side DLLs written for OFP live in this repo and would
need moving into win32hle. They are offered in the issue below (send it with
the PRs), for the maintainer to say whether and how he wants them.
`runtime/win32hle/module.c`'s OFP-only line (d3d8, dinput8, ijl15, ifc22 as
system DLLs) stays local.

## PR texts

### 1. win32hle: VirtualFree releases only a whole reservation; a wait survives its handle being closed

Two kernel32 fixes found with Operation Flashpoint: Cold War Assault (a
native Linux build on win32hle).

- `VirtualFree(addr, size, MEM_RELEASE)` released the whole reservation that
  contains `addr`. Windows fails the call with `ERROR_INVALID_PARAMETER`
  unless `addr` is the reservation's base and `size` is 0. OFP calls it on
  blocks inside its own memory pool and relies on the failure; the pool was
  released under it.
- `hle_wait`: a handle closed by another thread while this one waits on it
  made the wait fail. On Windows the wait holds the object and wakes when it
  is signalled. OFP closes an event its watchdog thread waits on when it
  exits. A handle that becomes invalid after the first poll now counts as
  signalled; one that is invalid on the first poll still fails.

Tested: OFP on Linux (32-bit flat and 64-bit based builds), and compile
checks of kernel32.c in both modes.

### 2. win32hle: Winsock select

`select(nfds, read, write, except, timeout)`, wsock32 ordinal 18, through
`poll()`. Winsock's `fd_set` is a count and an array of up to 64 SOCKETs (not
a bitmap) and `timeval` two 32-bit longs; a NULL timeout waits for ever. Each
set is rewritten to hold only its ready sockets and the total is returned. A
wait gives the machine to the other guest threads
(`hle_block_begin`/`hle_block_end`).

Operation Flashpoint's multiplayer screen calls it (it stopped on the missing
import). Tested with OFP's LAN and Internet server lists on Linux: the lists
show, with ping and players. Joining or hosting a game has not been tried.

### 3. win32hle: DirectSound 3D and DirectSound 8

- `IDirectSound3DBuffer` (QueryInterface on a `DSBCAPS_CTRL3D` buffer) and
  `IDirectSound3DListener` (one listener, from the primary buffer).
- The mix applies inverse distance past the minimum distance, held at the
  maximum and scaled by the listener's roll-off factor, and pans by the side
  of the listener the sound is on (right = top x front: DirectSound is
  left-handed). `DS3DMODE_NORMAL` and `DS3DMODE_HEADRELATIVE`;
  `DS3DMODE_DISABLE` plays the buffer as a 2D sound. Cones, velocity and
  Doppler are stored or ignored.
- DirectSound 8: `DirectSoundCreate8`, `CLSID_DirectSound8`,
  `IDirectSound8::VerifyCertification`, and `IDirectSoundBuffer8`'s `SetFX`
  (`DSERR_CONTROLUNAVAIL`), `AcquireResources` and `GetObjectInPath`.

Operation Flashpoint positions its sounds with it. Tested by playing OFP on
Linux.

### 4. recomp32: faster indirect calls (opt-in caches, imports first, no trace ring)

`RECOMP_ICALL` resolves every target with `recomp_lookup_manual`, then a
binary search of the dispatch table, then the imports. `recomp_resolve` does
the same, with three additions for hosts where one thread at a time runs
lifted code (win32hle's machine lock):

- `RECOMP_RESOLVE_CACHE` (a power of two): a direct-mapped cache of the
  lifted answers in front of the manual and binary lookups.
- `recomp_is_import`, a host's test for its import VAs (win32hle sets it):
  those go straight to `recomp_lookup_import`, before the search of the
  lifted functions that cannot hold them, and are never cached, as their
  lookup names the shim about to run.
- `RECOMP_ICALL_SITE_CACHE`: each indirect call site keeps the last lifted
  target it resolved (one 64-bit word: the VA and the function's offset from
  `recomp_resolve`), so the common case is a compare. GCC/Clang only.

`RECOMP_NO_ICALL_TRACE` leaves out the indirect-call trace ring (two stores
and two counters a call); a crash report then has no ring. Without the
defines and without `recomp_is_import`, the lookups are as before.

Operation Flashpoint makes about two million indirect calls a second (C++
virtual calls):
- with a 16384-entry cache, its start-up loading stalls went from 3.8 s to
  3.0 s;
- the site cache with no trace ring took 4.2 % off its game code cycles per
  frame (fixed bench, three runs each);
- imports first took `recomp_resolve` from 4.1 % of the main thread to 0.5 %
  in a Direct3D-heavy scene.

Tested: OFP on Linux (64-bit based, all of them on), the win32hle selftests
with and without the defines, and a lifted-style function using
`RECOMP_ICALL`/`RECOMP_ITAIL` compiled with each set.

### 5. win32hle: hosts can raise the guest's address space to 4 GB, and hear about allocation failures

- `hle_vm_any_hi` is where VirtualAlloc's "anywhere" reservations stop:
  0x7F000000 by default, unchanged. A based host (`RECOMP_MEM_BASE`) can
  raise it up to 0xFFF00000, the 4 GB a large-address-aware program gets on
  a 64-bit Windows.
- `hle_on_alloc_fail(what, n)`, when set, is called when the guest heap or a
  page reservation fails, so a host can log it.

Operation Flashpoint at long view distances ran out of 2 GB ("Out of reserved
memory"). Tested: OFP's 64-bit based build with 4 GB, played on Linux Mint.

### 6. win32hle: per-import timing on the main thread, for a host's profiler

With `hle_shim_timing` set, `shim_entry` adds each import's inclusive time
to `hle_shim_ns_main[i]` when it runs on the thread that called
`hle_mark_main_thread`. It also keeps `hle_in_shim_main` at the import that
thread is in (-1 in lifted code), so a sampling thread can tell game code
from time spent in the runtime. `hle_shim_total`/`hle_shim_name_at` name the
slots; `hle_mono_ns` is the clock for longer spans.

The clock read twice a call is `hle_ticks`: the CPU's time-stamp counter
where it runs at a constant rate (`constant_tsc` and `nonstop_tsc`), scaled
to ns by `hle_tsc_calibrate` (a host calls it at start; it waits 20 ms, and
later calls refine the scale), CLOCK_MONOTONIC elsewhere. In Operation
Flashpoint it took 1.6 % less of the main thread than CLOCK_MONOTONIC (three
runs each, same build), with the same times measured.

Off by default. Operation Flashpoint's performance log uses it (time per
import, share of game code, what the main thread was doing during a freeze).
It found most of that port's slowdowns.

### 7. win32hle: a hook after a guest's UDP broadcast

`hle_ws_broadcast_hook(fd, buf, len, port)`, when set, is called after a
successful `sendto` to 255.255.255.255 on a UDP socket (not IPX), with the
host socket, so a host can send the same datagram to other addresses.

Operation Flashpoint searches for LAN games by broadcast. Its host also sends
that search to the Internet servers listed by the community master server,
and their answers fill the game's own server list. Tested: the list shows
Internet servers with ping and players. Joining one this way has not been
tried.

### 8. win32hle: a window for a host's own OpenGL or Vulkan renderer

`hle_screen_gl`, set before `hle_screen_open`: 1 makes the window for
OpenGL, 2 for Vulkan, without an SDL renderer. The system cursor is hidden
and the guest's cursor moves go through `hle_cursor_hook`, as with a
DirectDraw frame. The host draws into `hle_screen_sdl_window()` and reports
where its picture is with `hle_screen_gl_picture(src_w, src_h, x, y, w, h)`,
so mouse positions map back to the game's coordinates (in drawable pixels,
so high-DPI windows map right). Without `hle_screen_gl`, nothing changes.

Operation Flashpoint's Direct3D 8 layer, on OpenGL and on Vulkan, draws
through it (windowed and full screen, on Linux Mint with an AMD card).

### 9. recomp32: x87 stack as a moving pointer, and in locals (opt-in)

- `RECOMP_X87_RING`: st(i) is `g_stp[i]`, a pointer into a 4096-entry
  buffer (and `g_sti64p` for the integer shadows). A push moves the pointer
  down one, a pop up one, as the FPU's TOP does. When it nears an end, it is
  moved back to the middle with the eight live values
  (`recomp_x87_recentre`, host_lite.c). `fp_push_impl`'s eight-register
  shuffle was most of the cost of x87-heavy code: sixteen moves a push or
  pop, through globals that any guest memory store may alias.
- `RECOMP_X87_LOCAL` (with `RECOMP_X87_RING`, `RECOMP_LOCAL_REGS` and
  generated code): the values a function pushes stay in locals; what lies
  below them stays on the ring. `RECOMP_REGS_OUT` writes them to the ring at
  every call, tail call and return, so callees, float returns and `__ftol`
  see the stack as before. A caught throw clears them, and the yield hook
  keeps them. `_xk` only changes by constants, so the optimiser keeps the
  values in host registers.
- generate.py writes x87 slots as `_ST(i)`/`_STI(i)`. Without the defines
  these are `_st[i]`/`g_st_i64[i]`, so existing builds are unchanged.
  `RECOMP_X87_LOCAL` uses a GCC/Clang statement expression.

Operation Flashpoint: the ring alone gave +47 % frames and 28 % shorter
loading stalls (llvmpipe, same scene). Its native Linux build has run both
for days of play. Compile-checked here: an OFP generated file with neither,
the ring, and both.

## Issue text: host-side DLLs from the Operation Flashpoint port, if wanted

Title: Operation Flashpoint port: a Direct3D 8 layer (OpenGL and Vulkan) and
a few DLLs, if you want them in win32hle

Operation Flashpoint: Cold War Assault now runs natively on Linux on
pcrecomp (lift32 + win32hle, 64-bit based build), with these host-side
pieces that are not game-specific. They live in the port's repository
(https://github.com/neurocontrarian/ofp-recomp, `src/linux/`):

- **Direct3D 8** (`d3d8.c` front end, `d3d8_gl.c` OpenGL 2.1, `d3d8_vk.c`
  Vulkan 1.3 with VMA; about 3,800 lines): fixed-function T&L, multitexture
  stages, fog, render targets, MSAA, dynamic vertex and index buffers. On
  Vulkan, OFP draws 3,000+ calls a frame at about 1 µs each.
- **DirectInput 8** (`dinput8.c`, 357 lines): the keyboard and the mouse on
  SDL2's state, immediate and buffered reads; no joystick. This may overlap
  #159.
- **IJL 1.5** (`ijl15.c`): Intel's JPEG library on stb_image.
- **IFC22** (`ifc22.c`): Immersion's force-feedback mouse classes, as a
  device that is not there.

The Direct3D layer draws through the GL/Vulkan window of PR #<8>, and it
overlaps #163 (Direct3D on Metal), so a common front end might make sense.
Also in that repository, in case it is useful to pcrecomp: `tools/region_c.py`
translates one hot loop of a lifted function (one entry, one exit) again,
with the function's stack slots and the x87 stack in C variables (a slot whose
address is taken stays in memory), and the lift calls it through a patch at
the loop's entry. Most of what made OFP's terrain building slow was every
`[esp+X]` being a guest memory access the compiler cannot keep in a register.
On three loops it cut a terrain segment's build from 13.2 ms to 11.5 ms, with
the same vertices and faces produced (checked by a fingerprint over 35 million
calls). On small functions without loops it gained nothing.

I would be glad to move any of these into `runtime/win32hle` in the shape you
prefer. Or you can leave them where they are if they do not fit.
