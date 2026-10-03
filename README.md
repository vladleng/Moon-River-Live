# Moon River Studio

**Moon River Studio** (`MRS`, рабочее сокращение `MR Studio`) — проект собственной performance-first DAW.

**Live Mode** — встроенный Performance / Show режим Moon River Studio, по роли близкий к Show Page в Studio Pro. Это не отдельное приложение и не отдельная продуктовая версия.

> Репозиторий называется `MR-Studio`; историческое имя — `Moon-River-Live`. Текущая архитектура охватывает всю Moon River Studio.

## Новый чат / новый разработчик

Начинать с [`docs/START_HERE.md`](docs/START_HERE.md).

Там зафиксированы:

- краткая архитектурная формула проекта;
- правило одного SHARED Core / Engine;
- роль встроенного Live Mode;
- порядок чтения документации;
- карта Issues;
- правила параллельной разработки;
- критические вещи, которые нельзя переизобретать или дублировать.

После `START_HERE.md` открыть актуальный Stage issue, над которым продолжается работа.

## Текущая реализация

**SHARED Stage 0 / issue #15** принят после Windows-checker, PR #36 слит в main.

**SHARED Stage 1 / issue #16** интегрирован через PR #37: общий realtime renderer,
родной vendor ASIO через PortAudio, WAV preload/playback, мониторинг и метрики.
Базовые hardware-тесты WAV и input monitoring при 48k/128 пройдены.
Оставшиеся длительные тесты и Studio Pro benchmark отложены пользователем;
performance gate остаётся pending и не блокирует дальнейшую разработку.
Это общий C++20 backend: Project Model, Command/Undo, Transport API,
tempo/meter contracts, подписки, fixtures и versioned snapshot.

- [Core contracts](docs/CORE_CONTRACTS.md) — API, ownership, threading и границы этапа.
- [Проверка Windows-сборки](docs/SHARED_STAGE_0_CHECKLIST.md) — консольный checker.
- [Audio core](docs/AUDIO_CORE.md) — realtime/device contracts и границы прототипа.
- [Проверка ASIO](docs/SHARED_STAGE_1_CHECKLIST.md) — guided Windows tester и benchmark.
- [Статус реализации](docs/IMPLEMENTATION_STATUS.md) — продолжение работы.

**SHARED Stage 2 / issue #17** принят пользователем, PR #38 интегрирован: Musical Timeline, Chord/Arranger lanes,
общие context/navigation services, snapshot v2 с чтением v1.
[Musical contracts](docs/MUSICAL_TIMELINE.md) · [Windows checker](docs/SHARED_STAGE_2_CHECKLIST.md).

**SHARED Stage 3 / issue #18** принят пользователем, PR #39 слит: общая MIDI/processor инфраструктура, native gain,
подготовленный graph и patch-state. VST3 host и hardware MIDI — будущие adapters.
[Contracts](docs/MIDI_PROCESSOR_GRAPH.md) · [Windows checker](docs/SHARED_STAGE_3_CHECKLIST.md).

### SHARED Stage 4 — persistence/state/recovery
Stage 4 was accepted and merged through PR #40. It adds one versioned project
archive for all workspaces, show references, legacy migration, preserved unknown
chunks, safe save/backup, background autosave and stopped recovery.
See [persistence contracts](docs/PERSISTENCE.md) and
[Windows acceptance checklist](docs/SHARED_STAGE_4_CHECKLIST.md).
Build normally, then run mrs_persistence_check (no audio hardware required).

