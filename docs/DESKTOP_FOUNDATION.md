# MRS Stage 0 — DAW Foundation (#20)

## Stack decision / spike
C++20 application controller plus a native Windows 10/11 x64 Win32 shell.
GDI double-buffered drawing provides the initial flat dark timeline; native
controls supply keyboard focus, text editing, file pickers and ASIO settings.
No browser/web server, new audio engine, GUI DLL distribution or additional
GUI package download is needed. The executable statically links existing libraries.
The foundation uses Segoe UI, graphite panels, neutral gray workspace selection and
amber playhead, following UI_UX_CONCEPT.md.

This stage proves UI integration, not the final drawing framework. Win32 currently
limits the graphical shell to Windows and requires explicit layout/accessibility
work. The platform-independent controller is tested on Linux too. A future UI
toolkit can replace the Win32 view without changing shared application services.
GPU rendering, rich editors and full custom accessibility are later decisions.
No claim that GDI is the final waveform/editor renderer.

Sources for the native spike:
- https://learn.microsoft.com/en-us/windows/win32/learnwin32/dpi-and-device-independent-pixels
- https://learn.microsoft.com/en-us/windows/win32/hidpi/wm-dpichanged
- https://learn.microsoft.com/en-us/windows/win32/gdi/wm-paint

## Structure and ownership
apps/studio-desktop/include/mrs/desktop.hpp: application/controller contract.
apps/studio-desktop/src/application.cpp: project/device/file commands and preferences.
apps/studio-desktop/src/win32_main.cpp: Windows views/navigation/layout.
core/audio/src/offline_device.cpp: SHARED offline clock adapter.
tests/desktop_tests.cpp: application integration tests.

Application owns one ProjectStore, EngineTransport, AudioEngine, GraphStore and
MusicalTimeline per open project. Arrange/Edit/Mix/Live use those same services.
Workspace switching changes a UI enum only, never resets playback or clones state.
Project edits use the existing command/Undo contract; track rename is exposed.
Device callbacks run independently of the UI, and logging/config/file dialogs stay
on the application thread. The shared offline adapter renders into discarded
preallocated buffers on its own development clock; it never produces hardware sound
and is not a timing/performance benchmark.

## Current UI
Arrange: core clips/sections/chords, moving playhead, click to seek.
Live is a separate show mode with .mrlive documents, planned in LIVE stages.
The production shell has no Live navigation button; saved legacy Live preference opens Arrange.
Edit: read-only clip inspector. Mix: read-only processor/parameter state.
These are workspace views, not completed arrangement/mixer/MIDI/plugin editors.
Tracks/rename/Undo/Redo remain in the workspace. A thin native menu row holds Files:
New project, Open project, Save, Save as, Import WAVs, Open WAV as new project,
Open demo project and Exit. File/project buttons are removed from the workspace.
Space = Play/Pause, Ctrl+N/O/S = New/Open/Save, Ctrl+Shift+S = Save as,
Ctrl+I = Import WAVs, Ctrl+Z/Y = project Undo/Redo outside name editing.
Native edit controls retain their normal typing/Undo behavior.

The demo has explicit fixture harmony/sections and a quiet 220 Hz tone. Harmony
is not inferred from a WAV. Open WAV creates a new audio project with no authored
chords/sections and preloads/validates PCM/float WAV through the shared decoder. Connect enforces a 512 MiB aggregate decoded preload cap.
Waveforms/clip editing/streaming are implemented in Stage 1a–1c; recording in 1d.
Resampling and MIDI editing remain later stages.

Project documents use the accepted Stage 4 archive. Unknown chunks/state survive
Open/Save. The controller replaces a project stopped using the silent offline clock. The shell
then restores the user's enabled ASIO connection, prepares the shared graph and
preloads audio. Playback never starts automatically; unavailable processors still fail explicitly.
Missing media or unavailable processors produce an error and leave audio disconnected
rather than silently omitting assets or substituting processors. The project remains
open and can be saved without losing unavailable plugin state.

Absolute media paths from Open WAV are preserved. Relative sources resolve against
the opened project folder. Save As to a different folder is currently refused when
relative media would be broken; same-folder Save/Save As works. No asset copying/
consolidation yet. Dirty project replacement/close prompts to save or discard.

## Audio settings
On first launch Desktop starts in Offline clock (no sound). Subsequent launches
restore an enabled saved ASIO connection by device name, resolving the current index.
Open WAV, Open project and Demo also restore it after replacing the project, using
the new project's sample rate and saved buffer/output/input selectors. Connect enables
restoration; explicit Disconnect or choosing Offline disables it and persists that choice.
Opening ASIO panel remains a temporary disconnect until explicit Connect. The ASIO artifact also enumerates the
same native vendor ASIO backend as Stage 1. User selects device, rate, buffer,
one-based physical output selectors (e.g. 1,2) and optional monitor input
(0 disabled, 1..64 physical channel). Connect validates configuration then stops
the previous callback, prepares graph/assets, opens and starts the selected backend.
Transport is reset on connect/disconnect/reconfiguration. No automatic host fallback.

Requested rate must match project/WAV rate; there is no resampler.
ASIO panel first disconnects audio; reconnect after panel/driver changes.
The dialog shows actual output latency and CPU; main status shows callback/underrun
counters. Native monitoring shares the same prepared processor graph. Monitoring
is optional and only enabled by choosing an input and pressing Connect.
Driver failures are reported; seamless reconnect/recovery remains deferred #16.

