# Аудиопоток: suspended start, resume и IRQL

2026-10-21, Y700 test device. ExCreateThread поддерживает только CREATE_SUSPENDED(bit0) и guest-affinity mask в верхнем байте(bits0..5). Другие флаги явно останавливаются. В KTHREAD+BC хранится suspend_count, host-поток ждёт CV до resume, освобождая execution_gate. KeResumeThread принимает известный KTHREAD, уменьшает count и пробуждает; возвращает status0 как pinned SDK, неизвестный объект INVALID_HANDLE. Это первоначальный suspended start, не произвольное приостановление уже исполняющегося потока. NtResumeThread пока не реализован.

CPU выбирается как младший установленный бит маски и отражается в PCR+10C/KTHREAD+BF; Android affinity не меняется. Полноценная миграция между guest CPU и вытесняющий планировщик не реализованы.

Добавлены KeRaiseIrqlToDpcLevel (гостевой DISPATCH2 с возвратом предыдущего уровня) и KfLowerIrql (только понижение). Состояние в PCR+18; Android-прерывания не маскируются, автоматическая доставка APC/DPC при понижении пока не реализована.

В реальном запуске создан аудиопоток id7 с флагами10000001, guest CPU4, стек262144. Игра установила priority hint15, возобновила поток; он начал82829F18 и вошёл в ожидание объекта830DCC58. Главный поток прошёл raise/raised-lock/release/lower и достиг XAudioRegisterRenderDriverClient, LR82823DF0. GPU read/write31/31.

278 self-tests прошли:6новых проверок suspended start/resume/guest CPU/неизвестного объекта и3проверки IRQL. Проверено, что ребёнок не исполняется во время освобождения gate до resume, затем завершается после resume. Полная звуковая система этим не проверена.

Журналы device-suspended-explore.log, device-audio-thread-explore.log, device-audio-thread-self-test.log. Следующий шаг: контракт регистрации XAudio render client и периодического вызова гостевого callback; регистрация без работающего consumer не должна объявляться готовым звуком.
