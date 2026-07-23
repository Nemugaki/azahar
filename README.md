# Azahar fork: Cool Developer Edition

To whomever may be curious enough, this is my personal Azahar branch, it has a couple of changes and features, that were made orchestrated by myself, but executed
with AI, as I am not knowledged enough regarding the ins and outs of emulator itself, or the 3DS GPU, to fit my needs, these being automation, scripts, debugging
and allowing programs such as AI agents access to the emulator state to help with debugging.

I eventually lost the plot and just added too many features, including overhauling existing debug features for my convenience and usage.

The main features are:

- The UI now uses QT Advancing Docking System, as I found to have issues with the so many debugger windows and tools stacking on top of eachother on a single, right side pane.
- Expanded and improved RPC. It is now what some people call "first class".
  - Controls game start, stop, pause, resume, restart, and frame advance. The server listens whenever the application is open, not only while a game is running, so scripts can control the whole game lifetime.
  - Exposes ARM registers, memory, Pica state and shaders, command traces, breakpoints, current render targets, and a bounded draw/frame timeline without requiring large state dumps.
  - Supports conditional Pica breakpoints by command, color/depth target, draw, or frame, including masked comparisons.
  - Exposes dedicated Pica breakpoint resume as well as normal and CPU-debug resume controls.
  - Reports a unified pause reason and supports blocking waits, so scripts can synchronize with user, CPU, Pica, and frame-advance pauses without polling.
  - Creates atomic, bounded ARM/Pica captures and compact byte-level diffs for comparing debugger states.
  - Uses protocol validation and explicit error replies, separates memory reads from default-off guest-memory writes, retries requests safely, and reports bind failures.
  - Lists, pins, deletes, and reports cache use for immutable debugger captures; paged Pica snapshot generations remain readable while newer captures arrive.
  - Supports savestate loading and saving by slot, plus screenshots to a chosen output path.
  - Reports handled request count and supports a configurable port for running multiple instances.
  - Gates memory, CPU, Pica, savestate, screenshot, and emulator-control access independently in the UI.
  - Lists, selects, imports, exports, and removes render-debugger sessions. Host-path capture access is independently gated and disabled by default.
- Qt debugger improvements use the same underlying state as RPC:
  - Breakpoint-aware frame advance from normal pauses, CPU-debug pauses, and Pica GPU breakpoints.
  - Structured conditional Pica breakpoint editing with field, value, mask, validation, and current-value helpers.
  - A searchable Frame → Render Target → Pipeline → Draw timeline labels draw mode, topology, vertex counts, shader entry points, target changes, and provides an event scrubber plus captured per-draw output previews.
  - Configurable capture memory and timeline frame limits evict the oldest debugger history before it can grow without bound.
  - One-shot, skip-count, hit-count, enable-all, and disable-all Pica breakpoint controls.
  - Surface channel/alpha views and raw export, shader entry-point navigation and raw words, and command-register before/after deltas.
  - Shared current color/depth render-target inspection in the surface viewer.
  - Per-core ARM register selection with unavailable-state reporting and previous-capture change highlighting.
  - Debugger tabs can dock on every edge, tab or nest together, float as non-modal windows, and remain in a static workspace while their content is enabled or disabled; View menu entries always reopen the corresponding tool. The Game view is also a closable, reopenable dock that can move, tab, or float with the workspace.
- A Qt-free render-debugger library is shared by the emulator producer, Qt, and RPC. Its bounded live session and immutable imported sessions use a documented fixed `.rdbg` interchange format carrying the timeline, register writes, draw associations, shaders, and owned resources; captures are not emulator-state rewind.
- GDB remote debugging works with GDB 17's handshake, complete ARM/VFP register transfers, safer memory packets, process reattachment, and pre-attach interrupts. The Nix flake includes a patched GDB 17.2 that recognizes the `3DS` OSABI, uses the 3DS ARM/Thumb breakpoint encodings, and software-steps across SVC instructions.
- Fixed an issue that CiTrace *apparently* had an issue since a GPU refactor from 2023.
- CiTrace and command histories are bounded by the debugger cache limit; incomplete traces are identified instead of silently saved.

### Nix development and 3DS GDB

The flake supports x86_64 and ARM64 Linux. Its development shell includes the
project dependencies and the patched `gdb-3ds`:

```sh
nix develop
gdb /path/to/title.elf
```

The debugger can also be run without entering the shell:

```sh
nix run .#gdb-3ds -- /path/to/title.elf
```

Enable Azahar's GDB stub in the Debug settings, or launch Azahar with
`--gdbport 24689`. Once the title is running, connect from GDB:

```gdb
target remote 127.0.0.1:24689
```

This GDB build also embeds Python with protobuf for Ghidra's TraceRmi debugger
bridge. Stock GDB does not recognize Azahar's `3DS` target description; use
the flake package for live 3DS debugging.

### Merging
Per [AI-POLICY.md](AI-POLICY.md) I will not open a merge request to the original repo with any of these changes whatsoever.
This repo remains open for anybody to explore its changes, and will remain updated as long as I require to use Azahar in my workflow.
I do not care for what the code is used for.

