Текущий этап: [XEX resources и реальные файлы](gpu-port/STARTUP-RESOURCES-FILES.md). TOC и Coalesced.ini прочитаны; новая остановка NetDll_XNetStartup. 333 проверки, GPU31/31.

Текущий этап: [отсутствующий debug volume](gpu-port/DEBUG-VOLUME.md). ShaderDumpxe query пройден, новая остановка KeEnableFpuExceptions; 306 проверок, GPU31/31. Полная файловая система ещё не реализована.

Текущий этап: [PCM submit](gpu-port/PCM-SUBMIT.md). AAudio принял стартовые игровые кадры, callback вернулся. Следующая граница NtQueryFullAttributesFile; 303 проверки, GPU31/31.

Текущий этап: [multiple-wait и выход на PCM submit](gpu-port/MULTIPLE-WAIT.md). Новые границы XAudioSubmitRenderDriverFrame и NtQueryFullAttributesFile. 294 проверки; GPU31/31.

Текущий этап: [AAudio endpoint и XAudio callback](gpu-port/AAUDIO-CLIENT.md). STARTED и вход callback подтверждены; новые зависимости wait-multiple/ANSI/volume-mask. 282 проверки; PCM ещё не подключён.

Текущий этап: [аудиопоток и IRQL](gpu-port/AUDIO-THREAD.md). Suspended/resume прошли, следующая остановка XAudioRegisterRenderDriverClient. 278 проверок; GPU31/31.

Текущий этап: [kernel semaphores](gpu-port/SEMAPHORES.md). Игра создала аудиосемафор, следующая остановка — флаги ExCreateThread. 269 проверок прошли; GPU31/31.

Текущий этап: [XMA context memory](gpu-port/XMA-CONTEXTS.md). Игра получила настоящий адрес XMA-контекстов; GPU read/write31/31. Следующая остановка KeInitializeSemaphore. 259 проверок прошли.

Текущий этап: [raised-IRQL spinlock](gpu-port/RAISED-SPINLOCK.md). GPU callback освободил оба lock и дошёл до IssueSwap; следующая остановка XMA MMIO 7FEA1800. 252 проверки прошли.

Текущий этап: [Vulkan pipeline и spinlock](gpu-port/PIPELINE-SPINLOCK.md). Драйвер создал игровой pipeline; GPU callback прошёл обычный spinlock, следующая остановка KeAcquireSpinLockAtRaisedIrql. 246 проверок прошли.

Текущий этап: [генерация VdSwap и исполнение PM4](gpu-port/SWAP-PM4.md). Read/write достигли 28/28, следующий блок 31/28 начал создание игрового graphics pipeline и не завершился до таймера. 242 проверки прошли.

Текущий этап: [системные GPU-метаданные и вход VdSwap](gpu-port/SYSTEM-BUFFER.md). Параметры VdSwap получены на планшете; генерация swap-пакетов ещё не реализована. 242 проверки прошли.

Текущий этап: [защита физической памяти](gpu-port/PHYSICAL-PROTECT.md). Четыре игровых вызова пройдены; следующая остановка VdGetSystemCommandBuffer. 242 проверки прошли, изображения пока нет.

Текущий этап: [EDRAM/HSIO](gpu-port/EDRAM-HSIO.md). Игра продолжила запуск при HSIO=false; следующая остановка MmSetAddressProtect. GPU read/write=25/25, 242 проверки; изображения пока нет.

# Xbox 360 → Android ARM64: начало работ, 21.10.2026

Пользователь выбрал Xbox 360 как исходную платформу и подтвердил наличие копии. Файлы ещё не извлечены. default.xex не анализировался. Совместимость PC-save после смены платформы не подтверждена; прежние исследования PC ABI и PhysX нельзя переносить автоматически.

## Зафиксированная основа

