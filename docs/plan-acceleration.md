# Plan: making the game code faster

Written on 2026-11-07, results added up to 2026-11-08.

## In short

In busy scenes the game spends 75 to 85 % of its time in its own computation
(the lifted code), and only 10 to 15 % drawing. That computation is what
brings it down to 40 frames per second.

Rules of the plan:

- Every step is **measured** on a fixed benchmark, before and after.
- **Threshold: at least 10 % less computation time per frame**, otherwise
  the step is dropped (reverted) and the next one is tried.
- Start with what cannot change the game's behaviour (compiler settings),
  then take more and more risk.
- Every winning step gives a package tested on the Linux Mint PC (log:
  "game code" and frames per second in scenes with more than 2000 draws).
- Review after steps 1 and 2: if their combined gain is under 15 %, discuss
  again before going further.

| Step | What | Expected gain | Risk for the game | Effort |
|---|---|---|---|---|
| 0 | Reliable benchmark | none (tool) | none | ½ session |
| 1 | Compiler settings (modern CPU, profile-guided optimisation) | 10-30 % | almost none | 1 session |
| 2 | Remove the debug tracing from the lifted code | 3-10 % | none (less diagnostics) | ½ session |
| 3 | Drawing on a 2nd core | up to 10-15 % in busy scenes | low (not the game logic) | 1-2 sessions |
| 4 | Finer translation (calls, registers) | 10-25 %? | medium | several sessions |
| 5 | Rewrite the slowest functions by hand | low (flat profile) | medium, checked | case by case |

## Setting up a benchmark machine

1. Packages: cmake, ninja, gcc, Xvfb, mesa-vulkan-drivers (lavapipe), python3,
   linux-tools (perf), gdb.
2. Profiling: `sudo sysctl kernel.perf_event_paranoid=1` (perf is refused
   otherwise).
3. Build: `./build-linux64.sh -j<N>`. One build at a time
   (`tools/build_variant.sh`, memory guard `tools/memguard.sh`).
4. Check that the game starts (`tools/run_test.sh`, or for real with a GPU).
   On an NVIDIA machine Vulkan goes through NVIDIA's driver, not lavapipe.

## Step 0: benchmark

Goal: a stable figure (±3 %) of the **game's computation time per frame**.

- Frames per second are not enough: vertical sync caps them and software
  rendering skews them. Measure the main thread's CPU time outside imports
  (the log already gives "game code %"; add a total in ms per frame, and a
  summary line at the end of the run).
- A fixed, busy scene: a single mission started automatically
  (`tools/ui_test.sh`), or the campaign cutscene `anims\intro1.cain`. Fixed
  length (60 s after loading), OFP_VSYNC=0, the player's settings frozen (a
  copy of UserInfo.cfg and ColdWarAssault.cfg in the benchmark).
- Run 3 times and note the spread. Above 3 %, stabilise first (same seed,
  same length, idle machine).
- Fidelity guard: screenshots (OFP_SHOT) at the same moments, compared by eye
  between two versions, plus 5 minutes of real play.

## Step 1: compiler settings (almost no risk)

The lifted code (`src/recomp/gen`, 230 MB of C) is compiled with generic
`-O2`.

1. **Modern CPU**: `-march=x86-64-v3` (AVX2, present on the Mint's i5-8600K
   and on an i5-13600K). **Mandatory: `-ffp-contract=off`**, otherwise the
   compiler fuses multiplications and additions and changes the results of
   floating-point computations (trajectories, physics). Keep
   `-fno-strict-aliasing -fwrapv`.
2. **`-O3`** on the generated code: to be measured (sometimes slower: code
   too big).
3. **Profile-guided optimisation (PGO)**: build with `-fprofile-generate`,
   play the benchmark (and some menu and multiplayer), rebuild with
   `-fprofile-use -fprofile-partial-training`. Often the biggest gain on large,
   branchy code like this. The profiles (.gcda) go into `work/`, not git.
4. LTO: only if the machine's memory allows it. Tried last.

Each option is tried alone, then the best combination is kept. These settings
do not change the game's logic. The only real danger is point 1
(floating-point), hence `-ffp-contract=off`.

## Step 2: remove the debug tracing (no risk for the game)

Every indirect call (about 2 million a second, watchdog counter) writes into a
trace ring and two counters (`RECOMP_ICALL`,
`third_party/pcrecomp/runtime/recomp32/recomp_types.h`). Every call and every
function also update `g_cur_func`.

