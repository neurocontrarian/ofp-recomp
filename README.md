# OFP Recomp: Operation Flashpoint on Linux, natively

Play **Arma: Cold War Assault 1.99** (Operation Flashpoint: Cold War Crisis, as re-released by
Bohemia Interactive) as a real Linux program: no Wine, no emulator. The game's own program is
translated and rebuilt for Linux, then runs with your copy of the game.

**No game file is in this repository.** You need your own copy of the game.

## What you get over the original

- **Native Linux, 64-bit.** The game gets a full 4 GB of memory of its own.
- **No more "Out of reserved memory".** The original stops at 1 GB of working memory, which
  long view distances run out of; here it is 1.5 GB by default, up to 2 GB.
- **Vulkan or OpenGL** instead of Direct3D 8, switched from inside the game.
- **Antialiasing** 2x, 4x or 8x for smooth edges.
- **Vertical sync** on or off, and a frames-per-second counter.
- **In-game options** for all of the above: Pause key or Ctrl+O, or the "Linux options"
  button in the game's Options screen. They are kept in `Options.txt`.
- **The Internet server list is back.** GameSpy closed in 2014; the Multiplayer list now shows
  the 1.99 servers of the community master server, with ping and players.
- **Maximum view distance and terrain detail are playable**, with only rare short stutters
  when turning.
- **Take it to another PC:** one self-contained folder (game, program, graphics drivers),
  nothing to install there.
- **Logs for bug reports:** frame rate, slow frames and their causes, settings.

## Performance

On a 2017 desktop (Core i5-8600K, Radeon RX 5700 XT, Linux Mint 22): about 60 fps most of the
time at the longest view distance and finest terrain, 35 to 45 fps in the busiest scenes.

## State (November 2026)

- Menus, single missions and campaign missions run.
- Multiplayer: the LAN and Internet server lists work; joining or hosting a game has not been
  tried yet.
- Like the original, the game itself runs on one processor core; that core is what limits the
  frame rate in busy scenes. Moving the drawing to a second core is planned.
- Played on Linux Mint 22 (AMD graphics, Vulkan and OpenGL) and Ubuntu 24.04 (NVIDIA).

## What you need

- A 64-bit Linux PC with Vulkan 1.3 or OpenGL 2.1 graphics; 16 GB of memory recommended for
  the build (8 GB works, slower).
- Your copy of Arma: Cold War Assault 1.99, with `ColdWarAssault.exe` 1.99 (see
  [INSTALL.md](INSTALL.md) for the exact version).

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

## For developers

How it is made (pcrecomp, the host code for Direct3D 8, DirectInput, sound and network, and
the performance work with its measurements): [docs/developers.md](docs/developers.md) and
[docs/plan-acceleration.md](docs/plan-acceleration.md).

## License and thanks

This project is under the [PolyForm Noncommercial License 1.0.0](LICENSE.md): you may use,
change and share it for any noncommercial purpose; commercial use, direct or indirect, is not
allowed. The parts that are not specific to this game (the Direct3D 8, DirectInput and JPEG
layers, the loop translator, and the changes to pcrecomp) are also available under the MIT
license, so that they can go into pcrecomp: the list is in [LICENSE-MIT.md](LICENSE-MIT.md).

The game and its data belong to Bohemia Interactive; none of it is in this repository. Thanks
to sp00nznet for pcrecomp.
