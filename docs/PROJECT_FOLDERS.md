# 0.1e upd1 fix1 — Studio/project content folders

Windows default content root: the Windows Known Folder Documents / MR Studio.
Known Folder lookup respects redirected Documents/OneDrive. Root contains Projects
and Lives. Files → Open studio folder opens this root. Internal config/logs keep
their existing LOCALAPPDATA location and saved ASIO preferences.

Projects/<name>/<name>.mrsproject lives in the project folder root; Media and
Mixdown are created with it. New project asks for its filename/location first.
Save picker selects <parent>/<name>.mrsproject and creates <parent>/<name>/.
Users can choose a different parent in the standard file picker. Opening existing
projects remains supported. Lives is reserved for future .mrlive shows; no fake Live
file workflow or second engine was added.

## Media ownership and portability
Import WAVs into a saved project copies each source into Media off the audio thread.
Duplicate source selections in a batch share one copy. Different sources with equal
filenames receive unique names; existing files/originals are never overwritten.
Record writes directly to Media/Take-<id>.wav. GUI asks for the project folder before
import/record into an unsaved project. Open WAV as new project also offers Save.

First save of legacy/unsaved projects consolidates current external WAVs into Media;
prior Audio recordings are copied, not deleted. Archive references become portable
UTF-8 Media/... relative paths with forward slashes. App-level unsaved imports remain
temporary external references until saved. No project/archive schema change.

Save As copies Media (including unused/Undo media) and Mixdown to a separate project
folder and writes relative references. Original project and originals remain intact.
No destructive media cleanup on Undo, track deletion or save. Missing current media,
identity mismatch and copy/write failures are explicit errors. Copies are staged with
unique partial files and published without overwrite. Failed pre-commit work removes
only files created by that operation; archive replacement retains existing safe-save
and backup semantics. Empty created directories may remain after a failed operation.

Runtime source keys/ProjectStore history stay stable. Controller aliases resolve their
new owned locations; long-source immutable metadata is rebound only off callback.
Successful consolidation/Save As retains paused/stopped position, loop and the same
device handle. Ordinary saves of already-owned media remain available during Play.
Consolidation/Save As require Pause/Stop in this foundation; no realtime file operations
or hot graph swap. Recorded takes and imported media use the same decoder/read-ahead.

Move the whole project folder to another location/computer, then open its .mrsproject;
the WAV references still resolve. External plugin/native state, unknown archive chunks,
clip edits and mixer model use existing persistence. External plugin libraries/
future IR presets are not automatically discoverable/copiable in this update; their
asset management belongs to the owning feature. Mixdown is storage groundwork,
not an implemented export command. Lives does not yet create or open .mrlive files.

## Validation
desktop_project_folders tests root/layout, Unicode filenames, duplicate/colliding
WAVs, rejected import rollback, original-file removal, relative archives, Save As
including unused Media/Mixdown, Undo/Redo, moved-folder reopen/playback, ordinary
save during Play, identity protection and long legacy source rebinding without its
original. GUI smoke checks the folder menu and root directories. Physical ASIO,
dialog behavior and recording remain manual acceptance.


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
