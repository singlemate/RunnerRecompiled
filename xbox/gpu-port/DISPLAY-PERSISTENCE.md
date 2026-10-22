# VdPersistDisplay: headless отказ и следующее ожидание GPU

2026-10-22. Это сохранение состояния дисплея, не система игровых сохранений.

## Наблюдавшийся контракт

Caller82815458 передаёт12нулевых байт на стеке и адрес output word. После вызова проверяет BOOL; только при true читает output и вызывает MmFreePhysicalMemory(type1). При false пропускает освобождение и продолжает flush. Закреплённый SDK возвращает1 с отдельным служебным physical allocation; это не доказательство фактического сохранения изображения.

У standalone probe нет presenter/удерживаемого дисплея. Для указанного callsite и нулевого descriptor реализован явный false, output остаётся неизменённым, память не выделяется. Ненулевой descriptor, другой callsite, неверные адреса или отсутствие готового GPU явно останавливают probe. Поддержка настоящего сохранения дисплея и физической памяти type1 остаётся открытой.

## Проверки

persist-display-self-test.log:630checks, exit0. Дополнительные проверки: BOOL false, сохранение output/argument, отсутствие physical allocation и изменения descriptor. Это проверка политики отказа, не display persistence.

persist-display-game.log подтверждает, что игра обработала отказ и продолжила MMIO публикацию: WPTR46/49/52/55/58/61. При последних publication snapshots RPTR остаётся52. CPU ожидает serial в sub82813C40/827FF5D0; backend последний раз сообщил WAIT_REG_MEM(info13,address1FBF3006,reference0). Это следующая исследуемая граница, а не доказанный отказ GPU.

Добавлено ограниченное наблюдение CPU poll: чтение6BE32слов из текущей writeback line и соседней предшествующей physical page, только если диапазоны принадлежат live allocations. Запись значений/подмена completion не выполняются; прежнее scheduling yield сохранено.

## Снятые значения и следующая работа

persist-display-game.log завершился по внутреннему240с лимиту, exit44. Повторный gpu-poll-memory-game.log ограничен внешним timeout35с и завершился124; это намеренный диагностический предел, не новая ошибка. Последний полный regression630checks/exit0: gpu-poll-memory-self-test.log.

На повторных snapshots CPU line1FBF4000 содержит serial11, cursorBF8F3F2C. Соседний32-байтовый allocation1FBF3000 содержит слова [0,1,0,0,0BADF00D,00000264]. Последний GPU wait info13/address1FBF3006/reference0/maskFFFFFFFF означает проверку BE32слова по выровненному адресу1FBF3004 (низкие2бита — endian mode); наблюдаемый value1 не удовлетворяет reference0. Эти две страницы — разные live allocations, а не доказанная ошибка физического alias. CPU-адреса E/F alias переводятся с4КиБсмещением, что совпадает с PhysicalHeap::GetPhysicalAddress закреплённого SDK.

Дальше требуется проследить, кто устанавливает/сбрасывает semaphore word1FBF3004, какие PM4interrupt и guest callbacks предшествуют этому ожиданию, и требуется ли реальный vblank source в текущем headless setup. Автоматический vblank пока отключён; его необходимость ещё не установлена. Нельзя снимать ожидание записью0 или искусственным увеличением serial.
