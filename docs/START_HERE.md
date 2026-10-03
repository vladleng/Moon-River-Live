# Moon River Studio — START HERE

## Актуальное продолжение — 2026-10-03
Stage 0 принят, PR #36 слит. Stage 1/2 интегрированы через PR #37/#38 в main.
Базовые ASIO WAV/monitoring при 48k/128 проверены пользователем. Оставшиеся
hardware/performance тесты отложены пользователем и не блокируют разработку;
gate остаётся pending в #16.
Stage 2 #17 принят. Snapshot schema 2 с чтением v1.
Stage 3 #18 принят после Windows-checker, PR #39 слит, issue закрыт.
Stage 4 #19 принят пользователем, PR #40 слит в main, issue закрыт.
MRS Stage 0 #20 принят пользователем 2026-10-03; PR #41 слит в main.
Пользователь подтвердил фикс восстановления ASIO после открытия другого WAV и перезапуска.
MRS Stage 1a #21 принят пользователем 2026-10-03; PR #42 слит в main.
0.1b fix1 принят пользователем 2026-10-03; PR #43 слит в main.
MRS Stage 1b / 0.1c принят пользователем 2026-10-03: все функции работают; PR #44 слит в main.
MRS Stage 1c / 0.1d upd1 fix1 принят пользователем 2026-10-03; PR #45 слит в main.
Приняты длинные WAV/disk read-ahead, серый интерфейс/меню Files, удаление кнопки Live
и центрирование моно. Все шесть CI jobs пройдены.
MRS Stage 1d / 0.1e принят пользователем 2026-10-03: «Все работает, записал на несколько каналов».
Весь MRS Stage 1 / Audio Arrangement #21 завершён. PR #46 принят и слит в main.
Последняя принятая сборка: 0.1e. Code head f78e0123cc7651f3418f0a42f8bc9fce861dfed7.
Контракты: docs/RECORDING.md; приёмка: docs/MRS_STAGE_1D_CHECKLIST.md.
Текущая небольшая доработка по запросу пользователя: 0.1e upd1 fix1, PR #47.
MR Studio/Projects/<имя>/<имя>.mrsproject + Media/Mixdown; MR Studio/Lives для будущих show.
Импорт и запись принадлежат Media; relative media refs, перенос папки и Save As с копиями.
Контракты: docs/PROJECT_FOLDERS.md; приёмка: docs/MRS_PROJECT_FOLDERS_CHECKLIST.md.
upd1 fix1 ещё не принят пользователем. Последняя принятая версия: 0.1e.
После upd1: MRS Stage 2 / #22 — Mixer / Routing. Разработка микшера ещё не начата.
При возобновлении сначала прочитать issue #22; выбрать первый подэтап микшера:
track gain/pan, mute/solo, meters и master bus на том же SHARED engine.
Запись 0.1e: один выбранный mono ASIO input и одна вооружённая дорожка за дубль;
последовательные дубли на разных дорожках поддерживаются. Multi-input recording остаётся будущей работой.
Правила версий: docs/VERSIONING.md. Длительные performance проверки #16 остаются
отложенными и nonblocking. Продолжить по запросу пользователя 2026-10-04 (Asia/Krasnoyarsk).



Этот документ — короткая точка входа для нового чата, разработчика или агента, который подключается к проекту без контекста предыдущих обсуждений.

## 1. Что мы строим

**Moon River Studio** (`MRS`, рабочее сокращение `MR Studio`) — собственная performance-first DAW.

Рабочее название не является окончательным и может быть изменено ближе к зрелой стадии проекта без изменения архитектуры.

**Live Mode** — встроенный Performance / Show режим Moon River Studio, по роли близкий к Show Page в Fender Studio Pro.

По уточнению пользователя 2026-10-03: Live — отдельный show-режим внутри MRS
с документом `.mrlive`, ссылающимся на `.mrsproject`. Это не кнопка workspace
рядом с Arrange/Edit/Mix. Show-экран и Files New/Open Live появятся в LIVE этапах.

Live Mode:

- не является отдельным приложением;
- не является отдельной DAW;
- не имеет собственной продуктовой версии;
- не имеет отдельного Audio Engine;
- не имеет отдельного Transport;
- не имеет отдельного MIDI Engine;
- не имеет отдельного plugin host;
- не хранит отдельную копию проекта для обычного workflow.

Главная формула:

