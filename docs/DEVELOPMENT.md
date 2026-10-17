# Developing LiveDeck

Technical notes for building, releasing and working on LiveDeck. For what the app does, see the [README](../README.md).

## Build on a Mac

You need the Xcode command-line tools and Homebrew's `cmake` and `ninja` (see "First-time setup" below).

```bash
./scripts/build-mac-app.sh --install
```

This builds a universal app (Apple Silicon and Intel), runs the tests, signs the app, makes `dist/LiveDeck-<version>.dmg` and copies **LiveDeck.app** into Applications.

- `--native` builds for this Mac's architecture only (about twice as fast).
- `cmake -S . -B build-xcode -G Xcode` makes an Xcode project.
- To sign and notarize with an Apple Developer ID, see the top of `scripts/build-mac-app.sh`.

The tests: `build-release/LiveDeckTests_artefacts/Release/LiveDeckTests` (add `--big` for the 4 GB RF64 test).

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

## First-time setup

```bash
xcode-select --install                 # Apple's compiler tools
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
eval "$(/opt/homebrew/bin/brew shellenv)"
brew install cmake ninja
```