- Put the tracing behind a build option (on for debugging, off in the
  package).
- `g_cur_func` feeds the log's sampler ("where [...]") and the crash report:
  keep it, or replace it with stack sampling (perf, gdb); to be measured.
- Cache the last resolved target at each indirect call site
  (`recomp_resolve` hashes on every call).
- Runtime changes go into `patches/pcrecomp-ofp.patch` (the runtime is a
  third-party checkout).

## Step 3: drawing on a 2nd core (low risk)

The game uses one core. Drawing (DrawIndexedPrimitive, Present: 11-18 % in
busy scenes) could move to a separate thread: the game thread records the
commands (d3d8.c), a render thread runs them (d3d8_vk.c). The game logic is
not touched. The risks are those of parallelism (buffer locks, textures
changed while being drawn), visible on screen rather than in the game.

## Step 4: finer translation (medium risk)

To be decided on a `perf` profile (step 0), not blindly. Leads:

- Every direct call saves and reloads all registers in global variables
  (`RECOMP_CALL`: REGS_OUT/IN, FLAGS_OUT/IN). Keep them in local variables
  between neighbouring functions.
- CPU flags computed and never read.
- Memory accesses: `ADDR` adds a 64-bit base to each access.

These changes touch the lifter (pcrecomp): regenerate all the code, then
compare on the benchmark (identical frames at the same moments).

## Step 5: hand rewrites (case by case)

Model: `src/linux/ofp_native.c` (sub_00618794, with OFP_NATIVE=check, which
compares the lifted and native versions on every call). The current profile is
flat: the heaviest function (sub_006A2150) weighs about 2 %. A rewrite is only
worth it for a function above 5 % of the time, or a group of related
functions.

## Starting measurements (Mint log of 2026-11-07, package 15, Vulkan)

- 60 fps (vertical sync) most of the time, 38-47 at 2600-3900 draws per
  frame.
- Scenes with more than 2000 draws: "game code" 68 % on average (the rest,
  mostly Present, is waiting for the screen), DrawIndexedPrimitive 11-18 %,
  PeekMessageA 4-7 %.
- Vulkan drawing: about 1 µs per draw.

## Results (Ubuntu machine, i5-13600KF, RTX 3090 on a virtual screen)

### The benchmark (2026-11-08)

`python3 tools/bench_game.py game <bench folder>`, then
`BENCH_GAME=<bench folder> BENCH_SECONDS=170 BENCH_PIN=0-11 tools/bench.sh OUT build-X/ofp`.

Figure kept: **millions of CPU cycles spent in the game's code per frame**
(main thread counter, minus the cycles of system calls). Time in ms depends on
the CPU frequency (power saving: ±7 %). Three traps found on the way, each
gave 10 to 20 % of spread between runs, more than the gains looked for:

1. the original mission 01 places the player at random → fixed mission
   "00 Bench";
2. enemy soldiers (random AI) → a mission with no other soldiers;
3. **a free frame rate**: the game's work per frame depends on the time since
   the previous frame, so on a rate that varies with the display → rate fixed
   at 25 fps (`OFP_FPS_CAP`).

With the three fixes: ±1.5 to 2.5 % between runs.

### Step 1: compiler settings

| Version | Mcycles/frame (3 runs) | vs reference |
|---|---|---|
| reference `-O2` | 30.83 · 30.67 · 31.10 | — |
| `-O2 -march=x86-64-v3 -ffp-contract=off` | 30.37 · 29.64 · 30.42 | −2.3 % |
| same + profile-guided optimisation (PGO) | 29.33 · 29.13 · 29.79 | −4.7 % |
| `-O3` (alone) | 30.53 · 30.69 · 30.04 (ref. 30.92 · 30.73 · 30.40) | −0.9 % |

The first figures (−9 % and −14 %), measured without a fixed rate, were
wrong. PGO profile: the "00 Bench" mission with the soldiers, 240 s
(`-fprofile-generate=<folder>`, linked with `-Wl,-u,__gcov_dump`; the game
writes the profile when it quits, perf.c).

### Step 2: indirect calls (`-DOFP_FAST_CALLS=ON`)

No trace ring, and a cache of the last target per call site (never for a
system call: their common entry point reads which function is asked for;
caching them made startup fail, R6008).

| Version | Mcycles/frame | Difference |
|---|---|---|
| reference | 30.83 · 30.33 · 30.58 | — |
| step 2 | 29.77 · 30.18 · 27.96 | −4.2 % |

### `perf` profile of the reference (main thread, user-mode cycles)

