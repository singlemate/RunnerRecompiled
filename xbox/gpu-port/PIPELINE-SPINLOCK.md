# Vulkan pipeline, GPU polling и spinlock

2026-10-21. Проверено на Lenovo Y700 test device через ADB.

Добавлена трассировка вызова vkCreateGraphicsPipelines и параметров PM4_WAIT_REG_MEM в external-host сборке SDK. Драйвер вернул VK_SUCCESS (0) для pipeline с игровым VS 1E6883FCCDE1F688. Это подтверждает создание pipeline, но не завершение всего командного блока.

Реализованы KfAcquireSpinLock/KfReleaseSpinLock: владелец записывается BE32 адресом PCR в гостевое слово; IRQL хранится в PCR+0x18, acquire поднимает его до 2 и возвращает предыдущее значение, release проверяет владельца и восстанавливает IRQL. Рекурсивное владение, неверные адреса и неизвестные уровни останавливаются. Гостевые обращения сериализованы execution_gate; при конкуренции ожидание освобождает gate. Это кооперативная модель, не аппаратное маскирование Android-прерываний. 4 новых проверки владения/IRQL вместе с существующими дают 246 прошедших проверок на планшете. Неправомерные пути и конкуренция нескольких владельцев отдельными тестами пока не проверены.

Для --gpu-explore добавлена уступка 1 мс через каждые 1024 scalar reads наблюдённого 32-байтного блока общей GPU-памяти, содержащего system identifier. Учитываются физические алиасы. Она не меняет значения памяти, не имитирует fence completion, не заменяет полноценный планировщик. Все 300 AOT модулей пересобраны с этим hook, результаты mmio-rebuild-result.json.

Без уступки CPU busy loop мог не дать исполниться IRQ-worker; результат зависел от планирования. С ней настоящий GPU callback приобрёл и освободил lock 1004F7C8 с owner PCR61030000. Далее получена контролируемая остановка KeAcquireSpinLockAtRaisedIrql, LR82813FA8, lock1004E118, thread61031000. Снимок read/write остаётся 28/31. Отсутствие SIGALRM в этом запуске подтверждает достижение следующей границы, но не устойчивость всей модели планирования.

Следующий шаг: raised-IRQL варианты spinlock, контракт уровня прерывания при dispatch GPU callback, затем повторная проверка WAIT_REG_MEM. Поскольку pinned SDK сам не устанавливает IRQL в ExecuteInterrupt, нельзя без проверки перенести предположение о конкретном значении IRQ-уровня.

Журналы: device-pipeline-trace.log, device-spinlock-explore.log, device-gpu-poll-explore.log, device-gpu-poll-self-test.log. Cumulative rexglue-android-runtime.patch обновлён, reverse apply --check прошёл.
