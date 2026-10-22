# Общая память, RingBuffer и службы host — 21 октября 2026

Добавлен прямой GPU-view **того же memfd**, который уже используется загрузчиком для физических алиасов игры. Это не копия памяти. В loader впервые связан настоящий rex::memory::RingBuffer из исходника SDK. Полный CommandProcessor пока не подключён; VdInitializeEngines не возвращает фиктивный успех.

## Фактически проверено на Y700

121 проверка loader self-test прошла в ARM64-процессе (device-shared-ring-self-test.log). Новые проверки подтверждают: запись из гостевого алиаса видна в raw-view; запись в raw-view видна в гостевом алиасе; RingBuffer читает big-endian слова из этой памяти, переходит через конец кольца и обновляет read offset; writeback указателя виден в CPU-алиасах; неверные диапазоны, alignment, ring-size и read-index отклоняются; после освобождения resolve больше не возвращает буфер.

Это проверка транспорта и общего backing store, **не интерпретация PM4 или исполнение GPU-команд**. Значения в кольце синтетические. `MeGpuPhysicalView` не владеет выделенным диапазоном: вызывающая сторона должна удерживать execution_gate и сохранять allocation живым до окончания доступа. Снимки не заменяют память, а отсутствие write-watch/асинхронной когерентности по-прежнему ограничивает подключение GPU worker.

Raw-view резервирует 512 МиБ виртуальных адресов с PROT_NONE; только выделенные физические диапазоны открываются RW и закрываются при освобождении. Файл разделяется с A/C/E views, поэтому дополнительной копии или заполнения 512 МиБ RAM нет. Фактический доступ ограничен таблицей физических allocation. Для C++23 типов SDK обновлён build.ps1 загрузчика; игровой AOT-архив остаётся прежним, PPCContext по-прежнему XenonRecomp, reinterpret_cast к RexGlue не вводился.

После изменения повторён реальный запуск игры (device-shared-ring-game.log): сохранены прежние выделение памяти и остановка на VdInitializeEngines. Игра ещё не передавала команды в новый транспорт, изображение отсутствует.

## Дополнительная сборка RexGlue

Успешно скомпилирована 31 дополнительная единица: VMA, Vulkan submission/upload utilities, provider/drawer, host threading/clock/timer queue, ring buffer, formats/textures. В сумме строгая линковка 80 объектов и библиотек оставляет 16 символов вместо прежних 86 (link-integration-extended-result.json). Линкер по-прежнему возвращает ошибку: готовый plugin.so НЕ получен.

Остались интерфейсы Memory/MMIO/invalidation callbacks, XHostThread/XThread/XObject, ExecuteInterrupt, VdQueryVideoMode и два UI/window метода. Это границы runtime, а не отсутствующие GPU-алгоритмы, которые можно безусловно заменить нулём. Их нужно адаптировать к уже исполняющей игру памяти и потокам.

## Android platform patch и проверка host-служб

Добавлен отсутствовавший main_android.h с реальным android_get_device_api_level; он подключён в memory_posix.cpp. В трёх ветках PosixConditionBase Android использует стандартный mutex/condition-variable путь вместо отсутствующих в Bionic robust pthread mutex APIs. Обычная синхронизация поддержана; восстановление mutex после смерти владельца этим патчем не реализовано.

Отдельный ARM64-процесс host_services_check с **реальным кодом RexGlue**, без заглушек, выполнил 10 проверок: обнаружение API 36, создание native host-thread, передача сигнала событием, ожидание завершения thread, auto-reset и timeout, callback очереди таймеров на std::jthread. Результат device-host-services.log. Это отдельная проверка host-слоя SDK; она не заменяет Xbox thread API нашего loader и не проверяет принудительное завершение владельца mutex.

Патч rexglue-android-gpu.patch обновлён. compile_integration_support.py, link_host_services_check.py и audit_integration_link.py добавлены в GPU build.ps1. Аудит сохраняет ожидаемую ошибку полной линковки в JSON; успешное завершение скрипта аудита означает сформированный отчёт, а не готовый плагин.

## Следующий необходимый шаг

Ввести явный GPU host-контракт вместо зависимости бэкенда от отдельного KernelState/Memory: общий memory view, запись/инвалидация и MMIO; запуск GPU host-worker; доставка interrupt в собственный PPCContext с явным ABI-переходом. Затем связать этот контракт с CommandProcessor и проверить реальную обработку packets/rptr writeback. Изученный ExecutePrimaryBuffer в SDK при ошибке пакета всё ещё возвращает входной write_index; перед подключением нужно исключить фиктивное подтверждение обработки такого пакета. В этом этапе этот upstream код не менялся и не исполнялся.

APK не менялся. Проверочные процессы завершены. Игровой бинарник и приватные данные остаются в work и на личном устройстве; здесь только исходники адаптации и отчёты.
