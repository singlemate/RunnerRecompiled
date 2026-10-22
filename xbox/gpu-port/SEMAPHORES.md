# Подсчётные kernel-семафоры

2026-10-21, Lenovo Y700 test device. Добавлены KeInitializeSemaphore, KeReleaseSemaphore и type5 в KeWaitForSingleObject. Гостевой объект20байт: type5, BE32 signal_state+4, BE32 limit+16. Допускаются count0..limit, limit1..INT32_MAX. Изменения сериализованы execution_gate. Успешное ожидание расходует ровно один токен; release возвращает предыдущий count и будит ожидающих. Нулевой/отрицательный adjustment и превышение limit останавливают диагностический загрузчик до изменения count. Механизм гостевого исключения STATUS_SEMAPHORE_LIMIT_EXCEEDED пока не реализован.

При блокировке ожидание освобождает gate. Поддержаны poll, ограниченный относительный timeout и indefinite wait. Absolute timeout/APC/Wait=true handoff по-прежнему не поддержаны. Priority increment не меняет приоритет host; справедливость очереди не гарантируется. NtCreateSemaphore/NtReleaseSemaphore и handle-семафоры пока остаются неподдержанными.

269 self-tests прошли на планшете, включая10новых проверок: два исходных токена, пустой poll, release до максимума, последовательное расходование трёх токенов, блокирующее ожидание с release из другого host-потока, timeout без расходования. Некорректные параметры проверяются кодом; отдельный тест guest-исключения не выполнялся.

В реальном запуске игра создала семафор830DCC44,count0,limit6 и продолжила до ExCreateThread с неподдержанными флагами, LR8282A320. GPU read/write31/31 сохранились. В месте вызова создаётся аудиопоток с affinity bits и другим режимом старта; следующий шаг требует разбора suspended/resume и CPU affinity, а не игнорирования флагов.

Журналы device-semaphore-self-test.log и device-semaphore-explore.log. APK и исходная копия игры не изменены.
