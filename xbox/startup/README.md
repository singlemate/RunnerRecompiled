Текущий этап: [диагностика тёмных артефактов](../gpu-port/TEXTURE-ARTIFACTS.md).641regression checks на Y700. Сопоставлены native BC и SDK compute decode: тёмный фон сохраняется, подмена формата не является исправлением. Следующий шаг — выделить draw/shader фона и проверить bindings/blend/depth. Live Android Surface ещё не подключена.

Предыдущий этап: [первый визуально проверенный кадр](../gpu-port/FIRST-GUEST-FRAME.md).641self-tests на Y700. Vulkan offscreen presenter создал14кадров1280×720 за45с. Подтверждён стартовый экран Mirror's Edge с Press START button. Снимок открыт через ACTION_VIEW на планшете; live Android Surface ещё не подключена. Следующий шаг — Activity/Surface/lifecycle, затем настоящий game input bridge.

Предыдущий этап: [vblank и GPU semaphore](../gpu-port/VBLANK-SEMAPHORE.md).641self-tests на Y700. Источник ожидания найден: guest queue требовала source0/vblank. Монотонный60Гцclock вызывает исходный handler, который сам сбрасывает semaphore. Подтверждено продвижение GPU ring1636/1636; Android presenter ещё не подключён. 125сигровой прогон завершён внешним timeout124 без нового unsupported/fault, с созданием многих Vulkan pipelines и повторными PRESENT requests. Следующий основной этап — подключить Android Surface/presenter.

Предыдущий этап: [VdPersistDisplay и GPU semaphore](../gpu-port/DISPLAY-PERSISTENCE.md).630self-tests на Y700. Игра прошла явный headless отказ VdPersistDisplay; настоящая display persistence не реализована. WPTR61/RPTR52: backend ждёт0 в1FBF3004, наблюдается1; CPU ждёт продвижения writeback serial11. Следующая работа — владелец semaphore, прерывания/callbacks и возможный vblank. Android presenter ещё не подключён.

Предыдущий этап: [публикация GPU-команд](../gpu-port/GPU-SUBMISSION.md). 626 self-tests на Y700. Исправлен HSIO compatibility ответ, отключавший submissions в гостевом драйвере. Игра прошла GPU clock export, заполнение memory и тождественный720p scaler; ring43/43 до stop. Следующий import — VdPersistDisplay. Android presenter, XMA decoder, сохранения и game input bridge ещё не готовы.

Предыдущий этап: [Attract Movie и multiple wait](../gpu-port/ATTRACT-MULTIWAIT.md). 614 self-tests на Y700. Игра прошла XNADDR, третий guarded frsqrte, XMA create/Clear и чтение нужного диапазона Textures.tfc. Подготовлены файлы Attract Movie; расширено NT handle wait до64events/threads/notifications/mutants. Игровой прогон подтвердил успешные ожидания двух mutants до call32; завершился по лимиту240с (exit44). Проверка dcbst CPU VM ускорена с сохранением commit validation; [измерения и тесты](../gpu-port/CACHE-LINE-VALIDATION.md). Игра прочитала Attract_Movie.bik целиком161892072байта (coverage без дыр); за240с сгенерировано8894VdSwap packets, exit44. Следующий шаг — проследить их публикацию в GPU backend и system identifier. Декодер XMA, постоянные сохранения и Android→game input bridge ещё не готовы. Ниже сохранена хронология этапов.

Текущий этап: [профиль и game-global null](../gpu-port/PROFILE-FAILURE-GLOBAL.md). 443 проверки; новая граница — global830DD060=0 в829A0400. Причина инициализации пока не установлена.

Текущий этап: [длинная загрузка и уведомления](../gpu-port/LONG-BOOT-NOTIFICATIONS.md). 436 проверок. TdGame и Startup_int прочитаны; новая граница XamUserCreateAchievementEnumerator. Длинный режим отделён от обычного.

Текущий этап: [XMP, input, cache и wait](../gpu-port/XMP-INPUT-CACHE-WAIT.md). 412 проверок. Ts и локализация прочитаны; Tp подготовлен. Ранние timeout остаются.

Текущий этап: [XMP и FIFO](../gpu-port/XMP-FIFO.md). 399 проверок; XMP query пройден, следующая граница XMsgStartIORequestEx FA/7001A. Ранние timeout ещё остаются.

