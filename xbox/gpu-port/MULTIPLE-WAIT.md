# Ожидание нескольких объектов и выход на PCM submit

2026-10-21, Lenovo Y700 test device. Добавлен KeWaitForMultipleObjects для1..64 различных гостевых объектов: events(type0/1), semaphores(type5), известных KTHREAD. WaitAll(type0) сначала проверяет доступность всего набора под execution_gate, только затем расходует токены/auto-reset события. WaitAny(type1) выбирает младший индекс готового объекта и расходует только его. Manual-reset events и завершённые threads остаются сигнализированными. Ожидание освобождает gate и использует общую condition_variable.

Поддержаны indefinite, poll и относительный timeout<=60сек. Alertable/APC, absolute timeout, дубликаты объектов, неизвестные типы и WaitBlockArray-семантика пока не реализованы; wait blocks не заполняются host-указателями. Передаваемый guest WaitBlockArray не используется нашим CV runtime. Fairness не гарантируется. Это kernel pointer API, не NtWaitForMultipleObjectsEx по handles.

RtlInitAnsiString создаёт8байтный descriptor (BE16 length/max,BE32 pointer), обрабатывает NULL и пустую строку, проверяет доступность до terminator, ограничивает length65534. Память не выделяется, source не копируется. Слишком длинные/недоступные строки останавливаются.

XAudioGetVoiceCategoryVolumeChangeMask возвращает0 только для нашего зарегистрированного клиента и доступного выхода. Category setter пока stop import, поэтому изменения volume не создаются. PCM не передаётся и не подтверждается этим запросом.

294 self-tests прошли (12новых): WaitAll без частичного расходования, смешанный manual event/semaphore, auto-reset consumption, WaitAny index/приоритет, блокирующее пробуждение из другого host-потока, timeout, завершённый thread, nonempty/NULL/empty ANSI string.

В реальном запуске AAudio STARTED и callback сохранены. Аудиообработчик больше не остановился на multiple-wait: выполнение дошло до XAudioSubmitRenderDriverFrame. Основной поток прошёл ANSI и остановился NtQueryFullAttributesFile, LR829FD05C, input700FF2D8. GPU31/31. PCM submit остаётся неподдержанным, слышимый звук не тестировался; callback return пока не подтверждён. Следующий шаг — проверенная конверсия planar6 BE float → stereo LE и передача через реальный AAudio sink с ограниченной очередью, без фиктивного consumption. Отдельный следующий блок — файловая система и сопоставление игровых путей.

Журналы device-multiple-wait-self-test.log, device-multiple-wait-explore.log.
