# 0.1e upd1 fix1 — папки студии и проектов

Windows ASIO сборка MR-Studio-0.1e-upd1-fix1-ASIO-Windows. Распаковать, запустить
MoonRiverStudio.exe. Предыдущая принятая 0.1e сохраняется для сравнения.

1. Files → Open studio folder: откроется Документы/MR Studio с Projects и Lives.
   Saved ASIO settings и восстановление соединения работают как прежде.
2. Files → New project: выбрать имя, например Test в Projects.
   Получится Projects/Test/Test.mrsproject; рядом Media и Mixdown.
   Отмена выбора имени оставляет текущий проект на месте.
3. Import WAVs: оригиналы останутся в выбранном месте, копии появятся в Media.
   Импортировать два WAV с одинаковыми именами из разных папок: оба сохранятся.
   Save. Закрыть проект, временно переименовать/переместить исходные WAV и повторно
   открыть Test.mrsproject: воспроизведение работает по внутренним копиям.
4. Добавить/вооружить дорожку, выбрать ASIO input, Record. Take WAV появляется в
   Media, без новой Audio папки. End rec/Pause/Stop и Undo/Redo работают.
5. Save, закрыть, перенести ВСЮ папку Test в другое место и открыть файл проекта
   из нового места. Импортированные и записанные клипы, waveform и edits доступны.
6. Pause/Stop → Save As с новым именем Copy: отдельная папка Copy, собственные
   Media/Mixdown и Copy.mrsproject. Исходная папка Test остаётся на месте.
   Undo/Redo после Save As не требуют старых оригиналов. Для проверки перенести
   исходную папку Test и повторно открыть Copy: аудио продолжает работать.
7. Открыть старый .mrsproject из 0.1e с внешними WAV/Audio, затем Save:
   сохраняется папка проекта с Media-копиями, старые файлы не удаляются.
8. Обычный Save в уже подготовленном проекте доступен при Play. Save As и
   первичное копирование медиа требуют Pause/Stop. При записи Save блокируется,
   Close завершает дубль перед предложением сохранить.
9. Mixdown сейчас пустая папка для будущего экспорта. Lives — папка для будущих
   .mrlive шоу; создание Live-документов не включено в эту доработку.
10. Проверить Unicode имя проекта, 100/125/150% scaling, файловые диалоги и меню.
    Не выбирать существующее другое имя проекта: overwrite не выполняется.

Итоговая структура: MR Studio/Projects/<имя>/<имя>.mrsproject + Media/ + Mixdown/.
Переносить всю папку проекта целиком; .mrsproject отдельно не содержит WAV.


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