# README.md
![Azahar Emulator](https://azahar-emu.org/resources/images/logo/azahar-name-and-logo.svg)

![Current Release](https://img.shields.io/github/v/release/azahar-emu/azahar?label=Current%20Release)
![Current Prerelease](https://img.shields.io/github/v/release/azahar-emu/azahar?include_prereleases&label=Current%20Prerelease)

![GitHub Downloads](https://img.shields.io/github/downloads/azahar-emu/azahar/total?logo=github&label=GitHub%20Downloads)
![Google Play Downloads](https://playbadges.pavi2410.com/badge/downloads?id=io.github.lime3ds.android&pretty&label=Play%20Store%20Downloads)
![Flathub Downloads](https://img.shields.io/flathub/downloads/org.azahar_emu.Azahar?logo=flathub&label=Flathub%20Downloads)
![CI Build Status](https://github.com/azahar-emu/azahar/actions/workflows/build.yml/badge.svg)

<b>Azahar</b> is an open-source 3DS emulator project based on Citra.

It was created from the merging of PabloMK7's Citra fork and the Lime3DS project, both of which emerged shortly after Citra was taken down.

The goal of this project is to be the de-facto platform for future development.

# Installation

### Windows

Azahar is available as both an installer and a zip archive.

Download the latest release in your preferred format from the [Releases](https://github.com/azahar-emu/azahar/releases) page.

If you are unsure of whether you want to use MSVC or MSYS2, use MSYS2.

---

### MacOS

To download a build that will work on all Macs, you can download the `macos-universal` build on the [Releases](https://github.com/azahar-emu/azahar/releases) page.

Alternatively, if you wish to download a build specifically for your Mac, you can choose either:

- `macos-arm64` for Apple Silicon Macs
- `macos-x86_64` for Intel Macs

---

### Android

There are two variants of Azahar available on Android, those being the Vanilla and Google Play builds.

The Vanilla build is technically superior, as it uses an alternative method of file management which is faster, but isn't permitted on the Google Play store.

For most users, we currently recommended downloading Azahar on Android via the Google Play Store for ease of accessibility:

<a href='https://play.google.com/store/apps/details?id=io.github.lime3ds.android'><img width='180' alt='Get it on Google Play' src='https://raw.githubusercontent.com/pioug/google-play-badges/06ccd9252af1501613da2ca28eaffe31307a4e6d/svg/English.svg'/></a>

Alternatively, you can install the app using Obtainium, allowing you to use the Vanilla variant:
1. Download and install Obtainium from [here](https://github.com/ImranR98/Obtainium/releases) (use the file named `app-release.apk`)
2. Open Obtainium and click 'Add App'
3. Type `https://github.com/azahar-emu/azahar` into the 'App Source URL' section
4. Click 'Add'
5. Click 'Install', and select the preferred variant

If you wish, you can also simply install the latest APK from the [Releases](https://github.com/azahar-emu/azahar/releases) page.

Keep in mind that you will not recieve automatic updates when installing via the APK.

---

### Linux

The recommended format for using Azahar on Linux is the Flatpak available on Flathub:

<a href='https://flathub.org/apps/org.azahar_emu.Azahar'><img width='180' alt='Download on Flathub' src='https://dl.flathub.org/assets/badges/flathub-badge-en.png'/></a>

Azahar is also available as an AppImage on the [Releases](https://github.com/azahar-emu/azahar/releases) page.

There are two variants of the AppImage available, those being `azahar.AppImage` and `azahar-wayland.AppImage`.

If you are unsure of which variant to use, we recommend using the default `azahar.AppImage`. This is because of upstream issues in the Wayland ecosystem which may cause problems when running the emulator (e.g. [#1162](https://github.com/azahar-emu/azahar/issues/1162)).

Unless you explicitly require native Wayland support (e.g. you are running a system with no Xwayland), the non-Wayland variant is recommended.

The Flatpak build of Azahar also has native Wayland support disabled by default. If you require native Wayland support, it can be enabled using [Flatseal](https://flathub.org/en/apps/com.github.tchx84.Flatseal).

# Build instructions

Please refer this repository's [wiki](https://github.com/azahar-emu/azahar/wiki/Building-From-Source) for build instructions

# How can I contribute?

### Pull requests

If you want to implement a change and have the technical capability to do so, we would be happy to accept your contributions.

If you are contributing a new feature, it is highly suggested that you first make a Feature Request issue to discuss the addition before writing any code. This is to ensure that your time isn't wasted working on a feature which isn't deemed appropriate for the project.

After creating a pull request, please don't repeatedly merge `master` into your branch. A maintainer will update the branch for you if/ when it is appropriate to do so.

### Language translations

Additionally, we are accepting language translations on [Transifex](https://app.transifex.com/azahar/azahar). If you know a non-english language listed on our Transifex page, please feel free to contribute.

> [!NOTE]
> We are not currently accepting new languages for translation. Please do not request for new languages or language variants to be added.

### Compatibility reports

Even if you don't wish to contribute code or translations, you can help the project by reporting game compatibility data to our compatibility list.

To do so, simply read https://github.com/azahar-emu/compatibility-list/blob/master/CONTRIBUTING.md and follow the instructions.

Contributing compatibility data helps more accurately reflect the current capabilities of the emulator, so it would be highly appreciated if you could go through the reporting process after completing a game.

# Minimum requirements

Below are the minimum requirements to run Azahar:

### Desktop

```
Operating System: Windows 10 (64-bit), MacOS 13.4 (Ventura), or modern 64-bit Linux
CPU: x86-64/ARM64 CPU (Windows for ARM not supported).
     Single core performance higher than 1,800 on Passmark.
     SSE4.2 required on x86_64.
GPU: OpenGL 4.3 or Vulkan 1.1 support
Memory: 2GB of RAM. 4GB is recommended
```
### Android

```
Operating System: Android 10.0+ (64-bit)
CPU: Snapdragon 835 SoC or better
GPU: OpenGL ES 3.2 or Vulkan 1.1 support
Memory: 2GB of RAM. 4GB is recommended
```

# What's next?

We share public roadmaps for upcoming releases in the form of GitHub milestones.

You can find these at https://github.com/azahar-emu/azahar/milestones.

# Join the conversation

We have a community Discord server where you can chat about the project, keep up to date with the latest announcements, or coordinate emulator development.

Join at https://discord.gg/4ZjMpAp3M6
