# XOA

A tenth-order Ambisonics (10OA → "XOA") spatial audio processor built with the
JUCE framework — the Ambisonics sibling of [WFS-DIY](https://github.com/pob31/WFS-DIY),
sharing the same real-time core.

## Project overview

XOA encodes sound sources into a 10th-order spherical-harmonic representation
(121 channels, ACN ordering / SN3D normalization), transforms the sound field
(rotation, mirroring, zoom), and decodes it to loudspeaker arrays or binaural
output.

The real-time engine and control plane are provided by
[spatcore](https://github.com/pob31/spatcore) — the shared spatial-audio core
extracted from WFS-DIY, consumed here as a git submodule and built natively
with CMake (`spatcore-audio`, `spatcore-control`, `spatcore-controllers`).

> **Status: bootstrap.** The repository currently builds a minimal JUCE
> application shell linked against all three spatcore libraries. The
> Ambisonics engine, parameter schema, OSC/MCP control surface and GUI are
> being ported/developed on top of it — see
> [Documentation/XOA-PLAN.md](Documentation/XOA-PLAN.md) for the roadmap.

## Building

### Prerequisites

**Windows**
1. [Git for Windows](https://git-scm.com/download/win)
2. [Visual Studio 2026 Community](https://visualstudio.microsoft.com/) with the
   **"Desktop development with C++"** workload (includes CMake).

**macOS**
1. [Xcode](https://apps.apple.com/app/xcode/id497799835) (includes git)
2. [CMake](https://cmake.org/download/) ≥ 3.22 (`brew install cmake`)

**Linux (Debian / Ubuntu)**
```bash
sudo apt install build-essential cmake pkg-config libasound2-dev libfreetype6-dev \
    libfontconfig1-dev libgl1-mesa-dev libcurl4-openssl-dev libgtk-3-dev \
    libwebkit2gtk-4.1-dev libudev-dev
```

### Clone and bootstrap

```bash
git clone --recurse-submodules https://github.com/pob31/XOA.git
cd XOA
./tools/setup.sh   # idempotent; fixes a clone made without --recurse-submodules
```

Submodules: `ThirdParty/JUCE` (JUCE 8), `ThirdParty/hidapi` (HID controller
support) and `spatcore` (the shared engine, pinned to a released tag).
`juce_simpleweb` and `roli_blocks_basics` are vendored under `ThirdParty/`.

### Configure and build

```bash
cmake -S . -B build
cmake --build build --config Release
```

On Windows this generates a Visual Studio solution in `build/` (open
`build/XOA.sln` to work in the IDE). On macOS add `-G Xcode` for an Xcode
project. The binary lands under `build/XOA_artefacts/<config>/`.

## License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0).

Copyright (c) 2026 Pierre-Olivier Boulant

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

Third-party components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