```text
Moon River Studio
│
├── Arrange
├── Edit
├── Mix
└── Live Mode
        │
        └── тот же Project Model
            тот же Transport
            тот же Audio Engine
            тот же MIDI backend
            тот же Plugin Graph
```

## 2. Главное архитектурное правило

В Moon River Studio существует **одно общее ядро / SHARED Core**.

```text
                    SHARED CORE
                         │
       ┌─────────────────┼─────────────────┐
       │                 │                 │
  Arrange / Edit        Mix            Live Mode
```

В SHARED Core входят:

- Project Model;
- Command / Undo;
- Transport;
- Audio Engine / ASIO;
- mixer/routing graph;
- MIDI model/backend;
- plugin/native processor graph;
- Musical Timeline;
- Tempo/Meter Map;
- Chord Track;
- Arranger Track;
- Markers/Cues;
- serialization/versioning;
- persistence/recovery foundation.

**Нельзя создавать отдельный backend специально для Live Mode**, даже если это кажется быстрее для конкретной задачи.

Если backend-функция нужна нескольким workspace, она относится к SHARED Core.

## 3. Performance-first

Стабильность аудио — blocking requirement проекта.

На Windows основной professional/live path должен использовать родной vendor ASIO driver аудиоинтерфейса.

Базовые требования:

- realtime audio thread отделён от UI/network/AI/file I/O;
- no blocking I/O и тяжёлых allocations в audio callback;
- playback не зависит от UI responsiveness;
- plugin latency учитывается;
- предусмотрены preload/read-ahead;
- low-latency live path не должен ломаться из-за тяжёлого playback/mix path;
- ведутся xrun/dropout/callback metrics;
- Fender Studio Pro используется как performance benchmark на одинаковом hardware/setup.

Перед активным наращиванием функций Audio Engine должен пройти performance gate.

Подробнее: `AUDIO_ENGINE.md`.

## 4. Как организована разработка

Используются три **issue track одного приложения**:

```text
[MRS]    функции DAW и production workspaces
[SHARED] общий Core / Engine
[LIVE]   встроенный Live Mode
```

Это не три продукта.

Версионируется **Moon River Studio**.

Live Mode имеет только Stage readiness:

```text
Moon River Studio 0.6
├── Arrange / Mix / MIDI
├── Musical Structure
└── Live Mode: Stage 1 complete
```

Live Mode может разрабатываться параллельно на mock/fixture implementations SHARED interfaces. После появления real backend mock должен заменяться без изменения архитектуры Live UI.

## 5. GitHub Issues — карта проекта

Главные tracking issues:

- `#1` — общий master roadmap Moon River Studio;
- `#12` — MRS roadmap;
- `#13` — SHARED Core / Engine roadmap;
- `#14` — Live Mode roadmap;
- `#3` — optional Fender Studio Pro compatibility/import.

SHARED Core:

- `#15` — Core contracts: Project Model, Command/Undo, Transport API;
- `#16` — Audio Engine / ASIO / performance gate;
- `#17` — Musical Timeline: tempo, meter, chords, arranger, markers;
- `#18` — MIDI / Plugin Graph;
- `#19` — Persistence / State / Recovery.

MRS stages:

- `#20` — DAW Foundation;
- `#21` — Audio Arrangement;
- `#22` — Mixer / Routing;
- `#23` — Plugins / Native DSP;
- `#24` — MIDI;
- `#25` — Musical Structure;
- `#26` — AI Foundation;
- `#27` — Advanced DAW / Reliability.

Live Mode stages:

- `#28` — UX Foundation / moving Chord Track;
- `#29` — Real Project / Transport Integration;
- `#30` — Setlists / Show Workflow;
- `#31` — Playback / Click / Cue;
- `#32` — Live Inputs / Patches;
- `#33` — MIDI Automation / Hardware Control;
- `#34` — Remote / Mobile Companion;
- `#35` — Concert Reliability.

## 6. С чего начинать новому чату

Минимальный порядок чтения:

1. `docs/START_HERE.md` — этот файл;
2. `README.md` — краткий обзор всего продукта;
3. `docs/ARCHITECTURE.md` — архитектурные границы;
4. `docs/ROADMAP.md` — этапы разработки;
5. `docs/DEVELOPMENT_TRACKS.md` — правила параллельной работы;
6. открыть master issue `#1`;
7. открыть parent issue нужного track: `#12`, `#13` или `#14`;
8. открыть конкретный Stage issue, над которым продолжается работа.