Текущий этап: [mutant и resume](../gpu-port/MUTANTS-RESUME.md). 392 проверки. Новая явная граница XMsgInProcessCall; повторные запуски ещё недетерминированы.

Текущий этап: [очередь и frsqrte](../gpu-port/FRSQRTE-STARTUP.md). 372 проверки; два достигнутых frsqrte пройдены диагностически, новая граница NtCreateMutant. ADB probe работает без экрана Activity.

Текущий этап: [потоки, пакеты и время](../gpu-port/THREADS-TIMESTAMP.md). 362 проверки. Подтверждено чтение StartupMovie.bik; повторные запуски ещё недетерминированы.

Текущий этап: [диагностика переключения потоков](../gpu-port/POLL-SCHEDULING.md). 358 проверок; Core.xxx прочитан. Эксперимент poll-yield отдельно от штатного explore.

Текущий этап: [offline startup, локализация и события](../gpu-port/OFFLINE-SPLASH-EVENTS.md). 352 проверки; следующая задача — диагностика ожидания после запуска потоков.

Текущий этап: [XEX resources и реальные файлы](../gpu-port/STARTUP-RESOURCES-FILES.md). TOC и Coalesced.ini прочитаны; новая остановка NetDll_XNetStartup. 333 проверки, GPU31/31.

Текущий этап: [отсутствующий debug volume](../gpu-port/DEBUG-VOLUME.md). ShaderDumpxe query пройден, новая остановка KeEnableFpuExceptions; 306 проверок, GPU31/31. Полная файловая система ещё не реализована.

Текущий этап: [PCM submit](../gpu-port/PCM-SUBMIT.md). AAudio принял стартовые игровые кадры, callback вернулся. Следующая граница NtQueryFullAttributesFile; 303 проверки, GPU31/31.

Текущий этап: [multiple-wait и выход на PCM submit](../gpu-port/MULTIPLE-WAIT.md). Новые границы XAudioSubmitRenderDriverFrame и NtQueryFullAttributesFile. 294 проверки; GPU31/31.

Текущий этап: [AAudio endpoint и XAudio callback](../gpu-port/AAUDIO-CLIENT.md). STARTED и вход callback подтверждены; новые зависимости wait-multiple/ANSI/volume-mask. 282 проверки; PCM ещё не подключён.

Текущий этап: [аудиопоток и IRQL](../gpu-port/AUDIO-THREAD.md). Suspended/resume прошли, следующая остановка XAudioRegisterRenderDriverClient. 278 проверок; GPU31/31.

Текущий этап: [kernel semaphores](../gpu-port/SEMAPHORES.md). Игра создала аудиосемафор, следующая остановка — флаги ExCreateThread. 269 проверок прошли; GPU31/31.

Текущий этап: [XMA context memory](../gpu-port/XMA-CONTEXTS.md). Игра получила настоящий адрес XMA-контекстов; GPU read/write31/31. Следующая остановка KeInitializeSemaphore. 259 проверок прошли.

Текущий этап: [raised-IRQL spinlock](../gpu-port/RAISED-SPINLOCK.md). GPU callback освободил оба lock и дошёл до IssueSwap; следующая остановка XMA MMIO 7FEA1800. 252 проверки прошли.

Текущий этап: [Vulkan pipeline и spinlock](../gpu-port/PIPELINE-SPINLOCK.md). Драйвер создал игровой pipeline; GPU callback прошёл обычный spinlock, следующая остановка KeAcquireSpinLockAtRaisedIrql. 246 проверок прошли.

Текущий этап: [генерация VdSwap и исполнение PM4](../gpu-port/SWAP-PM4.md). Read/write достигли 28/28, следующий блок 31/28 начал создание игрового graphics pipeline и не завершился до таймера. 242 проверки прошли.

Текущий этап: [системные GPU-метаданные и вход VdSwap](../gpu-port/SYSTEM-BUFFER.md). Параметры VdSwap получены на планшете; генерация swap-пакетов ещё не реализована. 242 проверки прошли.

Текущий этап: [защита физической памяти](../gpu-port/PHYSICAL-PROTECT.md). Четыре игровых вызова пройдены; следующая остановка VdGetSystemCommandBuffer. 242 проверки прошли, изображения пока нет.

