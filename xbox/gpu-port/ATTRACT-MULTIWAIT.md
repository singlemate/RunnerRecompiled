# Ресурсы Attract Movie и ожидание нескольких handles

2026-10-22. После XMA ContextClear и чтения текстур.

textures-game.log подтвердил чтение Textures.tfc: offset496173056/read131072, затем продолжение до запроса Attract_Movie.txt. Это чтение нужных диапазонов cache, не всего файла520МБ. Текстовый ресурс31байт и связанный BIK161892072байт извлечены из пользовательского SVOD и переданы на планшет. В attract-game.log оба открыты; следующая остановка — собственное ограничение NtWaitForMultipleObjectsEx(count2), caller829FB664, thread61091000. Открытие не подтверждает воспроизведение видео.

Повторные сообщения process_type/raised spinlocks ограничены первыми8вызовами и степенями2 с явным счётчиком, отдельно для каждого host thread. Проверки/результаты guest operations и остановки на ошибках сохранены. Это сокращение диагностического I/O, не изменение scheduler.

## Реализованное ожидание

Для count1 сохранён прежний путь single wait, включая mutants. Для count2..64 доступны events, thread completion и notification readiness. WaitAny возвращает индекс первого готового объекта; WaitAll проверяет готовность всех до consumption. Только auto-reset events сбрасываются; manual events, завершённые threads и notification queue сохраняются. Невалидный handle проверяется до изменений других объектов. При блокировании execution gate освобождается, state_changed используется для пробуждения.

Поддержаны нулевой, относительный timeout до прежнего60с лимита probe и бесконечный. Абсолютный timeout, alertable/APC, duplicates, file sets останавливают probe явно. Это не полный NT dispatcher/Object Manager.

591 self-test прошёл на Y700 (work/startup/multiwait-self-test.log, exit0): WaitAny index, atomic WaitAll без частичного auto-reset, invalid handle без consumption, измеренный timeout5мс, реальное пробуждение другим host thread, event+notification без удаления сообщения и64различных handles с индексом63.

Игровая проверка — work/startup/multiwait-game.log; бинарник145314776байт. Все игровые ресурсы остаются только в work и приватной папке планшета.

## Мутанты в общем наборе

Первый прогон multiwait-game.log остановился явно: среди двух handles есть mutant. Добавлены проверка свободного владельца/рекурсии и захват набора только после готовности всех объектов. WaitAll не сбрасывает event и не захватывает свободный mutant, пока другой занят. Переполнение рекурсии проверяется до изменений всего набора.

WaitAny возвращает 0x80+индекс при abandoned mutant; WaitAll — 0x80, если хотя бы один захваченный mutant abandoned. Условия готовности и побочные эффекты общего ожидания сверены с [документацией Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kewaitformultipleobjects) и закреплённым SDK src/system/xobject.cpp.

603 проверки прошли на Y700: work/startup/mutant-multiwait-self-test.log, probe_exit=0. Дополнительные случаи: частичное ожидание, рекурсивный захват, abandoned для Any/All, отсутствие consumption при переполнении и реальное пробуждение после освобождения другим host thread. Игровая проверка: work/startup/mutant-multiwait-game.log; результат фиксируется после завершения.
## Игровой результат расширения

mutant-multiwait-game.log: оба Attract Movie ресурса открыты, WaitAny над 0004000c/00040014 завершается статусом0, в журнале подтверждены повторения до call32. Прежняя остановка снята. Итог probe_exit=44 по диагностическому лимиту240с, без нового unsupported import или fault.

Главный поток ожидает event0001002c. Рабочий61091000 продолжает проходить разные участки82EB/82EC; последние снимки попадают в word_writable (символизация module_offset7e0cd4 текущего бинарника). Это наблюдение о стоимости валидации/работе CPU, не доказательство deadlock и не подтверждение воспроизведения. Следующий шаг — измерить этот путь, прежде чем менять ожидания или лимиты. В частности, диагностический dcbst проверяет32слова каждой128-байтовой линии через общий поиск областей; его стоимость требует измерения.