Для audio/ASIO обязательно дополнительно прочитать:

- `docs/AUDIO_ENGINE.md`.

Для AI:

- `docs/AI_INTEGRATION.md`.

Для встроенного DSP/amp/cab:

- `docs/DSP_MODELING.md`.

Для Live Mode:

- `docs/UI_UX_CONCEPT.md`;
- `docs/LIVE_WORKFLOW.md`.

Для Studio Pro import/compatibility:

- `docs/STUDIO_PRO_INTEGRATION.md`;
- issue `#3`.

## 7. Что важно не перепутать

### Не создавать Moon River Live как отдельное приложение

Историческое имя репозитория — `Moon-River-Live`, но целевой продукт теперь Moon River Studio.

`Live Mode` — встроенный workspace.

### Не создавать отдельный Live Audio Engine

Live использует тот же engine, что Arrange/Edit/Mix.

### Не создавать отдельный Live project format для обычной работы

Live-specific данные хранятся как часть MRS Project Model либо как show/setlist state, ссылающийся на project IDs.

### Не делать Studio Pro обязательной зависимостью

Studio Pro теперь:

- UX/performance reference;
- benchmark;
- optional import/migration source.

Moon River Studio должна быть самодостаточной DAW.

### Не связывать AI с realtime thread

AI работает через Project Context API + Command/Tool API и никогда не вызывается из ASIO callback.

## 8. Live Mode — визуальный ориентир

Базовое направление уже выбрано:

- flat dark UI;
- без выпуклых/glossy элементов;
- высокая читаемость на ноутбуке;
- горизонтальная движущаяся Chord Track strip по принципу Show Page;
- current chord читается относительно playhead;
- previous/next chords остаются видимыми;
- sections/cues/setlist/transport постоянно доступны;
- performance safety важнее декоративности.

Live UI должен быть performance-oriented представлением того же открытого MRS project.

## 9. AI — архитектурная цель

Будущий AI/ChatGPT layer должен получать структурированное состояние DAW через API, а не управлять интерфейсом мышью.

Планируемые возможности:

- читать tracks/clips/MIDI/chords/sections/mixer/plugins;
- анализировать аранжировку;
- генерировать и редактировать MIDI;
- предлагать mixer/plugin changes;
- создавать markers/sections/clips;
- выполнять разрешённые commands с Undo/Redo;
- работать в режимах READ / SUGGEST / EDIT / AUTO.

Даже до реализации AI Project Model должен иметь stable IDs и нормальный Command API.

## 10. Native DSP / guitar processing

В долгосрочном плане MRS предусматривает:

- utility DSP;
- EQ/compressor/saturation;
- convolution/Cab IR;
- amp/preamp/pedal models;
- neural model player;
- собственные Moon River captures/models.

Factory Content и User Library должны быть лицензированно разделены.

## 11. Текущая стартовая логика разработки

Основной фундамент строится через SHARED + ранние MRS stages.

Критическая последовательность:

```text
#15 Core contracts
      │
      ├── #16 Audio Engine / ASIO
      ├── #17 Musical Timeline
      └── #20 MRS DAW Foundation
```

После фиксации нужных contracts Live UI может идти параллельно:

```text
#15 / #17 interfaces
        │
        └── #28 Live UX prototype on mocks
                    │
                    └── #29 real Core integration
```

Новые чаты не должны ждать завершения всей DAW, если текущую задачу можно безопасно вести через зафиксированный SHARED interface.

## 12. Как передать задачу следующему чату

Достаточный стартовый запрос:

```text
Ознакомься с docs/START_HERE.md, docs/ARCHITECTURE.md,
docs/ROADMAP.md и GitHub Issues #1, #12, #13, #14.

После этого открой issue #XX и продолжай работу над ним.
Не создавай отдельный engine или отдельное приложение для Live Mode.
```

Если работа идёт над Audio Engine, нужно добавить:

```text
Обязательно прочитай docs/AUDIO_ENGINE.md и соблюдай performance gate.
```

## 13. Source of truth

При противоречии старых обсуждений и текущего репозитория приоритет имеют:

1. актуальный Stage issue;
2. `docs/START_HERE.md`;
3. `docs/ARCHITECTURE.md`;
4. `docs/ROADMAP.md`;
5. специализированный документ соответствующей подсистемы.

Если обнаружено противоречие между актуальными документами, сначала исправить документацию и только затем продолжать реализацию.


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