Текущий этап: [EDRAM/HSIO](../gpu-port/EDRAM-HSIO.md). Игра продолжила запуск при HSIO=false; следующая остановка MmSetAddressProtect. GPU read/write=25/25, 242 проверки; изображения пока нет.

Последний этап: [kernel waits и critical regions](../gpu-port/KERNEL-WAITS.md), 242 проверки. Worker events работают; новая остановка VdRetrainEDRAMWorker, изображения нет.

Последний этап: [первые игровые PM4](../gpu-port/FIRST-GAME-PM4.md) — в --gpu-explore read/write=25/25, 228 self-tests. Draw/изображение не подтверждены; default identifier stop сохранён.

Последний этап: [видеонастройка и настоящий игровой ring](../gpu-port/XCONFIG-RING.md), 220 проверок. Ring 32 KiB создан; новая остановка — non-null system GPU identifier. Изображения нет.

Последний этап: [GPU в реальном startup игры](../gpu-port/GAME-GPU.md), SetupContext SUCCESS, 214 self-tests; новая остановка ExGetXConfigSetting. Игрового ring/изображения ещё нет.

Последний этап: [настоящий Vulkan CP на Y700](../gpu-port/EXTERNAL-CP.md): setup + PM4 Type0 + shutdown прошли, full link 0 unresolved. К игре GPU пока не подключён.

Последний этап: [блокировка гостевого исполнения в CP](../gpu-port/GUEST-GATE.md), 209 проверок на Y700. Настоящий GPU CP ещё не запущен.

Последний этап: [готовность GPU-worker](../gpu-port/READINESS.md), 200 проверок на Y700. Полный GPU ещё не запущен.

Последний этап: [внешний host-worker CommandProcessor](../gpu-port/HOST-WORKER.md), 192 проверки на Y700. Полный GPU ещё не запущен.

Последний этап: [software physical invalidation](../gpu-port/WRITE-WATCH.md), 184 проверки на Y700. CP/GPU пока не подключён.

Последний этап: [поток доставки GPU-прерываний](../gpu-port/INTERRUPTS.md), 169 проверок на Y700. Полный GPU ещё не подключён.

Последний этап: [адаптер памяти GPU](../gpu-port/MEMORY-INTERFACE.md), 148 проверок на Y700, 12 внешних символов при полной линковке. GPU ещё не подключён.

Последний этап: [MMIO из перекомпилированного кода](../gpu-port/MMIO.md), 300/300 TU и 136 проверок на Y700. GPU пока не подключён.

Последний этап: [общая физическая память CPU/GPU и RingBuffer RexGlue](../gpu-port/SHARED-MEMORY.md).

Новый этап: [компоненты Xenos/Vulkan под Android](../gpu-port/README.md). Переводчик SPIR-V и создание VulkanDevice RexGlue работают на Y700; полный GPU пока не подключён к игре.

Текущий этап: [видеорежим, физическая память и GPU](VIDEO-PHYSICAL.md), [результат](video-physical-result.json). Остановка — VdInitializeEngines: требуется Xenos/Vulkan backend.

Последний этап: [рабочие потоки на ARM64](THREADS.md), [результат](threads-result.json). Два игровых рабочих потока запущены и ожидают события; текущая остановка — XGetVideoMode.

Текущий этап: [кэш, события и запрос рабочего потока](CACHE-EVENTS.md), [результат](cache-events-result.json). Остановка — ExCreateThread: нужен многопоточный runtime.

Последний этап: [начальный поток, TLS, блокировки и таймер](THREAD-TLS-RUNTIME.md). Текущая остановка — FscSetCacheElementCount; [результат](runtime-result.json). Ниже сохранён отчёт первого этапа; его раздел «следующий шаг» является историческим.

# Первый native-вход в код игры на Y700 — 21.10.2026

На устройстве TB322FC (ADB test device) выполнен рекомпилированный исходный entry point 0x829FAB98 (_xstart). Выполнение предсказуемо остановлено на отсутствующей переменной ядра Xbox XexExecutableModuleHandle. Это первый связный запуск стартового кода на ARM64, не запуск меню или геймплея.

## Фактический вывод

```json
{"event":"entry_begin","entry":"829fab98","mappings":76809,"sections":10,"page_size":4096}
{"event":"unimplemented_variable_import","name":"XexExecutableModuleHandle","iat":"8200062c","ordinal":403,"lr":"82a01580","sp":"700fed10"}
```

