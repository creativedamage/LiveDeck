# Changelog

Write what changed under **Unreleased**, then run `./scripts/release.sh <version>`.
The section becomes the release notes that LiveDeck shows in its update window.

## Unreleased

## 0.7.1 - 2026-10-18

- **Select clips**: Cmd+A selects every clip (audio file) in the take on screen; click a clip to select it, Shift or Cmd-click to add more, Esc to clear. Delete removes the selected clips from their tracks (the files stay in the session folder), dragging one selected clip moves them all together, and right-click offers Move to Start / Move to Playhead for the whole selection. All undoable with Cmd+Z. Shift+Cmd+A now selects all tracks.
- **Routing checks**: tracks whose input or output is turned off in Audio Setup (or doesn't exist on the device) now show their channel in orange "(off)", and the event log names them. The device line shows how many channels are on (e.g. "64 in [1 on]").
- **View > Turn On All Inputs and Outputs** switches every channel of the audio device on in one click.

## 0.7.0 - 2026-10-17

- **LiveDeck accounts and licenses**: sign in with email and password (with email confirmation, Forgot Password and Change Password) or with Google or Microsoft, register your organization, and buy an annual ($50/year) or perpetual ($129) license through Stripe. Verified non-profits get 10% off and no sales tax.
- Each license runs on 2 computers at a time on the same network. LiveDeck checks the license at every launch and can start offline for up to 14 days. It never locks during a show.
- **Account menu**: Account and License, Edit Registration, Sign Out. Admins also get an Admin Panel to verify non-profits and to create, revoke or extend licenses.
- **Flat design**: a new flat look throughout, with solid colours, no gradients or bevels, flat meters, buttons, knobs and dialogs.

## 0.6.0 - 2026-10-16

- **Drop audio onto a track**: drop a file on a track to put it on that track (its audio in the take on screen is replaced; the old file stays in the session folder). Several files fill that track and the ones below; files dropped below the last track still become new tracks.
- **Move clips**: drag a clip by its name bar (or Alt-drag anywhere on it) to move it in time or onto another track. It snaps to 0:00, its old position, other clips and markers. If the other track already has audio you choose Replace or Swap.
- **Right-click a clip**: Move to Start of Take, Move to Playhead, Remove from Track, Show File in Finder.
- **Undo** (Edit > Undo, Cmd+Z) for imports and clip moves.
- Moving a clip never rewrites audio: each take's manifest stores where every file starts.

## 0.5.0 - 2026-10-16

- **Drag and drop audio onto the timeline**: WAV, BWF and AIFF files (or a whole folder) become new tracks in the take on screen, starting where you drop them (near the left edge snaps to 0:00). Stereo files make stereo tracks; multichannel files are split into one track per channel. Files are converted to the session's format and sample rate; the originals are never touched.
- **LiveDeck folder in Documents**: Documents > LiveDeck > Shows (new sessions go here by default) and Templates (File > New from Template lists them). Choose Folder... still records anywhere else, such as an external SSD.
- **Feedback and feature requests**: Help > Request a Feature, Report a Bug or Send Feedback opens a filled-in GitHub issue in your browser.
- New app icon that fills the whole icon shape (no more icon inside an icon).

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
