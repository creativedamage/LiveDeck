# LiveDeck

A multitrack recorder and virtual-soundcheck player for live events, built for macOS (it also builds on Windows). It records SoundGrid, Dante, MADI and USB interfaces to Broadcast WAV files. It can write to two disks at once, shows a Tracks Live-style timeline, and plays takes back out to the console.

**Version 0.4.** The record and playback engine is covered by 167 automated checks. It has been built on Linux with GCC and Clang and on your Mac, but **not tested yet with SoundGrid, Dante or MADI hardware.** Do that before a show.

## Install on your Mac

You need the Xcode command-line tools and Homebrew's `cmake` and `ninja`. See "First-time setup" at the end if you don't have them.

```bash
cd ~/Documents/LiveDeck
./scripts/build-mac-app.sh --install
```

This builds a universal app (Apple Silicon and Intel), runs the tests, signs the app, makes `dist/LiveDeck-0.4.0.dmg`, and copies **LiveDeck.app** into Applications. Open it from Launchpad or Spotlight like any other Mac app. Double-clicking a `.livedeck` session file also opens it in LiveDeck.

Options:

- `--native` builds for this Mac's architecture only, which is about twice as fast.
- To install on other Macs without Gatekeeper warnings, sign and notarize with an Apple Developer ID. The instructions are at the top of `scripts/build-mac-app.sh`.

For development, `cmake -S . -B build-xcode -G Xcode` makes an Xcode project.

## Using LiveDeck

| Task | How |
| --- | --- |
| Pick the driver, sample rate and buffer size | **Audio Setup** (Cmd+,). SoundGrid, Dante Virtual Soundcard, MADI and USB interfaces appear as CoreAudio devices. |
| Make a track mono or stereo | In a channel strip's **Input** menu, pick a channel under **Mono** or a pair under **Stereo** (1-2 ST, 3-4 ST…). That choice sets the track width. Stereo tracks record one interleaved stereo BWF file, which Pro Tools, Logic and Reaper all import. |
| Choose outputs | The **Output** menu has the same Mono / Stereo sections. A mono track sent to a stereo pair plays on both sides. A stereo track sent to a mono output is summed. |
| Add tracks | **+ Track** adds one track on the next unused input. **Tracks > Add Multiple Tracks…** adds many at once (count, starting input, mono or stereo pairs). Tracks are never created automatically. |
| Mirror outputs | **Mirror Input to Output** makes each track's output follow its input (in 7 → out 7, in 3-4 ST → out 3-4 ST). It never creates or removes tracks. Patching an output by hand turns it off. |
| Choose channels | Untick channels you don't use in **Audio Setup**. They stay off and are left out of the track menus. A track patched to a channel that disappears keeps its patch, shows "(off)", and records silence until the channel returns. |
| Start a session | **File > New Session** (Cmd+N) asks for a name. **Create** makes `Documents/LiveDeck/Shows/YYYY-MM-DD_<Name>/` (with `Audio/`, `Recovery/`, `Logs/` and `Markers/` inside); **Choose Folder…** puts it anywhere else, such as an external SSD. Opening a session or template always asks where to record. |
| Templates | **File > Save as Template…** saves the track layout into `Documents/LiveDeck/Templates`. **File > New from Template** lists them. LiveDeck creates the `Documents/LiveDeck` folder (Shows + Templates) the first time it starts. |
| Import audio | Drag WAV, BWF or AIFF files (or a folder of them) from Finder onto the timeline. Each file becomes a new track in the take on screen (or a new take if there is none), starting where you drop it; dropping near the left edge starts at 0:00. Stereo files make stereo tracks, multichannel files make one track per channel. Files are converted to the session's format and sample rate; the originals are not changed. Imported tracks start disarmed. |
| Record | **REC** (Cmd+R) starts a new take. Inputs and arm states are locked while recording. Waveforms draw live as audio comes in. **STOP** (Cmd+.) finalises the take. |
| Play back for virtual soundcheck | Pick a take, then press **PLAY** or Space. Each recorded track plays to its own outputs in place of its live input. |
| Move around the timeline | Click or drag anywhere in the timeline to move the playhead. Cmd+scroll zooms, and Cmd+0 zooms to fit. **Follow** keeps the playhead in view. S / M / L set the row height. Right-click a track header to change its colour. Return goes back to zero. |
| Select tracks | Click a track header on the timeline or a strip in the mixer. Shift-click selects a range, Cmd-click adds or removes one track, Cmd+A selects all, Esc clears. |
| Delete tracks | Right-click > **Delete Track(s)**, Delete/Backspace, or **- Track**. You're asked to confirm. Recorded audio files are always kept. |
| Rename tracks | Double-click a name (timeline or mixer). **Tab** saves and moves to the next track, Shift+Tab goes to the previous one, Return saves, Esc cancels. |
| Reorder tracks | Drag a track header up or down on the timeline, or a strip sideways in the mixer. A multi-selection moves as one block. Recorded takes follow their tracks: each take saves a small manifest (`Audio/.take_NNN.json`) that matches files to tracks. |
| Several interfaces | **View > Additional Audio Devices…** opens more devices next to the primary one (for example Dante and MADI), at the same sample rate and buffer size. Their channels are listed after the primary's as "Device: channel". Each device keeps its own clock: its tracks are recorded bit-exact at that clock and never resampled. The CLOCK chip shows the drift between devices; clock all devices from one word clock / PTP source to keep them sample-locked. Direct outs must be on the same device as the track's input. |
| Remote control | **Transport > Remote Control…** turns on OSC (default UDP port 9010) and optional feedback (default 127.0.0.1:9011). The OSC commands and feedback messages are listed in that window. MIDI: pick an input, use MMC (Play / Stop / Record / Locate), or **Learn** a note/CC for each transport button. Record over OSC/MIDI needs an open session, and play/stop never stops a recording. |
| Markers | **M** or the **MARK** button. Markers show as flags on the ruler and are saved in `Markers/markers.csv`. |
| Record to two disks | **Disks > Set Redundant Folder (Disk B)**. Each disk has its own buffer and writer thread. |
| Check a disk | **Disks > Benchmark Disks** runs the real write pattern for 15 s and reports PASS or FAIL. |
| Change the layout | The **View** menu shows or hides the mixer, meter bridge and event console. Drag the bar between the timeline and the mixer to resize them. |
| Recover after a crash | When you open a session that wasn't stopped cleanly, LiveDeck offers to repair its files. Everything that reached the disk is kept. |
| Feedback | **Help > Request a Feature / Report a Bug / Send Feedback** opens a filled-in GitHub issue in your browser (a free GitHub account is needed to submit it). **Help > Feature Requests and Known Issues** shows what's already been asked. |
| Start without dialogs | `/Applications/LiveDeck.app/Contents/MacOS/LiveDeck --session /Volumes/SSD/MyShow` |