Lifted code: flat profile, no function above 2 %. Technical items: reading
the clock (vdso) ~8 %, largely the timing of system calls (`hle_shim_timing`,
two reads per call, on in the package); `recomp_resolve` 5.8 % (step 2);
`memmove`/`memset` 6.6 %; NVIDIA driver 16.6 %. Risk-free lead: timing with
`rdtsc` instead of `clock_gettime`.

### Lead 1: timing system calls with the CPU counter

The package times every system call (two clock reads per call). The profile
gave ~7 % to reading the clock, but only a third of it comes from that timing:
the rest is read by the NVIDIA driver. Replaced with `rdtsc` (`hle_ticks`,
calibrated against CLOCK_MONOTONIC at startup then every second; `OFP_TSC=0`
keeps the old clock for comparison). Same build (build-combo), 3 runs each,
cycles of the whole main thread per frame:

| | run 1 | 2 | 3 | mean |
|---|---|---|---|---|
| system clock | 64.87 | 64.40 | 65.00 | 64.76 |
| CPU counter | 64.25 | 63.50 | 63.51 | 63.75 (−1.6 %) |

The measured times stay the same (game 8.2 ms per frame in both cases): the
conversion is right. A small gain, but free.

### Stutters when turning (Mint log of 2026-11-08, package 17)

Not the shaders: none of the slow frames in play creates a pipeline. It is the
game's own code (60 to 90 % of the slow frame, no file read): when an object
comes into view for the first time, the game builds its 3D model. During those
seconds the main thread is mostly in `sub_00671950` (5,000 lines, building a
model: 15-30 %), `sub_00618959`, `sub_006A2660`, `sub_005B6637` and
`sub_00618794` (already native). Target of step 5, measured on those frames
(the current benchmark produces none: a benchmark that discovers objects is
needed). Apart from that: above ~4,000 draws per frame, the Mint drops to
30-50 fps, CPU-bound (game code ~80 %): steps 3 and 4.

### "Fly-over" benchmark and terrain building (2026-11-08)

`BENCH_PATH=discover tools/bench_game.py game <bench folder>`: a fly-over of
Malden from town to town (15 m, 120 m/s, 160 s, a full turn every three
towns), nothing is seen twice. With the Mint's settings
(`BENCH_CFG=work/bench-cfg-mint`, taken from its log: 2,449 m view distance,
grid 3.125, 1920×1080, `OFP_MSAA=8`, `OFP_XVFB_SCREEN=1920x1080x24`,
`BENCH_SECONDS=250 BENCH_WINDOW=170`), the benchmark machine reproduces the
stutters: ~150 frames over 50 ms, up to 300 ms. With the benchmark's default
settings (short view), none.

`perf --call-graph=lbr` profile: 14 % of the main thread under
`sub_00670AE0` → `sub_00671950`, building a terrain segment (8 × 8 cells).
Inside: its own code 30 %, `sub_00618959` 21 %, `sub_0069B8F5` 10 %,
`sub_00609BC7` 7.5 %, `sub_005B6637` 7 %, `sub_00618794` 6 % (already
native).

- The segment cache is not too small: limit ~270-300 for ~80 segments in view
  (new `terrain:` line in the log). Segments unused for 60 s are dropped.
- The game itself lowers the view distance when the frame rate falls under
  the player's "frame rate" setting (30): 2,449 → ~2,050 m on the benchmark
  (25 fps).
- `sub_00618959` (terrain vertex search) rewritten natively, checked over 103
  million calls (`OFP_NATIVE=check`, 0 difference); `OFP_NATIVE_OFF=00618959`
  to compare. 3 fly-overs each: game code in slow frames 11,364 → 11,013 ms
  (−3 %), the rest unchanged. Small gain: the search was not the main cost.
- Terrain detail one notch lower (grid 6.25 instead of 3.125, same fly-over):
  0 frames of 100 ms or more (instead of ~65), game code in slow frames
  11.0 → 3.8 s, 24.8 fps instead of 23.5. The finest grid multiplies the
  building work: that is cause number 1.

### Terrain work, part 1 (2026-11-08)

Native, checked in `check` mode on the fly-over (0 difference):
`sub_00618959` (100 M calls), `sub_005B6637` (adding a face, 53 M calls), and
the game's array `Resize` done in place when the capacity is enough (in the
native vertex additions). `OFP_NATIVE_OFF=00618959,005B6637` to compare.
3 fly-overs each (Mint settings):

