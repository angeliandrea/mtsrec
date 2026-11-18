# Multicam Timecode Sync Recorder

**mtsrec** is a free ISO recorder for Windows that works next to a Blackmagic ATEM switcher. It records
every DeckLink input to its own file, all with the same timecode, and when the show ends it builds the
editing project with the cuts and dissolves that went on air.

> **Beta.** It works and it is tested on a real control room, but it is still a beta: do not rely on it
> as the only recorder of a show you cannot repeat. Always keep another recording running.

## What it does

- Records every input of the DeckLink cards in the computer, one file per camera, in **DNxHD/DNxHR**
  inside MOV, with the **timecode** that arrives embedded in the SDI signal, or from the computer clock.
- Records the **format and frame rate the ATEM is set to**, with no conversion: 1080p from 23.98 to 60,
  1080i at 50, 59.94 and 60, and 2160p (experimental).
- Records the SDI audio, also as **Broadcast WAV** files that carry the same timecode.
- Logs every **cut and dissolve** of the ATEM over the network (read only), and **markers** you add while
  recording, from the keyboard or the page.
- At the end it creates the **DaVinci Resolve project** with a multicam clip and a timeline identical to
  the live program, and exports **XML and EDL for Adobe Premiere Pro**.
- Brings in the clips of a **HyperDeck** recorder on the same timeline.
- Checks the computer first (cards, CPU, drives, Windows settings) and proposes only what the machine
  can really handle, measured.

## Requirements

- Windows 10 or 11, 64-bit
- One or more Blackmagic DeckLink capture cards with **Blackmagic Desktop Video 16** or later
- An ATEM switcher on the same network, for cuts and names (optional: without it the files are recorded
  with their timecode, and the project has no cuts)
- DaVinci Resolve Studio for the automatic project (optional)
- Microsoft Edge WebView2 Runtime, already present on Windows 11 (without it the recorder opens in the
  web browser and works the same)

## Install

Download `mtsrec-<version>.msi` from [Releases](../../releases) and run it. It installs for your user only,
in `%LOCALAPPDATA%\Programs\mtsrec`, and needs no administrator rights. Settings, setups and the input map
are kept in `%USERPROFILE%\mtsrec`. Installing a new version replaces the old one; if WinISORecorder, the
previous name of this program, is installed, the installer removes it and keeps its settings.

The user manual is installed with the program (Help button or F1) and is also in
[`manuale/`](manuale/).

## Privacy

The program sends nothing over the internet by itself. On the local network it talks only to the ATEM
and HyperDeck addresses you enter. A test report is created only when you ask for it: you see what is
inside, and you decide whether to email it. See [CODE_SIGNING_POLICY.md](CODE_SIGNING_POLICY.md).

## Build from source

Everything builds on Windows with free tools. The [GitHub Actions workflow](.github/workflows/build.yml)
does it from scratch on every push, and is the reference.

1. Visual Studio 2022 Build Tools (or later) with the C++ workload, CMake and Ninja
2. MSYS2 with `mingw-w64-clang-x86_64-clang`, `nasm` and `make`, to build FFmpeg 9.0.2 from the official
   source into static libraries: `ffmpeg/compila-ffmpeg.sh` (see `ffmpeg/compila-ffmpeg.bat`)
3. The Python 3.13 embeddable package and the Microsoft.Web.WebView2 NuGet package, unpacked in one
   folder named by the `MTSREC_DEPS` environment variable
4. `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` and `cmake --build build`
5. The installer: `py installatore/crea-msi.py --build build --wix <WiX Toolset 3.14 folder>`

Comments in the source and the build scripts are in Italian; everything the user sees is in English.

## License

MIT, see [LICENSE](LICENSE). Copyright (c) 2026 Andrea Angeli.

Third-party components keep their own licenses: FFmpeg (LGPL 2.1 or later, built from source and linked
statically), Python (PSF License), the Microsoft WebView2 SDK loader, and the COM interface definitions of
the Blackmagic DeckLink SDK in [`third_party/decklink-sdk/`](third_party/decklink-sdk/). Details in
[`licenze/THIRD-PARTY NOTICES.txt`](licenze/THIRD-PARTY%20NOTICES.txt).

ATEM, DeckLink, HyperDeck, DaVinci Resolve, Desktop Video and Blackmagic Design are trademarks of
Blackmagic Design Pty Ltd. This project is not affiliated with or endorsed by Blackmagic Design. Adobe and
Premiere Pro are trademarks of Adobe.
