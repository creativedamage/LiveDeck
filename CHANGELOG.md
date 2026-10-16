# Changelog

Write what changed under **Unreleased**, then run `./scripts/release.sh <version>`.
The section becomes the release notes that LiveDeck shows in its update window.

## Unreleased

## 0.4.0 - 2026-10-16

- **Several audio devices at once** (e.g. Dante + MADI): View > Additional Audio Devices. Each device keeps its own clock; its tracks are recorded bit-exact and the drift between devices shows in the CLOCK chip. Channels are numbered after the primary device's and stay stable if a device goes offline.
- **Remote control**: OSC for Bitfocus Companion and other controllers (record, stop, play, locate, markers, takes, per-track arm/mute/solo) with live feedback; MIDI Machine Control; MIDI Learn for transport buttons. Transport > Remote Control.
- **Stereo tracks show separate L and R waveforms.**
- **Mirror Input to Output now only makes each track's output follow its input** (in 7 -> out 7). It no longer creates or re-patches tracks.
- Fixed: deleted tracks could come back after a device change. Tracks are now only created by + Track or Tracks > Add Multiple Tracks.
- Fixed: channels unticked in Audio Setup were switched back on. They now stay off and are left out of the track menus.
- + Track patches the next unused input; new Tracks menu with Add Multiple Tracks.

## 0.3.0 - 2026-10-16

- In-app updates: LiveDeck checks GitHub for new releases (Help > Check for Updates, or automatically once a day), shows what's new, then downloads, verifies and installs the update and relaunches. It never updates while recording or playing.
- Automated builds: every release is built, tested and packaged on GitHub.

## 0.2.0 - 2026-10-16

- Renamed to LiveDeck, with an app icon, a native macOS menu bar and keyboard shortcuts.
- Tracks Live-style timeline: coloured track rows, waveforms (live while recording), ruler with markers, click or drag to locate, zoom and follow.
- Mono or stereo tracks, chosen in each track's Input menu; stereo records interleaved stereo BWF. Mono and stereo output pairs.
- Multitrack playback for virtual soundcheck.
- `.livedeck` session files (older `.omr` sessions still open).

## 0.1.0 - 2026-10-15

- First version: 128+ track BWF recording, redundant second disk, crash recovery, disk benchmark, 1:1 mirror patching, meters.