Exit 42 означает намеренную диагностическую остановку, не успешный запуск игры. LR соответствует вызову sub_82A01498 из sub_82A01570. Статический путь от _xstart включает __savegprlr_28 и начальные CRT-функции; полной динамической трассировки каждой инструкции здесь нет.

Сводка с SHA256 исполняемого файла, данных и исходного XEX: device-entry-result.json. Полный журнал: device-entry.log. Исходный XEX сохранил SHA256 3ed882fd1900ff2bbccf1de745fb7e92c7d2168bb06069cbab311ac8eff28e1b.

## Как устроен запуск

Небольшой диагностический loader entry_probe.cpp связан с настоящим libmirrorsedge_ppc_fourth.a. Исходный PPC не исполняется эмулятором: процессор планшета выполняет AOT ARM64-код. Прочитаны 10 секций локального образа пользователя, настроена таблица косвенных вызовов XenonRecomp и отдельный гостевой стек. 4 ГиБ виртуального диапазона зарезервированы с PROT_NONE; доступны только нужные страницы. Это не выделение 4 ГиБ физической памяти.

275 импортируемых функций завершают процесс с именованной диагностикой. Для 12 импортируемых переменных установлены указатели на недоступные страницы-сторожа. Ни одна из этих зависимостей не возвращает выдуманный результат успеха. На первом обращении к данным XexExecutableModuleHandle срабатывает сторож и выводит имя переменной, ordinal, IAT и состояние LR/SP.

Изначальный контекст нулевой, кроме выделенного стека и архитектурных значений заголовка; настоящий Xbox thread/TEB/TLS ещё не создан. На данном пути до диагностической остановки этого оказалось достаточно. Нельзя использовать такую инициализацию как подтверждённую среду исполнения всей игры.

Сборка: NDK r30, Android API 35, C++17, статическая libc++. В инструмент входят текущая ARM64-библиотека игры и локально сгенерированные диагностические окончания импортов. Штатный APK не изменялся. На устройстве остались /data/local/tmp/me_native_entry_probe и /data/local/tmp/me_native_entry_sections.bin. Процесс завершился; фоновых игровых потоков не осталось.

Исполняемый файл (117 645 104 байта) и bundle секций (17 868 322 байта) хранятся только в work/ и на пользовательском планшете, не добавлены в проектный Git или APK. В outputs опубликованы собственный loader, сведения о проверке и скрипт подготовки tools/prepare_xbox_entry_local.py. Повторный скрипт намеренно отказывается перезаписывать существующий приватный bundle.

## Проверка RexGlue ABI

Реальные заголовки обеих систем скомпилированы для Windows x64; затем ключевые смещения подтверждены static_assert при компиляции под Android ARM64. Результаты: ../context-layout.json и abi-layout.cpp.

| Поле | Наш XenonRecomp | RexGlue c94f5ebd |
|---|---:|---:|
| sizeof(PPCContext) | 2688 | 2688 |
| r1 | 16 | 16 |
| f0 | 328 | 336 |
| f31 (измерено x64) | 576 | 584 |
| v0 | 592 | 592 |
| SAT/VSCR | uint32_t vscr на 2640 | uint8_t vscr_sat на 328 |

Одинаковый размер не означает совместимость: reinterpret_cast или memcpy всего контекста испортят FPR/служебные поля. Отличается и механизм косвенных вызовов: наш код использует memory lookup, RexGlue — свой resolver. RexGlue runtime в этот запуск не был связан; переходник ABI ещё не реализован. В заголовке RexGlue также остаётся предупреждение о ширине операнда FPCR; в нашем контексте оно исправлено ранее.

## Следующий необходимый шаг

Реализовать реальную модель загруженного модуля и экспорт XexExecutableModuleHandle, затем RtlImageXexHeaderField по настоящим заголовкам пользователя. Для RexGlue нужна явная передача полей контекста/диспетчеризация, либо повторная генерация его собственным генератором. Не подменять module handle нулём и не выдавать простое прохождение этой точки за готовый runtime.

На этом этапе не подтверждены heap, потоки, файловая система игры, исключения, GPU, звук, PhysX, PC-сохранения или совместимость оставшихся 4 инструкций. Две отсутствующие статические таблицы по-прежнему требуют контроля при фактическом достижении соответствующих путей.









