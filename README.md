# DS2 Photo Mode

A photo mode for Dark Souls II: Scholar of the First Sin (Steam, 64-bit, DX11): a free camera, see-through panels drawn with Dear ImGui, poses for your character, and Snap, which saves a screenshot to your Steam library with the panels hidden. Everything is controller driven (L3+R3 turns it on and off), and works with a keyboard and mouse too (F9). Created by reeleese; the Nexus page has the full description and screenshots.

The mod is one file, `xinput1_3.dll`. This repository is its complete source and the build that produces the released binary.

## Keyboard and mouse

F9 turns photo mode on and off. Mouse: aim. W A S D: move. Q / E: down / up (roll in 6DOF flight). Shift / Ctrl: faster / slower. Mouse wheel: focal length. Up / Down arrows: pick a row; Left / Right: change it. Tab: cycle tabs. Left click, Enter or Space: snap. R or Backspace: reset the camera. The hint lines show keys or pad buttons, whichever you used last. While photo mode is on, the game's DirectInput keyboard and mouse are blanked (`src/kbm.cpp`), so keys don't also act in the game.

## Why it is a DLL named xinput1_3.dll

`DarkSoulsII.exe` imports `XInputGetState` and `XInputSetState` from `xinput1_3.dll`, so a DLL of that name next to the exe is loaded by the game itself. It exports the same functions (`src/xinput1_3.def`) and passes them on to the system's XInput, so the pad works as normal. Being the game's XInput also lets it hand the game a neutral pad while photo mode is on. It then hooks two things with MinHook: the DXGI swap chain's `Present` (to draw the panels, `src/overlay.cpp`) and the game's camera update (`src/camera.cpp`). Such a proxy DLL with hooks is what antivirus heuristics dislike, which is why the build below is reproducible: you can check the released file is exactly this source.

It writes two small files: `photo-mode.log` in `%LOCALAPPDATA%\ds2-photo-mode\`, and `photo-mode.ini` beside the DLL, which holds the date photo mode was last opened (it decides when the start-up reminder shows).

## Build

Linux, bash and [uv](https://docs.astral.sh/uv/). No root; uv downloads the compiler (`ziglang==0.17.0` from PyPI, zig's clang) on the first run.

    ./build.sh my-build        # -> ~/.cache/ds2-photo-mode/my-build/xinput1_3.dll
    tests/run.sh               # native checks of the maths

`PM_BUILD_ROOT` moves the output. `--dev` adds `src/devchan.cpp`, a file-based debug command channel that is never part of a release.

## Verify the DLL

Each release lists the sha256 of its `xinput1_3.dll` (`SHA256SUMS` in the GitHub release, built by the workflow in `.github/workflows/build.yml` from the tag). The build is reproducible: the same source gives the same bytes, whatever the folder or the time.

    git checkout v1.1.0        # the tag of the release you have
    ./verify.sh                # builds twice in two folders, prints the sha256
    sha256sum path/to/xinput1_3.dll

The two hashes should match. `./verify.sh EXPECTED_SHA256` does the comparison for you. This applies from release 1.1.0; the 1.0 binary was built before the build was made reproducible. The toolchain is fixed: ziglang 0.17.0 (target `x86_64-windows-gnu`), plus `normalize_pe.py`, a short script that zeroes the link timestamp and replaces the linker's random debug GUID with a hash of the file (those were the only bytes that differed between builds).

## Layout

- `src/`: the mod. `camera.cpp` the free camera, `overlay.cpp` the Present hook and ImGui overlay, `ui.cpp` the panels, `pose.cpp`, `position.cpp`, `snap.cpp`, `input.cpp`, `kbm.cpp`, `xinput_proxy.cpp`; `cammath.h`, `posemath.h`, `posnmath.h`, `uilayout.h` hold the maths without Windows headers so `tests/` can run them natively.
- `third_party/`: Dear ImGui (MIT) and MinHook (BSD-2-Clause), with their licences.
- `build.sh`, `verify.sh`, `normalize_pe.py`, `tests/`.

## Licence

Apache-2.0 (`LICENSE.txt`, `NOTICE.txt`): keep the NOTICE and credit reeleese as the original creator. Developed alongside AI (Claude by Anthropic), directed and tested by reeleese. The libraries in `third_party/` keep their own licences.