MRS Stage 0 / #20 принят пользователем 2026-10-03, PR #41 слит в main.
Подтверждены воспроизведение WAV и восстановление ASIO после смены файла и перезапуска.
Следующий этап — MRS Stage 1 / #21 Audio Arrangement.
Полный ASIO performance gate (#16) остаётся pending и не блокирует разработку.

## Основная концепция

```text
Moon River Studio
├── Arrange
├── Edit
├── Mix
├── Project
└── Live
```

Один project используется и для production, и для performance.

```text
                    Moon River Studio
                           |
        +------------------+------------------+
        |                  |                  |
     Arrange              Mix               Live
        |                  |                  |
        +------------------+------------------+
                           |
                       SHARED CORE
                           |
        +------------------+------------------+
        |                  |                  |
   Audio Engine           MIDI             Plugins
        |                  |                  |
       ASIO            MIDI I/O             VST3
```

Live Mode не имеет отдельного ASIO engine, Transport, MIDI engine, plugin host или project copy.

## Основные направления MRS

- audio tracks/clips/events;
- recording/playback;
- MIDI tracks/editor;
- mixer, buses and routing;
- VST3 hosting;
- native DSP;
- automation;
- tempo/meter map;
- Chord Track;
- Arranger Track;
- markers;
- project save/load/recovery;
- AI / ChatGPT integration;
- Live Mode.

## Live Mode

- setlists;
- moving Chord Track strip;
- current/next section;
- cues/markers;
- transport;
- click/cue;
- live inputs;
- patches;
- MIDI automation;
- foot control;
- preflight/recovery;
- remote/mobile companion.

Live Mode появляется на определённом этапе развития MRS, а затем может развиваться параллельно с production-функциями на том же SHARED Core.

## Performance-first

Audio performance — blocking requirement.

На Windows основной professional/live path должен поддерживать прямую работу через родной vendor ASIO driver аудиоинтерфейса.

Критические принципы:

- realtime audio thread отделён от UI/network/AI/file I/O;
- no blocking file/network/UI work in audio callback;
- preload/read-ahead;
- plugin latency accounting;
- low-latency monitoring path;
- xrun/dropout diagnostics;
- benchmark относительно Studio Pro на одинаковой конфигурации.

Подробнее: [`docs/AUDIO_ENGINE.md`](docs/AUDIO_ENGINE.md).

## AI / ChatGPT

MRS проектируется так, чтобы AI мог работать со структурированным Project Model через Context/Tool API.

Будущие возможности:

- понимать tracks/clips/MIDI/chords/sections;
- анализировать аранжировку и mixer state;
- генерировать и редактировать MIDI партии;
- предлагать изменения;
- после разрешения пользователя выполнять project commands;
- управлять DAW естественным языком.

AI не является частью realtime audio path и при его недоступности DAW/Live Mode продолжают работать нормально.

Подробнее: [`docs/AI_INTEGRATION.md`](docs/AI_INTEGRATION.md).

## Native DSP / IR / Amp modeling

Планируется возможность встроенных processors:

- EQ/compressor/saturation;
- convolution/Cab IR;
- amp/preamp/pedal DSP;
- neural model player;
- собственные Moon River captures/models.

Factory Content и User Library должны быть лицензированно разделены.

Подробнее: [`docs/DSP_MODELING.md`](docs/DSP_MODELING.md).

## Fender Studio Pro

Studio Pro больше не является обязательным authoring environment.

Он остаётся:

- performance/UX reference;
- возможным import/migration source;
- optional compatibility target для существующих проектов.

Compatibility/import ведётся отдельно в issue #3 и не блокирует основную разработку MRS.

## Организация разработки

Используются три issue track:

```text
[MRS]    DAW features/workspaces
[SHARED] common Core / Engine
[LIVE]   embedded Live Mode
```

Это три потока разработки **одного приложения**, а не три продукта.

Версионируется только Moon River Studio. Live Mode имеет Stage readiness и входит в соответствующие MRS builds.

Live UI может использовать mock SHARED services до готовности real backend, что позволяет вести работу параллельно.

Подробнее: [`docs/DEVELOPMENT_TRACKS.md`](docs/DEVELOPMENT_TRACKS.md).

## GitHub Roadmaps

- #1 — master roadmap;
- #12 — MRS roadmap;
- #13 — SHARED Core roadmap;
- #14 — Live Mode roadmap;
- #15–#19 — SHARED stages;
- #20–#27 — MRS stages;
- #28–#35 — LIVE stages;
- #3 — Studio Pro compatibility/import.

## Документация

- [`docs/START_HERE.md`](docs/START_HERE.md) — обязательная точка входа для новых чатов/разработчиков.
- [`docs/MOON_RIVER_STUDIO_VISION.md`](docs/MOON_RIVER_STUDIO_VISION.md) — целевая концепция MRS.
- [`docs/PROJECT_VISION.md`](docs/PROJECT_VISION.md) — общее видение продукта.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — единый Core и workspaces.
- [`docs/AUDIO_ENGINE.md`](docs/AUDIO_ENGINE.md) — ASIO, realtime rules и performance benchmark.
- [`docs/AI_INTEGRATION.md`](docs/AI_INTEGRATION.md) — ChatGPT/OpenAI integration, Context/Tool API и permissions.
- [`docs/DSP_MODELING.md`](docs/DSP_MODELING.md) — native DSP, Cab IR, amp/preamp/pedal и neural models.
- [`docs/DEVELOPMENT_TRACKS.md`](docs/DEVELOPMENT_TRACKS.md) — параллельные MRS/SHARED/LIVE issue tracks.
- [`docs/UI_UX_CONCEPT.md`](docs/UI_UX_CONCEPT.md) — UI/UX-концепция Live Mode.
- [`docs/DATA_MODEL.md`](docs/DATA_MODEL.md) — Project Model / musical/live data.
- [`docs/STUDIO_PRO_INTEGRATION.md`](docs/STUDIO_PRO_INTEGRATION.md) — optional Studio Pro compatibility/import track.
- [`docs/LIVE_WORKFLOW.md`](docs/LIVE_WORKFLOW.md) — live workflow внутри MRS.
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — общий roadmap.

## Roadmap в одном экране

| MRS version / stage | Основная цель |
|---|---|
| MRS 0.1 | Foundation + Core contracts + ASIO performance gate |
| MRS 0.2 | Audio Arrangement |
| MRS 0.3 | Mixer / Routing |
| MRS 0.4 | VST3 / Native DSP |
| MRS 0.5 | MIDI |
| MRS 0.6 | Chord/Arranger/Musical Structure + first native Live integration |
| MRS 0.7 | AI Foundation |
| MRS 0.8+ | Advanced DAW / Reliability |

Live Mode начинается как UI prototype после базовых SHARED contracts и далее развивается параллельно через LIVE Stage 0–7.

## Рабочее название

`Moon River Studio` / `MR Studio` — рабочее название всей DAW. Финальное название может быть изменено на поздней стадии.

## Лицензия

Лицензия проекта пока не определена.


## MRS Stage 0 — DAW Foundation
Native C++20/Win32 desktop shell with Arrange/Edit/Mix navigation, shared
ProjectStore/EngineTransport/GraphStore, timeline/playhead, Open WAV/project,
Save/Undo and vendor ASIO settings. Starts in an explicit silent offline mode.
[Desktop contracts/stack](docs/DESKTOP_FOUNDATION.md) ·
[Windows acceptance](docs/MRS_STAGE_0_CHECKLIST.md).
ASIO Actions artifact: MR-Studio-MRS-Stage-0-ASIO-Windows; run MoonRiverStudio.exe.
Edit/Mix are initial read-only views; detailed editors arrive in their own stages.

## MRS Stage 1a — Audio Arrangement
First slice of #21: New project, audio track create/delete/reorder, batch WAV import
into the current project, per-channel waveforms and zoom/scroll. Uses the same
shared ProjectStore/Undo/AudioEngine and retained ASIO connection for stopped edits.
[Contracts](docs/AUDIO_ARRANGEMENT.md) · [Windows checklist](docs/MRS_STAGE_1A_CHECKLIST.md).
Stage 1a accepted by user on 2026-10-03; PR #42 merged into main.
Stage 1b (0.1c) accepted by user on 2026-10-03; PR #44 merged into main.
Stage 1c (0.1d upd1 fix1) accepted; recording is Stage 1d / 0.1e.

## Версии сборок
Правила пользователя: 0.1b, 0.1c и далее; upd1/upd2 для небольших обновлений,
fix1/fix2 для ошибок. Stage IDs сохраняют структуру плана.
[Правила и текущее соответствие](docs/VERSIONING.md).
**0.1b fix1** принят пользователем и интегрирован через PR #43.
Принятый подэтап: **0.1c / MRS Stage 1b** — выбор, перемещение, обрезка и разделение клипов,
удаление отдельного клипа и общий Undo/Redo. UI drag preview, snap 1/16, сохранение позиции Pause.
[Windows checklist](docs/MRS_STAGE_1B_CHECKLIST.md). Пользователь подтвердил все функции 2026-10-03; PR #44 слит в main.
Принят **0.1d upd1 fix1 / MRS Stage 1c** — disk read-ahead, UI follow-up и моно L/R.

## MRS Stage 1c — 0.1d disk read-ahead
Long WAVs use bounded background disk buffers in the same SHARED AudioEngine.
Per-voice offsets, seek/loop priming and separate disk underrun/error counters;
waveform peaks build from bounded blocks with cancellation. Small WAVs preload.
[Contracts](docs/AUDIO_ARRANGEMENT.md) · [Windows checklist](docs/MRS_STAGE_1C_CHECKLIST.md).
PR #45; базовая 0.1d проверена пользователем. upd1 интерфейса и fix1 моно также приняты, PR #45 слит в main. Whole #21 stays open for recording/save-load acceptance.

## 0.1d upd1 fix1 — UI follow-up
Neutral gray background/buttons; thin Files menu for project/WAV actions;
Arrange/Edit/Mix navigation without Live button. Mono routes to the selected main pair.
[Acceptance checklist](docs/MRS_STAGE_1C_UPD1_CHECKLIST.md). All six CI jobs passed; user accepted UI/mono follow-up on 2026-10-03; PR #45 merged.
Live is a separate show mode with .mrlive documents referencing .mrsproject songs,
using the same SHARED Core/Engine. Its file commands/screen belong to LIVE stages.

Следующий подэтап: **Stage 1d / 0.1e** — запись, мониторинг и итоговая приёмка save/load.
Stage 1c завершён; общий #21 остаётся открытым. 0.1e принят пользователем; PR #46 слит в main.

## MRS Stage 1d — 0.1e recording/monitor
Один вход ASIO, одна вооружённая audio track, raw mono float32 WAV через bounded
фоновой писатель. Record/Arm track/Monitor в общем транспорте; завершение дубля,
Undo/Redo и сохранение/открытие проекта с внешним WAV. Существующие клипы слышны
во время записи; файл содержит только вход. При dropout сохраняется валидная часть
с предупреждением. Без loop recording и компенсации задержки в этой версии.
[Контракты](docs/RECORDING.md) · [Windows checklist](docs/MRS_STAGE_1D_CHECKLIST.md).
Принятая ASIO сборка: MR-Studio-0.1e-ASIO-Windows. Пользователь подтвердил работу;
Stage 1 / #21 завершён. Далее Mixer / Routing #22.


## Приёмка 0.1e — 2026-10-03
Пользователь подтвердил: «Все работает, записал на несколько каналов».
MRS Stage 1d / 0.1e принят; PR #46 слит в main. Подтверждение относится к текущему foundation workflow;
одновременная запись нескольких ASIO inputs не добавлялась (один выбранный input
и одна вооружённая дорожка за дубль). Весь MRS Stage 1 / #21 принят.
Проверенный code head: f78e0123cc7651f3418f0a42f8bc9fce861dfed7.
PR CI: все шесть jobs пройдены (56/56 Linux/ASIO, 57/57 Windows offline).
Следующая работа после паузы: MRS Stage 2 / #22 Mixer / Routing; реализация не начата.
Пользователь попросил продолжить 2026-10-04 по Asia/Krasnoyarsk. 


## 0.1e upd1 fix1 — folders and portable projects
Windows content root: Documents/MR Studio with Projects and Lives.
Each project owns <name>/<name>.mrsproject, Media and Mixdown. Imported WAVs
copy into Media; recordings write there. Archives use relative Media/... references.
Save As copies Media/Mixdown and retains Undo/device continuity; original project
and source files remain intact. Move the whole folder, then reopen its .mrsproject.
[Contracts](docs/PROJECT_FOLDERS.md) · [Windows checklist](docs/MRS_PROJECT_FOLDERS_CHECKLIST.md).
PR #47, acceptance pending. Lives prepares storage for future .mrlive workflow;
Mixdown prepares storage for later export. Mixer/Routing #22 follows after upd1.


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