Preferences live in %LOCALAPPDATA%/MoonRiverStudio/desktop.cfg (versioned, bounded
reader), logs in studio.log. Workspace, requested device/rate/buffer/channel settings
are retained together with connection intent (config v2, with v1 migration).
Device handles are reopened, never serialized; playback state is never restored.
Preferences failures are reported and a damaged config falls back to defaults.
Config is convenience state, not crash-safe project storage. No audio callback logs.
Project autosave scheduling/recovery chooser UI is not included yet; accepted shared
autosave/recovery APIs remain available.

## DPI / verification
PerMonitorV2 manifest, GetDpiForWindow, DIP-scaled layout/fonts and WM_DPICHANGED
suggested rectangles. Main and audio windows track their own DPI. Initial main
window is clamped to monitor work area; minimum logical size 1000x620.
At short window heights only the first timeline tracks fit; scrolling/full editor
layout comes with Audio Arrangement. This is a foundation for 100/125/150% scaling.

Six desktop suites join all 41 previous contracts on Windows/Linux Debug/Release
and Windows ASIO Debug/Release. Offline Windows builds also run a GUI smoke test
that opens all workspaces/settings, renames a track and exercises 150% layout.
ASIO builds do not open a driver in CI. Hardware playback and visual DPI acceptance
are manual checks from MRS_STAGE_0_CHECKLIST.md. Existing Stage 1 performance gate
remains pending/nonblocking by the user's decision.

Build:
cmake -S . -B build -DMRS_BUILD_ASIO=ON
cmake --build build --config Release
Run build/Release/MoonRiverStudio.exe. Without MRS_BUILD_ASIO only the explicit
offline adapter is available. Linux builds the controller/contracts, not a GUI.

## 2026-10-03 acceptance fix
User confirmed their WAV plays through Komplete Audio ASIO Driver. Screenshots
show actual 48000 Hz and zero output underruns at the captured paused state.
This is basic shell hardware acceptance, not the deferred sustained performance gate.

Audio settings initialization was incorrectly validating device selection on edit
notifications before device enumeration. Device validation now runs only for
explicit Connect / ASIO panel actions. Field/combo initialization and incomplete
typing do not open devices or show errors. GUI smoke asserts zero unexpected errors
and exercises typing while combo selection is temporarily absent.
Ruler label spacing is adaptive to avoid overlapping bar numbers in long WAV projects.

## ASIO connection continuity fix
User confirmed the settings popup is gone. Replacing a project had reset the backend
to Offline, requiring repeated Connect. The shell now restores enabled saved ASIO
settings after project replacement and on startup. Missing devices/unsupported rates/
media failures produce one explicit error, keep the project open and do not substitute
another hardware driver. Automatic hardware recovery during playback remains deferred.

## 0.1d upd1 / fix1
Primary background, panels, buttons and active workspace selection are neutral gray.
Chord/section/clip/waveform/playhead colors stay unchanged. Files menu uses native
Win32 menu keyboard/DPI handling; its row is outside the workspace client rectangle.
Mono voices route equally to the first two selected outputs (or the sole output),
with no automatic spill into additional cue outputs. Stereo routing remains L/R.
Menu placement, removed file/Live buttons and 150% layout are covered by GUI smoke;
mono_route covers one, two and four output configurations.

## 0.1e — Stage 1d
Record (R), Arm track/Disarm and Monitor on/off share Application/AudioEngine.
Input dBFS meter remains independent of monitor. Pause/Space ends the take; Stop
ends it and returns to zero. Close finalizes before save/discard. Project/device
edits and file commands are disabled during recording; monitor stays available.
Recorded WAVs live under Audio beside the saved project, clips use absolute paths;
Undo leaves media on disk. Input and recorded mono route only to the main pair.
See RECORDING.md and MRS_STAGE_1D_CHECKLIST.md; physical acceptance is pending.


## 0.1e upd1 fix1 — project-owned folders
The requested folder follow-up replaces the GUI recording Audio directory with Media.
Project folders contain their .mrsproject, Media and Mixdown; the studio root contains
Projects and Lives. Imported WAVs are copied and archived with portable Media/... refs.
First save consolidates legacy external/Audio sources without deleting originals.
Save As copies content and retains source aliases/Undo and the same stopped/paused
device handle. See PROJECT_FOLDERS.md and MRS_PROJECT_FOLDERS_CHECKLIST.md.
User acceptance of upd1 pending; base 0.1e remains accepted.


## Included fix1: concurrent seek read-head protection
Seek priming previously published the future target as the current read head before
its queued transport command reached the callback. A worker could then evict the
still-playing page in that interval. Prime sets the initial head only once,
keeps the future target separately warm, and lets the callback move the active head.
The worker skips protected pages before claiming ownership. A control/worker-only
atomic gate serializes the ready snapshot with victim selection, closing the stale
snapshot window without waiting, locking or I/O on the audio callback.
Prepared UI seeks are coalesced and applied only after the callback pins all needed
target pages. If a later prime displaced an earlier target, the callback continues
the current position and retries on its next block; worker retry pages remain
protected. This closes the queued-command handoff race without blocking RT.
The concurrent seek exact-sample/zero-underrun/zero-RT-allocation regression is
repeated eight times in every Debug/Release CI job for this fix.
