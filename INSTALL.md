# Installing OFP Recomp

You build the program once from your own copy of the game, then play. No game
file is in this repository and none is downloaded: the program is made from
your `ColdWarAssault.exe` and runs on your game's data.

Tested: built on Ubuntu 24.04, played on Linux Mint 22 (AMD) and Ubuntu 24.04
(NVIDIA). Other recent x86-64 distributions should work with the same
packages under their own names.

## What you need

- A 64-bit Linux PC, 16 GB of memory recommended (8 GB works with a slower
  build, see step 4), about 10 GB of free disk space.
- **Arma: Cold War Assault 1.99** (Operation Flashpoint: Cold War Crisis as
  re-released by Bohemia Interactive):
  - its data folders: `AddOns`, `BIN`, `Campaigns`, `DTA`, `Missions`,
    `MPMissions`, `SPTemplates`, `Templates`, `Worlds`, and `Products.cfg`;
  - its program, `ColdWarAssault.exe`, version 1.99, PE timestamp
    `0x58BD672D` (6 March 2017). The project was made with the copy inside
    the game's Linux release by Virtual Programming. Copies from the Windows
    releases have not been tried: if their timestamp differs, step 3 stops
    with a message and nothing is built.

## 1. Packages

Ubuntu 24.04, Linux Mint 22, Debian 13:

```sh
sudo apt install git build-essential cmake ninja-build python3-venv pkg-config \
  libsdl2-dev libsdl2-ttf-dev libgl-dev libvulkan-dev \
  libvulkan-memory-allocator-dev libstb-dev fonts-liberation
```

## 2. The code

```sh
git clone https://github.com/neurocontrarian/ofp-recomp
cd ofp-recomp
```

## 3. Preparation (10 to 30 minutes, once)

```sh
tools/prepare.sh /path/to/ColdWarAssault.exe
```

This downloads [pcrecomp](https://github.com/sp00nznet/pcrecomp) at the
commit the project uses and applies `patches/pcrecomp-ofp.patch`, makes a
Python environment (`.venv`, with capstone and pefile), copies your
`ColdWarAssault.exe` to `work/`, finds its functions (`work/analysis/`) and
translates them to C (`src/recomp/gen/`, about 25,000 functions, 230 MB).

## 4. Build

```sh
./build-linux64.sh -j4
```

The program is `build-linux64/ofp`. The build compiles 230 MB of C: 17 minutes
with `-j4` on an i5-13600K, up to an hour on older CPUs. Each job can take
4 GB of memory: use `-j2` with 8 GB.

Faster program (about 6 % less CPU time; needs a CPU from 2013 or later with
AVX2; crash reports say less), in a separate folder:

```sh
cmake -S . -B build-fast -G Ninja -DOFP_FAST_CALLS=ON \
  "-DGEN_OPT=-O2 -march=x86-64-v3 -ffp-contract=off"
cmake --build build-fast -j4
```

`-ffp-contract=off` is required with `-march`: without it the compiler
changes the game's floating-point results.

## 5. The game folder

Make a folder with the game's data and its program, for instance `game/`
inside the project:

```sh
mkdir game
cp -r /path/to/the/game/{AddOns,BIN,Campaigns,DTA,Missions,MPMissions,SPTemplates,Templates,Worlds,Products.cfg} game/
cp work/ColdWarAssault.exe game/
```

(Links instead of copies work too.) Optional: the game's Options screen gets
a "Linux options" button with

```sh
python3 tools/make_resource.py game/BIN/RESOURCE.BIN game/RES/bin/resource.bin
```

## 6. Play

```sh
build-linux64/ofp --run --game game
```

- `--fullscreen` for full screen; F11 switches in the game.
- The first start writes `game/ColdWarAssault.cfg` (1024×768); change the
  resolution and the rest in the game's own Options screen.
- The recomp's own options (antialiasing, vertical sync, Vulkan or OpenGL,
  frames per second counter): Pause key or Ctrl+O in the game. They are saved
  to `Options.txt` next to the program (or the file named by `OFP_OPTIONS`).
- Logs (performance, slow frames, settings) go to `logs/` in the current
  folder, or to the folder named by `OFP_LOG_DIR`.
- Vulkan is used only if it is chosen in the options and the graphics driver
  supports Vulkan 1.3; OpenGL otherwise.

## 7. Another PC (optional)

```sh
sudo apt install pigz mesa-vulkan-drivers
tools/make_package.sh work/pkg
```

`work/pkg/OFP-Recomp.tar.gz` holds the program, the game folder `game/` and
every library the program loads (Mesa's OpenGL and Vulkan drivers included).
Unpack it on the other PC and start `play.sh`: nothing needs installing
there. `README.txt` inside says the rest.

## When something goes wrong

- *"PE timestamp 0x…, this project lifts 0x58BD672D"*: not version 1.99 of
  `ColdWarAssault.exe`, or not the same build of it.
- *"src/recomp/gen is empty"*: step 3 did not finish.
- The build stops with *"Killed"*: out of memory; use fewer jobs (`-j2`).
- The game window stays black or the game quits at once: run it from a
  terminal and keep what it prints, plus the newest file in `logs/`, for a
  bug report.
