# Shared recording / Stage 1d (0.1e)

One Recorder belongs to the existing RenderGraph / AudioEngine; no second transport,
audio device, project store or recording-specific playback backend. The desktop
arms one existing audio track and selects one hardware ASIO input through DeviceConfig.
Monitoring routes that input to the first two selected outputs and can be toggled
through the fixed control queue during recording or idle playback.

## Ownership and realtime boundary
Recorder allocates a fixed 262144-float mono ring (1 MiB) and opens a unique temporary
file on the control thread. One audio producer copies raw input before monitor,
backing voices, native processors and output limiter. Only finite samples are stored;
non-finite input becomes zero with an explicit counter. No callback file operations,
heap allocation, locks, thread wake/wait, project mutations or ownership release.
Atomics used by capture/meter are compile-time required lock-free.

A worker polls the ring, encodes bounded 8192-frame little-endian float32 blocks,
and writes RIFF WAV. Release/acquire publication prevents overwrite of pending data.
The worker drains before header repair/publication; finish is only called once
callbacks have stopped. Temporary .partial filenames are unique and published without
overwriting existing media. Publication uses MoveFileExW without replace on Windows
and a same-directory hard link on other tested platforms. This is clean completion,
not a guarantee of power-failure durability.

## Start / end workflow
Start requires paused/stopped playback, no loop, an armed audio track, one connected
hardware input and available retained-source/clip/disk-voice budgets. Offline clock
cannot record. Same ASIO handle is stopped to install the capture graph, then resumed
with Play through the existing EngineTransport. GUI requests saving an unsaved project
first and creates Audio/Take-<unique-id>.wav beside it.

End stops callbacks, detaches capture, retains paused position, drains and finalizes
the WAV, and attaches it with one AddRecordedClip command to the armed track.
The clip uses capture start and actual committed frame count, source offset zero,
and an absolute UTF-8 source path. Other clips are audible while recording but are
not written into the take. Pause/Space finalize; Stop also resets position.
Close finalizes before the normal dirty-project save/discard prompt.
Undo removes the reference, never the WAV; Redo restores it. Reopen reuses the
existing decoder, disk read-ahead and archive; no schema change.

## Failure behavior / budgets
The first input underflow/overflow, missing input, unexpected seek/loop, non-contiguous
capture position, full ring or RIFF/timeline size limit latches a fault and stops further
capture. No silent splice over dropped time. Control polling finalizes the valid prefix
and displays a warning; zero frames produce no file/clip. Driver dropout bits are
forwarded before capture. Counters report accepted frames, missing/dropped blocks,
discontinuities and non-finite input. Disk write/finalization failure retains the
partial file and reports its path where available; no success clip is created.
A finalized file remains on disk if later clip attachment/graph preparation fails.
Device/graph reconfiguration and project/Undo/file edits are blocked while recording;
monitor controls remain realtime-safe.

32 disk voices / 256 MiB pages and 128 clips/retained sources remain shared project
limits. Start reserves one mono disk voice (256 KiB pages) for a long take.
Recording adds one fixed 1 MiB capture ring and one 32 KiB worker staging buffer.
RIFF data size is bounded below 4 GiB; no RF64, resampler, stereo/multiple input
recording, loop/punch/take management, automatic input/output-latency compensation,
asset consolidation or crash repair UI. Arm/monitor are runtime state, not archive data.

## Validation
audio_recording checks raw float32 roundtrip, backing/monitor separation, idle/pause,
monitor toggle without capture loss, zero RT allocation, flagged/null input, discontinuity,
overflow, timeline limit, non-finite sanitizing, ring wrap/drain, existing-file protection
and empty takes. desktop_recording checks arm/input/offline guards, retained device,
nonzero capture start, Pause/Stop, one-step Undo/Redo, blocked changes, dropout prefix,
import/edit/record/graph/unknown-chunk project save/load and centered playback.
Offline Windows GUI smoke covers recording controls and Arm/Disarm alongside menu/DPI.
Physical ASIO acceptance remains manual: MRS_STAGE_1D_CHECKLIST.md.


Read-ahead follow-up: priming now reads atomic published page tags without claiming
callback pins. The earlier control scan could briefly make a ready page unavailable
to concurrent render. Streaming regression repeats concurrent seeks and keeps exact
sample, zero-underrun and zero-allocation assertions. Recording driver Stop failure
closes the backend before draining/attaching the take, reports disconnection and
requires an explicit reconnect; simulated driver failure is covered by desktop_recording. A lost streamed backing
source at End rec is tested: capture is detached and the WAV finalized before
playback rebuild can fail; the saved take/reference survives with an error.


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
