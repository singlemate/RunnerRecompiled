# Виртуальный vblank и сброс GPU semaphore

2026-10-22. Продолжение DISPLAY-PERSISTENCE.md.

## Причина ожидания

gpu-irq-trace-v2-game.log подтвердил доставку обоих настоящих PM4interrupt(source1,cpu2) в guest callback82813F58. Очередь принимала запросы, handler входил и возвращался. Первый вызов с argument200 сбрасывал semaphore1→0; второй с argument264 оставлял1. Потеря IRQ не подтвердилась.

Оригинальный sub82814688 при более позднем target vblank помещает работу в очередь device+16572, увеличивает producer device+16704. sub82814570, вызываемый из GPU callback для source0, продвигает guest vblank counter, разбирает эту очередь и сам записывает0 в mailbox+4 при наступлении target. У headless runtime source0 ранее отсутствовал.

## Реализация

Добавлен MeGpuVblankWorker: монотонные steady_clock deadlines с периодом16666667нс (гостевой профиль60Гц), пропуск пропущенных периодов без catch-up burst, CV stop/join и сохранение исключения. GPU bridge вызывает уже имеющийся pinned SDK PumpExternalVblank под общим execution_gate; SDK продвигает счётчик виртуального display clock и отправляет source0/cpu2 через существующий IRQ worker. GPU completion/serial и mailbox напрямую не меняются. Это виртуальная развёртка дисплея, не физический Android vsync и не свидетельство presentation.

Clock включён только после успешного GPU setup в диагностическом GPU режиме. Его process-lifetime instance завершается вместе с probe через _exit/timeout. Нормальный lifecycle приложения, pause/resume, переключение частоты и связывание с Android Surface ещё не реализованы.

IRQ trace ограничен первыми16вызовами и степенями2. Снимки mailbox берутся под guest gate, входные и выходные значения наблюдаются без изменения. Самый первый gpu-irq-trace-game.log не относится к новой сборке: начальный adb push отказал unauthorized, а запуск использовал предыдущий binary. После успешной повторной передачи выполнены v2 self-test и game; выводы о IRQ основаны только на v2.

## Проверка на Y700

vblank-self-test.log:641checks, exit0. Добавлены проверки дедлайнов до/на границе, отсутствия повторного due, пропуска9периодов без очереди catch-up, реальных двух host ticks, отказа повторного start и остановки worker без failure.

vblank-game.log: ограниченный35секундный прогон, внешний timeout/exit124. Source0 callbacks действительно исполнялись; trace calls256и512 подтверждает изменение semaphore1→0 внутри исходного handler. В штатном publication snapshot достигнуто WPTR1636/RPTR1636, затем опубликован1641 до остановки. Прежняя граница61/52 пройдена, snapshot counters не подменялись. Открыты TdMainMenu и TdMainMenu_Images; меню визуально не подтверждено. Android presenter отсутствует.

Более длинный прогон: work/startup/vblank-long-game.log,125секунд/внешний timeout124. Новых unimplemented_function_import, execution_fault или external_gpu_packet_fault в журнале нет. Подтверждено дальнейшее чтение TdMainMenu_Audio0 и Textures.tfc, многократное создание Vulkan pipelines с driver result0 и повторные swap/present requests. Это уже прохождение прежнего CPU/GPU ожидания, а не только начального блока31/31. Число запросов PRESENT или pipeline не равно FPS или числу корректно показанных кадров.

Следующий основной этап — интеграция GPU backend в Android Activity/Surface и создание presenter. Устойчивость длинных сессий и lifecycle остаются открытыми;125секунд не доказывают полную стабильность. Standalone probe по-прежнему сообщает NO PRESENTER.

Финальные метрики125сrun:89созданий pipeline и89driver result0;303генерации VdSwap,302сообщения NO PRESENTER. Последний штатный publication snapshot WPTR4651/RPTR4651. Все эти числа относятся к диагностическому запуску на Y700, не к отображённым кадрам.