| | before | after |
|---|---|---|
| game code in slow frames | 11,823 ms | 9,196 ms (−22 %) |
| frames of 100 ms or more | 70 | 56 |
| game cycles per frame | 63.8 M | 61.0 M (−4 %) |

Left in building a segment (`sub_00671950`, 13 % of the main thread): its
inner loop (0x6729c0-0x6734d7: x87 computations, 8 virtual height calls per
cell) ~31 %, `sub_006A2660` (index buffer, 11 %, min/max loops and fans),
`sub_0060FBEE` (8 %), `sub_006CE940`, `sub_00618E9D`, `sub_0060AA26`.
`sub_00671950` and `sub_006A2660` are called from their own file: `--wrap`
does not catch them, another way is needed (weak definition + alias, in
progress).

### Terrain work, part 2: measurements (2026-11-08, series 16)

3 fly-overs each, game cycles per frame:

| | mean | game code in slow frames |
|---|---|---|
| v3 + fast calls (`build-fc`) | 63.3 M | 9,663 ms |
| + `OFP_FAST_LOOPS` (`build-fl`) | 63.8 M | 9,773 ms |
| combo, index buffer lifted (`OFP_NATIVE_OFF=006A2660`) | 60.1 M | 8,750 ms |
| combo, index buffer native | 61.2 M | 8,908 ms |

Neither `OFP_FAST_LOOPS` nor the native index buffer (identical to the lifted
one, checked) gains measurably: under the noise (±4 % from one fly-over to the
next). `OFP_FAST_LOOPS` was removed (review of 2026-11-08). Noise: the game
adjusts its view distance to the frame rate (player's "frame rate" setting 30,
benchmark capped at 25); the work therefore depends on speed. To be fixed in
the benchmark (`frameRate` under 25 in `work/bench-cfg-mint`) before the next
measurements. The big piece left: the loop of `sub_00671950` (31 % of the
building).

### Terrain work, part 3: loops translated apart (2026-11-08)

`tools/region_c.py` translates a hot loop (one entry, one exit) with the stack
variables and the x87 stack in C variables; `tools/make_regions.sh` writes
`src/linux/ofp_region_*.c`, `tools/patch_gen_regions.py` plugs in the hooks
(`run_lift.py` PATCHES) in the lifted code. Three loops of `sub_00671950`
(cells, normals, faces). Check: `OFP_TRACE_HASH=1` (fingerprint of every
vertex and face built) identical to the lifted code over the first 35 million
calls (the rest depends on the frame rate, even between two identical runs).
Direct measurement (`terrain:` line of the log, fly-over, Mint settings):

| | time to build a terrain segment |
|---|---|
| as package 17 (`OFP_NATIVE_OFF=…`) | 16.9 ms |
| cell loop translated | 11.8 ms (with the first native pieces) |
| + normals and faces | 11.5 ms |
| + vertex search grids kept and larger | 10.7 ms |

That is −37 % per segment since package 17. Mint log with package 18
(different sessions, to be taken with care): 52.9 → 57.8 fps on average,
frames of 100 ms or more 8.8 → 6.3 per minute.

### Whole functions translated: no gain (2026-11-08)

`tools/region_c.py` can also translate a whole function (FRAME `func`, END 0:
the extent is found; internal calls included). Seven hot functions of the
fly-over (3x4 transform `sub_006CED10` written by hand, terrain height
`sub_006777C0`, bounding box `sub_00618D13`, two sort comparisons, cache list,
1/sqrt): identical to the lifted code in check mode (tens of millions of
calls), but **5 % slower** on the village benchmark (3 runs each); profile:
2.27 % against 1.79 % for the transform. On a small function without a loop,
the lifted code is already good, and the dispatch (weak function, mode,
registers passed through the globals) costs more than the little gained.
Removed. The gain is in loops whose stack variables the tool keeps in
registers.

Kept: the cell loop's region with its per-site call cache and the identity
function `TextureD3D__virtual_15` done in place (a terrain segment
10.7 → 9.0 ms), and `recomp_resolve` testing imports first (Direct3D calls:
before, the binary search of the lifted functions came first).

- Native index buffer (`sub_006A2660`), measured by the time to build a
  terrain segment (2 fly-overs each): 9.5 ms lifted, 8.7 ms native (−8 %).
  The per-frame measurement of series 16 did not see it (noise). Kept.
- Per terrain segment (Mint settings, fly-over): 16.9 ms (package 17) →
  8.7 ms (package 20), twice as fast.