Загружен rexglue/rexglue-sdk без submodules, commit c94f5ebdcb3c9d1a460ca48e04f9758448f8d518. Исходники находятся в рабочей папке work/references/rexglue-sdk, оригинал не изменён. Это SDK с генератором кода и runtime; отдельно устанавливать второй генератор на этом этапе не требуется.

В исходниках уже есть REX_PLATFORM_ANDROID и части Vulkan Android surface. Значит Android-код не нужно писать полностью заново. Однако CMake выбирает Linux-платформу, UI требует X11/XCB и Wayland, core ссылается на pthread/rt как отдельные библиотеки. Это места для адаптации, а не доказательство полного списка проблем.

## Проверка настоящего исходника

NDK r30, aarch64-linux-android35-clang++, C++23:

```powershell
& PATH_TO_NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android35-clang++.cmd -std=c++23 -IPATH_TO_REX/include -c PATH_TO_REX/src/core/fiber_posix.cpp -o fiber_posix.o
```

Компиляция исходного fiber_posix.cpp завершилась четырьмя ошибками: не объявлены getcontext (два вызова), makecontext, swapcontext. Полный вывод: fiber-compile.log. Это реальный барьер сборки компонента runtime, не синтетическая проверка арифметики. Полная сборка SDK не выполнялась; submodules, CMake и host-компилятор ещё не подготовлены.

## Следующая работа

- Выбрать Android ARM64 backend переключения fiber-контекстов с корректным сохранением стека/регистров и лицензией. Нельзя подменять кооперативные fibers обычными потоками без проверки семантики.
- Подготовить отдельную Android-конфигурацию: системные библиотеки Bionic, поверхность Android и lifecycle приложения, разделение host codegen и target runtime. Не маскировать Android под полноценный GNU/Linux.
- После получения пользовательских файлов Xbox: снять хеш default.xex, определить версию/Title ID/формат и импорты, затем запускать анализ. Получение C++ не считать запуском игры.

До этого не расширять демонстрационное приложение и не обещать игровую совместимость. Игровые файлы хранить отдельно от Git и APK; пользователь выбирает собственные данные при дальнейшем запуске.

Источники: https://github.com/rexglue/rexglue-sdk/tree/c94f5ebdcb3c9d1a460ca48e04f9758448f8d518

## Файлы пользователя проверены — 21.10.2026

Источник: D:\Mirror`s Edge _XBOX360. Формат LIVE/SVOD Games on Demand, 35 частей, 5 952 516 096 байт данных. Каталог прочитан: 1521 запись. Извлечён только корневой default.xex в work/xbox-game-private (вне репозитория проекта); исходный контейнер открыт только для чтения.

XEX2: 17 707 008 байт, SHA256 3ed882fd1900ff2bbccf1de745fb7e92c7d2168bb06069cbab311ac8eff28e1b. Title ID 45410850, Media ID 426CA5A2, version/base version 00000003. Entry point 829FAB98, image base 82000000 (значения заголовка). Encryption type 1, compression type 1. Таблицы заголовка перечисляют xboxkrnl.exe (326 записей) и xam.xex (236 записей); записи не равны числу уникальных API, ordinals/реализации ещё не сопоставлены. PPC-код ещё не декодирован и не рекомпилирован.

Хеш основного файла контейнера одинаков при двух чтениях. Полное дерево хешей SVOD и подпись не проверялись: это структурная инвентаризация, не проверка подлинности. TU обнаружено отдельно и не применялось. Данные/EXE не публиковались и не встраивались в APK.

Воспроизводимый инструмент: tools/inspect_god.py. Инвентаризация и хеши: xbox/game-inventory.json. Сторонний god2xex изучен в work/references, но не запускался: его безусловное удаление выходной папки и запись файлов не использованы. Для извлечения применён ограниченный собственный reader с проверками границ и отказом перезаписи отличающегося XEX.

Следующий этап: host-инструмент RexGlue для загрузки и анализа этого XEX, затем отчёт о неразрешённых функциях/импортах. До него не делать выводов о количестве переносимых функций, запуске или сроках порта.