Space never stops a recording. Only STOP or Cmd+. ends a take.

**If the built-in Mac microphone is too hot:** LiveDeck never adds gain to what it records. Lower the level in System Settings > Sound > Input. Pro interfaces deliver audio at a fixed level, so they aren't affected.

## Releases and updates

LiveDeck updates itself from this project's GitHub Releases.

**One-time setup** (on your Mac): `brew install gh`, then `./scripts/setup-github.sh`. It signs you in to GitHub, creates the public **LiveDeck** repository, pushes the code and offers to publish the first release.

**Publishing a new version:**

1. Write what changed under `## Unreleased` in `CHANGELOG.md` and commit it.
2. Run `./scripts/release.sh 0.3.1`, using the next version number.

That's all. The script sets the version, tags it and pushes. GitHub Actions then builds and tests a universal app on a Mac. It publishes a release with `LiveDeck-<version>.dmg`, `LiveDeck-<version>-mac.zip` and its `.sha256` checksum. The changelog section becomes the release notes. Every push and pull request is also built and tested (`.github/workflows/ci.yml`).

**In the app:**

- **Checking:** Help > Check for Updates…, plus an automatic check once a day (toggle it in the Help menu).
- **Installing:** LiveDeck shows the release notes and offers Install and Relaunch, Skip This Version, or Later. Installing downloads the update zip and checks its checksum, its code signature and that it's really LiveDeck at the expected version. It then swaps the app, keeping the old one until the copy succeeds, and relaunches. The installer log is `~/Library/Logs/LiveDeck-update.log`.
- **During a show:** LiveDeck never installs while recording or playing.

**Without an Apple Developer ID**, builds are ad-hoc signed. In-app updates still install cleanly. The first time you install a downloaded `.dmg` on a new Mac, right-click LiveDeck.app and choose **Open**. To remove that step, add your Developer ID certificate and notarization details as repository secrets; the list is at the top of `.github/workflows/release.yml`.

## How it stays up

- **The audio callback only copies samples.** It never allocates memory, waits on a lock or touches a file. Routing changes arrive as an immutable snapshot.
- **Each disk has its own lock-free buffers and writer thread.** The buffer holds 20 s by default (Disks > Buffer Depth). A stall longer than that is replaced with silence and logged with exact sample positions. Every file, stereo included, stays sample-aligned.
- **Files are preallocated so their length always matches the audio written.** Headers and the journal update every second. Files larger than 4 GB switch to RF64 on their own.
- **Waveform overviews are built while recording** and cached in `Audio/.peaks/`, so reopening a session draws instantly.
- **Playback is the record path in reverse.** A disk underrun gives silence, never a stall, and tracks stay aligned.

## Source map

```
Source/Core/     engine with no UI (built into the app and the tests)
  RecordEngine   real-time process(): routing, mono/stereo, meters, record buffers, playback, monitor bus
  RecordTarget   one disk: buffers, writer thread, journal, gap handling, live waveform peaks
  Player         playback reader thread and buffers; scanTakes()
  PeakStore      waveform overview (multi-level) and the background cache builder
  Updater        GitHub Releases check, download, checksum, verified swap-and-relaunch
  BwfWriter      BWF writer (bext + iXML), mono or stereo, RF64 upgrade
  Recovery, PatchModel, Session, Benchmark, PlatformFile, EventLog, SpscRing
Source/Audio/    AudioEngine: JUCE AudioDeviceManager -> RecordEngine
Source/UI/       TimelineView, ChannelStrip / MeterBridge, TransportBar, MainComponent (menus)
Resources/       AppIcon.png
scripts/         build-mac-app.sh (build + package), release.sh (publish a version)
.github/         CI and release workflows
Tests/           LiveDeckTests (run with --big to include the 4 GB RF64 test)
```

## Not built yet

- Sending live audio from one device's input to another device's output (needs a resampler). Playback and recording across devices work.
- OSC feedback goes to one target. For several controllers, point them at Companion and let Companion fan out.

## First-time setup

```bash
xcode-select --install                 # Apple's compiler tools
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
eval "$(/opt/homebrew/bin/brew shellenv)"
brew install cmake ninja
```
