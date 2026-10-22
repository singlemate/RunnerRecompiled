# План Mirror’s Edge Native Android ARM64

Направление: Xbox 360 → Android ARM64. PC-версия не используется как основа порта.

Обновлено 2026-10-22. Галочка означает проверенный результат указанного объёма, а не готовность всего порта.

## Выполнено
- [x] Перекомпилировать Xbox 360-код в ARM64 (300 модулей).
- [x] Запустить код игры на Lenovo Y700 Gen4.
- [x] Подключить Vulkan backend к памяти игры.
- [x] Обработать первые GPU-команды.
- [x] Реализовать генерацию команд VdSwap в диагностическом режиме.
- [x] Подтвердить создание pipeline с игровым шейдером.
- [x] Добавить обычные spinlock.
- [x] Реализовать raised-IRQL spinlock: acquire/release без изменения уровня; проверено игровым callback.
- [x] Проверить восстановление TLS, IRQL, стека и контекста GPU-обработчика, включая исключение, в self-tests. Последний полный прогон на Y700: 641 проверка.

## Ближайшая работа
- [x] Обслужить чтение XMA ContextArrayAddress: настоящий массив320×64байта в физической памяти; декодер ещё не реализован.
- [x] Реализовать kernel semaphore initialize/release/wait: проверены токены, предел параметров, timeout и пробуждение; игра прошла инициализацию.
- [x] Поддержать фактические флаги аудиопотока: suspended start, KeResumeThread, guest CPU4; поток начал исполнение.
- [x] Поддержать гостевые raise/lower IRQL, проверено на планшете.
- [x] Диагностическая регистрация одного XAudio render client: реальный AAudio STARTED и вход игрового callback.
- [x] KeWaitForMultipleObjects (events/semaphores/threads), RtlInitAnsiString и запрос маски изменений громкости: игра прошла эти вызовы.
- [x] Диагностический XAudioSubmitRenderDriverFrame: planar6 BE → stereo, реальный AAudio write, частичные записи/ошибки, возврат callback.
- [ ] Непрерывное воспроизведение: consumption-driven refill, unregister/stop, audio focus; стартовые4credits пока ограничены.
- [x] Разобрать первый NtQueryFullAttributesFile: ShaderDumpxe не подключён, корректный NO_SUCH_FILE и перевод статуса; игра продолжила.
- [ ] Полные файловые атрибуты/mount/чтение игровых ресурсов для фактических игровых томов.
- [x] Отключение FPU traps, основной module handle, реальные XEX resource sections и ASCII subset Unicode conversion.
- [x] Read-only opening/query/read/close: игра полностью прочитала Xbox360TOC.txt и Coalesced.ini из своей копии.
- [x] Индекс исходного SVOD: отсутствующий файл отличаем от ещё не перенесённого; нет доступа к raw дискам хоста.
- [x] Диагностическая статистика tracked guest memory и legacy XAM version profile.
- [x] Диагностический offline startup XNet/WSA: игра обработала недоступность сети и продолжила; полный backend не реализован.
- [ ] Полный жизненный цикл XAudio client: возврат callback, PCM submission/consumption, пополнение очереди, unregister и остановка.
- [x] Подтвердить полное завершение GPU-блока: на планшете WPTR31/RPTR31, без подмены счётчиков.
- [ ] Проверить протокол system identifier и завершения команд.
- [ ] Проверить устойчивое восстановление всех блокировок GPU callback при конкуренции и отказах. Тесты состояния обработчика не доказывают отсутствие утечек произвольных гостевых locks.
- [ ] Заменить диагностические уступки времени устойчивым механизмом совместного исполнения CPU/GPU.

## Обязательные требования первой тестовой сборки
- [ ] Штатные сохранения Xbox-версии: стабильный профиль/пути, запись и фактическая загрузка прогресса.
- [ ] Проверить сохранность после перезапуска, обновления APK, очистки кэша и сбоя записи; backup и восстановление.
- [ ] Экспорт/импорт резервной копии отдельно от позднего импорта PC-save.
- [ ] Зафиксировать package id и ключ подписи, обновление без удаления приложения.
- [ ] Подключить выбор системного/пользовательского Vulkan-драйвера и безопасный fallback отдельным запуском.
- [ ] Подготовить тест на Samsung Galaxy Z Fold6 (Adreno750), включая feature/page-size probe, отчёт и поведение складного экрана.
- [ ] Подготовить APK и инструкцию внешнему тестеру; игровые ресурсы остаются пользовательскими.

Подробные критерии: [RELIABILITY-AND-DEVICES.md](RELIABILITY-AND-DEVICES.md). Всё выше — требования, не отметки готовности.

## Последующие этапы (состав может уточняться)
- [ ] Реализовать необходимые файловые и системные вызовы по фактическим обращениям.
- [ ] Подключить чтение игровых ресурсов из пользовательской копии.
- [ ] Подключить вывод Vulkan к поверхности Android-приложения.
- [ ] Пройти до меню и загрузить уровень.
- [ ] Подключить ввод к игре: контроллер, клавиатура/мышь, сенсорное управление с автоматическим скрытием.
- [ ] Реализовать необходимое декодирование XMA и вывод звука Xbox-версии.
- [ ] Проверить игровую физику и поведение уровня в перекомпилированной Xbox-версии.

- [ ] Добавить настройку пропуска вступительных роликов.
- [ ] Проверить стабильность и производительность на Y700 Gen4.
- [ ] Собрать первую тестовую Android-сборку с запуском игры и проверить её на планшете.

## В самый последний момент
- [ ] После первой тестовой сборки исследовать совместимость PC-сохранений с Xbox-версией. По результату решить, возможен ли импорт или нужна конвертация; совместимость заранее не предполагается.

## Проверено 2026-10-21: локализация и события
- [x] Прочитать Coalesced.int (424640 байт) и Splash.bmp (2764856 байт) из своей Xbox-копии.
- [x] Проверить большие файловые буферы по всем committed-страницам и VM address protection, включая decommit-дыру.
- [x] Именованные события: root FFFFFFFC, OPENIF, общее состояние, счётчик открытий и последнее закрытие; 8 дополнительных проверок.
- [x] Пройти 352 self-tests на подключённом Y700.
- [ ] Разобрать достижение alarm(20) после запуска дополнительных потоков; последний LR 8238E858 не определяет текущую инструкцию.

## Проверено: переключение при опросе
- [x] Собрать 300 модулей с -O1, сохранив старый -O0 архив.
- [x] Отделить timeout от fault и снять последовательные host PC/guest registers.
- [x] Подтвердить starvation при опросе: экспериментальная передача execution_gate разблокирует файловый worker.
- [x] Прочитать Core.xxx полностью и проверить установку файловой позиции; 358 self-tests.
- [ ] Восстановить полноценное планирование: экспериментальная точка переключения не заменяет scheduler.
- [ ] Разобрать новое ожидание по снимкам всех потоков; Engine.xxx подготовлен, чтение пока не подтверждено.
Подробности: [диагностика переключения потоков](xbox/gpu-port/POLL-SCHEDULING.md).

## Проверено: пакеты, ролик и время
- [x] Получить снимки нескольких гостевых потоков и найти второй цикл опроса.
- [x] В экспериментальном режиме передавать execution_gate при worker status poll, сохраняя исходный результат.
- [x] Подтвердить чтение Engine.xxx, GameFramework.xxx и полного StartupMovie.bik.
- [x] Подготовить read-driven KeTimeStampBundle и проверить продвижение миллисекунд; 362 self-tests.
- [ ] Подтвердить прохождение KeTimeStampBundle в игровом запуске: повторные прогоны пока останавливаются раньше по таймеру.
- [ ] Разобрать цикл worker 61091000 и завершение задания, ожидаемого 61081000; загрузка пока недетерминирована.
Подробности: [потоки и время](xbox/gpu-port/THREADS-TIMESTAMP.md).

## Уточнение ожидания worker
- [x] Записать интервалы KeDelayExecutionThread: повторяется Sleep(0), caller82372140; длительная пауза исключена для этого цикла.
- [x] Проверить и отклонить гипотезу исправления паузой100мкс: ожидание остаётся, эксперимент удалён.
- [ ] Проследить постановку/снятие заданий в игровом цикле823720xx; подтверждения потери задания пока нет.

## Проверено: очередь и frsqrte
- [x] Подтвердить продвижение producer/consumer очереди: наблюдавшийся Sleep(0) не доказывает потерю задания.
- [x] Найти SIGTRAP в sub82EBBDE8 и сопоставить с оставленной unsupported frsqrte.
- [x] Пройти 2 достигнутых frsqrte через guarded диагностический seed; 372 проверки. Bit-exact арифметика не подтверждена.
- [x] Unnamed NtCreateMutant, рекурсивный owner/wait/release, abandoned acquisition и close unowned. Named/multiple wait остаются открыты.
- [ ] Подключить вывод к Android Activity/Surface: текущие ADB-прогоны не показывают экран приложения.
Подробности: [очередь и frsqrte](xbox/gpu-port/FRSQRTE-STARTUP.md).

## Проверено: mutant и resume
- [x] Связать unnamed mutant с single wait/release/close и завершением owner.
- [x] Реализовать NtResumeThread по handle с предыдущим suspend count; 392 self-tests на устройстве.
- [x] Пройти предыдущую границу и получить следующий вызов XMsgInProcessCall.
- [x] XMsgInProcessCall FA/7001B: проверка контроллера системной музыки, подтверждено на игре.
- [ ] Устранить нестабильность загрузки: один из двух последних прогонов остановился раньше по таймеру.
Подробности: [mutant и resume](xbox/gpu-port/MUTANTS-RESUME.md).

## Проверено: XMP и порядок исполнения
- [x] Поддержать XMPGetPlaybackController для текущего профиля, 3 проверки параметров/outputs.
- [x] FIFO execution gate и 4 проверки порядка, все399 self-tests прошли на устройстве.
- [x] Пройти XMP query и подготовить запрошенный IpDrv.xxx из своей копии.
- [x] XMsgStartIORequestEx FA/7001A: текущий setter, overlapped0, пустое множество listeners; игра прошла вызов. Полный notification API не готов.
- [ ] Ранние timeout остаются: FIFO не является полным исправлением загрузки.
Подробности: [XMP и FIFO](xbox/gpu-port/XMP-FIFO.md).

## Проверено: ввод, cache operation и новые пакеты
- [x] Корректный input-provider-absent путь: игра обработала опрос users0..3; Android input bridge ещё не подключён.
- [x] Пройти достигнутый dcbst82EC0968 только для coherent CPU VM с проверкой диапазона и барьером.
- [x] Handle multiple wait с count1, acquire/timeout/auto-reset; итог412 self-tests на планшете.
- [x] Прочитать IpDrv.xxx, Ts_LOC_int.xxx и Ts.xxx.
- [x] Подготовить запрошенный Tp.xxx из своей копии.
- [x] Подтвердить чтение Tp.xxx; затем полностью прочитать TdGame.xxx и Startup_int.xxx в длинном прогоне.
- [ ] Полные notifications/overlapped, count>1 handle waits и cache operations вне обычной VM.
Подробности: [XMP, input, cache и wait](xbox/gpu-port/XMP-INPUT-CACHE-WAIT.md).

## Проверено: длинная загрузка и системные уведомления
- [x] Отделить чтение больших пакетов от слишком короткого20с лимита; отдельный длинный probe120с.
- [x] Прочитать Tp.xxx, весь TdGame.xxx (19070976 байт) и Startup_int.xxx.
- [x] Реализовать listeners, маску/версию/FIFO/match, manual readiness и close; связать XMP broadcast с реальной очередью.
- [x] Проверить цепочку XMP→notification→wait→read→timeout и отсутствие profile/XUID;436 self-tests.
- [x] Диагностический absent-profile отказ achievement enumerator; игра обработала1317. Полный сервис не реализован.
- [ ] Постоянный локальный профиль и полноценные Xbox-сохранения остаются незавершёнными.
Подробности: [длинная загрузка и уведомления](xbox/gpu-port/LONG-BOOT-NOTIFICATIONS.md).

## Проверено: профиль и новый null pointer
- [x] Проверить отказ достижениям при отсутствующем профиле без фиктивного handle и изменения outputs.
- [x] Обработать отсутствие input capabilities и NULL buffer;443 self-tests.
- [x] Сопоставить worker object с thread handle20004 и уточнить, что field32 связан с CPU2, а не доказанным busy state.
- [x] Дойти до нового SIGSEGV: getter8223CF38 вернул global830DD060=0, разыменование в829A0400.
- [x] Проследить создание/очистку global830DD060: отказ XNetStartup вызывал очистку. В повторном игровом запуске локальная инициализация позволила сохранить объект; create_end=0, pointer11D73480.
- [ ] Раннее ожидание рабочего пула и постоянный профиль/сохранения ещё не исправлены.
Подробности: [профиль и game-global null](xbox/gpu-port/PROFILE-FAILURE-GLOBAL.md).

## Проверено 2026-10-22: локальный XNet и следующая граница
- [x] Проверить локальную службу XNet на реальном UDP-сокете, привязанном к loopback; 448 self-tests. Внешние соединения и Xbox Live не реализованы.
- [x] Пройти прежнее создание game-global без очистки и null pointer; лог work/startup/network-confirm-long.log, завершение probe_exit=42 на следующем неподдержанном импорте.
- [x] Извлечь запрошенный GuidCache.xxx (98304 байта) из своей Xbox-копии.
- [x] Реализовать NetDll_WSAWaitForMultipleEvents для событий: WaitAny/All, миллисекунды/INFINITE, ошибки handles и освобождение gate. На Y700 прошли462 self-tests, включая межпоточное пробуждение бесконечного ожидания.
- [x] Подтвердить вход WSAWaitForMultipleEvents в игровом запуске: count1, WaitAny, INFINITE, реальный event10024. Ожидающий поток освобождает gate, загрузка продолжилась; завершение этого ожидания пока не подтверждено.
- [x] Подтвердить полное чтение GuidCache.xxx (98304 байта), лог work/startup/pool-transition-long.log.
- [x] Подготовить следующий запрошенный LoadingSimple.bik (179428 байт) из своей Xbox-копии и передать на планшет.
- [x] Подтвердить полное чтение LoadingSimple.bik (179428 байт); воспроизведение ролика не подтверждено.
- [x] Локализовать раннее ожидание worker pool и пройти проблемный порядок массива в отдельном compatibility experiment; см. следующий раздел. Полноценное планирование ещё не готово.

## Проверено 2026-10-22: обход workers и пакет меню
- [x] Сопоставить исходные PowerPC-инструкции и получить оба порядка возврата workers; зависание связано с повторным сбросом index=count-1.
- [x] Подготовить отдельный compatibility experiment --gpu-explore-pool-scan; оригинальные файлы игры и стандартный режим сохранены.
- [x] Пройти470 self-tests, включая вызовы настоящей гостевой8266A880 для обоих порядков массива, reserved-only, flag1 и пустого пула.
- [x] Подтвердить игровое срабатывание исправленного обхода при проблемном порядке: index1→0, normal worker выбран, загрузка продолжилась.
- [x] Извлечь и передать TdMainMenu.xxx40763392байт; игра открыла пакет и прочитала первые131072байта.
- [x] Подтвердить полное чтение TdMainMenu.xxx: последний offset40632320 +131072 =40763392, лог work/startup/tdmainmenu-pool-scan.log. Следующая граница — XamUserReadProfileSettings, caller829C29B4.
- [x] Реализовать диагностический absent-user отказ XamUserReadProfileSettings без фиктивных настроек/изменения выходных буферов;478 self-tests на планшете. Постоянный профиль не реализован.
- [x] Подтвердить прохождение чтения настроек профиля: игра обработала1317 для3settings и дошла до XamUserGetName; work/startup/profile-read-game.log.
- [x] Диагностическая обработка XamUserGetName для отсутствующего профиля, проверка HRESULT/аргументов/неизменности output;484 self-tests.
- [x] Подтвердить прохождение XamUserGetName: игра обработала80070525 при capacity32 и дошла до KeQuerySystemTime; work/startup/user-name-game.log.
- [x] Реализовать KeQuerySystemTime: CLOCK_REALTIME →1601 epoch,100нс, BE64; NULL no-op, void ABI. Проверены UTC-интервал/соседние bytes/регистры; всего488 self-tests на Y700.
- [x] Подтвердить игровое прохождение KeQuerySystemTime: получено реальное UTC FILETIME, игра продолжила; work/startup/system-time-game.log.
- [x] Подготовить согласованный начальный часовой пояс гостевой консоли: UTC, имена UTC, нулевые bias/transition dates для settings1..7; системные UTC-часы не смещаются.506 self-tests на Y700.
- [x] Подтвердить игровое прохождение settings1..7 с корректным required_size4; work/startup/timezone-game.log. Следующий запрос того же caller — setting12 RETAIL_FLAGS.
- [x] Поддержать достигнутый RETAIL_FLAGS как явную начальную настройку runtime0x40 по pinned SDK;509 self-tests на планшете.
- [x] Подтвердить игровое прохождение setting12; work/startup/retail-timezone-game.log. Следующий вызов RtlTimeToTimeFields, caller829FB368.
- [x] Реализовать RtlTimeToTimeFields и RtlTimeFieldsToTime с BE16/BE64, календарной проверкой и сохранностью output при ошибке;528 self-tests на Y700.
- [x] Подтвердить игровое прохождение RtlTimeToTimeFields: корректная UTC-дата2026-10-22/Thursday; следующий data import XboxKrnlVersion, ordinal344, IAT820007E4.
- [x] Получить target2.0.7645.0/minimum2.0.6717.0 из настоящих XEX imports и привязать XboxKrnlVersion к явному target-based профилю совместимости;535 self-tests на Y700.
- [x] Подтвердить игровое прохождение XboxKrnlVersion: следующая граница XamInputSetState, caller829F9728; work/startup/kernel-version-game.log.
- [x] Согласованная absent-provider обработка XamInputSetState:1167 без фиктивной вибрации, NULL160, входной пакет неизменён;540 self-tests на устройстве.
- [x] Подтвердить игровое прохождение запроса вибрации: user0→1167 обработан, игра продолжила; work/startup/vibration-game.log. Реальный Android→game input/haptics bridge ещё не подключён.
- [x] NetDll_XNetGetEthernetLinkStatus(caller1): flags0 для локального offline backend, без внешнего link/speed/duplex/Xbox Live.542 self-tests на планшете.
- [x] Подтвердить игровое прохождение offline link status; work/startup/ethernet-game.log. Следующая граница InterlockedPopEntrySList, caller82B8CB3C, thread610E1000.
- [x] Реализовать InterlockedPopEntrySList/InterlockedFlushSList с64-битными seq_cst atomic операциями и BE header;552 self-tests, включая4host threads/64nodes без потерь и дублей.
- [x] Подтвердить игровое прохождение SList; work/startup/slist-game.log. Следующая остановка — собственный ExCreateThread_probe_capacity_exceeded при16thread records.
- [x] Расширить общий предел records до64 для обычных/GPU/audio потоков; static bounds и крайний слот63 проверены.556 self-tests на Y700.
- [x] Подтвердить создание игровых потоков17и18 со стеками32768байт, PCR61100000/61110000 и раздельными TLS; work/startup/thread-capacity-game.log.
- [x] Реализовать NetDll_XNetGetTitleXnAddr для offline backend: XNET_GET_XNADDR_NONE1, пустые36байт, без выдуманного MAC/online identity.560 self-tests на Y700.
- [x] Подтвердить игровое прохождение XNADDR:NONE, игра продолжила до запроса TdMainMenu_Images.xxx; work/startup/title-address-game.log.
- [x] Извлечь запрошенный TdMainMenu_Images.xxx229376байт из своей Xbox-копии и передать на планшет.
- [x] Подтвердить полное чтение TdMainMenu_Images.xxx229376байт; work/startup/menu-images-game.log. Следующий SIGTRAP сопоставлен с unsupported frsqrte82384344.
- [x] Подготовить21пакет TdMainMenu_Audio0..20.xxx, всего39518208байт, из своей Xbox-копии и передать на планшет. Чтение/воспроизведение ещё не подтверждены.
- [x] Подключить третий достигнутый frsqrte82384344 к существующему finite-positive guarded helper; проверить исходный opcode и пересобрать приватную TU36.560 regression self-tests прошли.
- [x] Подтвердить игровое прохождение третьего frsqrte82384344 (наблюдавшийся input3388892.75) и чтение всего Audio0.xxx2523136байт; work/startup/menu-frsqrte-game.log. Xbox bit-exact estimate ещё не реализован.
- [x] XMACreateContext/XMAReleaseContext:320реальных64-байтовых слотов в том же физическом массиве, OOM/release/reuse/zeroing, согласованность MMIO.570 self-tests на Y700; декодера ещё нет.
- [x] Подтвердить игровое создание XMA-контекста BF4AB000 в том же массиве MMIO1F4AB000; work/startup/xma-context-game.log. Следующий write — Context0Clear7FEA1A80.
- [x] Поддержать Clear bank0..9 для принадлежащих пулу idle contexts: очистка valid/read/write offsets с сохранением остальных полей.575 self-tests, включая слоты0и319.
- [x] Подтвердить игровое прохождение XMA Clear для context0; work/startup/xma-clear-game.log. Декодер/enable/completion ещё не реализованы.
- [x] Извлечь следующий запрошенный Textures.tfc520159232байт из своей Xbox-копии; перед передачей проверено свободное место устройства.
- [x] Завершить передачу Textures.tfc и сверить SHA256 на устройстве с исходным извлечённым файлом.
- [x] Подтвердить чтение Textures.tfc по реальному offset496173056/read131072 и продолжение до Attract_Movie.txt; work/startup/textures-game.log. Полное чтение520МБ не заявляется.
- [x] Подготовить Attract_Movie.txt31байт и связанный Attract_Movie.bik161892072байт из своей Xbox-копии; передать на планшет.
- [x] Подтвердить открытие Attract_Movie.txt/.bik; work/startup/attract-game.log. Следующий блокер — NtWaitForMultipleObjectsEx count2.
- [x] Поддержать1..64handles для events/threads/notifications, WaitAny/All, relative/zero/infinite timeout;591 self-tests на Y700. Первый игровой прогон выявил mutant в наборе; files/APC остаются неподдержаны.
- [x] Подтвердить прохождение ожидания двух mutants в игре: WaitAny возвращает0, повторения до call32; work/startup/mutant-multiwait-game.log. Прогон завершён по лимиту240с, exit44; это не подтверждение завершения загрузки.
- [ ] Повторные прогоны и устойчивое планирование остаются открыты; compatibility patch не заменяет scheduler Xbox.
Подробности: [обход пула и пакет меню](xbox/gpu-port/POOL-SCAN-MENU.md).

- [x] Расширить NT WaitAny/WaitAll на mutants: рекурсия, abandoned, проверка переполнения до захвата набора и настоящее пробуждение другим потоком; 603 self-tests на Y700, mutant-multiwait-self-test.log, exit0.

- [ ] Разобрать длительную работу потока61091000 после Attract Movie: снимки показывают меняющиеся участки82EB/82EC и частые проверки памяти в word_writable; отличить стоимость CPU-декодирования/валидации от отсутствующего завершения.

- [x] Убрать32повторных поиска памяти на каждую dcbst-line: актуальная VM/page commit проверка, без кеширования владельца;614 self-tests на Y700. Локальный benchmark описан в xbox/gpu-port/CACHE-LINE-VALIDATION.md.
- [x] Подтвердить полный read coverage Attract_Movie.bik161892072байта без дыр:1242reads, work/startup/cache-line-game.log.coverage.json. Прогон240с/exit44;8894генерации VdSwap, воспроизведение не подтверждено.

- [ ] Проследить публикацию GPU command buffers после VdSwap и реальное завершение/system identifier; генерация пакетов отдельно от их исполнения. Подробности: [CACHE-LINE-VALIDATION.md](xbox/gpu-port/CACHE-LINE-VALIDATION.md).

- [x] Проследить потерю GPU submissions: HSIO=false выставлял guest disable bit; команда не доходила до CP_RB_WPTR. Контрольный журнал gpu-submission-game.log.
- [x] Исправить контракт virtual GPU readiness после успешного backend setup; аппаратная калибровка Xbox не заявляется.
- [x] Подключить гостевой VdGpuClockInMHz=500 и RtlFillMemoryUlong с BE32 pattern/границами/void ABI.
- [x] Пройти тождественный720p scaler200words: наблюдавшийся профиль, valid PM4 NOP, остальные профили unsupported.626checks на Y700.
- [x] Подтвердить дальнейшую публикацию и исполнение ring: WPTR43/RPTR43 до stop, scaler-packed-game.log. Backend вызвал PRESENT; Android presenter отсутствует.
- [ ] Разобрать VdPersistDisplay, фактические аргументы и владение display memory; затем продолжить GPU path. Текущий stop42, LR82815458.
Подробности и уточнение прежней интерпретации HSIO: [GPU-SUBMISSION.md](xbox/gpu-port/GPU-SUBMISSION.md).
- [x] Разобрать текущий VdPersistDisplay callsite: нулевой12Bdescriptor, BOOL-ветка отказа пропускает освобождение output.
- [x] Реализовать явный отказ для headless probe без allocation/изменения output; игра прошла вызов. Настоящее сохранение дисплея остаётся открытым.
- [x] Пройти630regression checks на Y700; persist-display-self-test.log и gpu-poll-memory-self-test.log.
- [x] Подтвердить новые GPU publications до WPTR61; RPTR52 при текущем ожидании. persist-display-game.log, limit240с/exit44.
- [x] Снять значения CPU/GPU памяти: GPU ждёт0 в1FBF3004, фактически1; CPU writeback serial11. Повторный35сснимок завершён внешним timeout124.
- [ ] Проследить владельца/сброс GPU semaphore1FBF3004, PM4interrupt и guest callbacks; определить необходимость vblank. Без подмены значения или completion.
- [ ] Реализовать настоящее сохранение состояния дисплея при подключённом presenter, если понадобится игровому/системному сценарию.
Подробности: [DISPLAY-PERSISTENCE.md](xbox/gpu-port/DISPLAY-PERSISTENCE.md).
- [x] Подтвердить доставку PM4 IRQ source1/cpu2: второй callback откладывает semaphore reset до target vblank; потери IRQ не выявлено.
- [x] Добавить виртуальную монотонную60Гцразвёртку и source0 через SDK/IRQ worker, без записи semaphore из host.
- [x] Пройти641self-test на Y700: cadence boundaries/skipped periods/worker lifecycle и регрессии.
- [x] Подтвердить semaphore1→0 внутри оригинального source0 handler и GPU ring1636/1636;35сrun/внешний timeout124.
- [x] Завершить125сигровой прогон после vblank: внешний timeout124, без нового unsupported/fault; идут чтение ресурсов, создание pipelines и повторные PRESENT requests. Следующий основной этап — Android presenter.
- [ ] Подключить pause/resume/shutdown и Surface vsync к нормальному Android lifecycle; текущий clock только diagnostic process-lifetime.
Подробности: [VBLANK-SEMAPHORE.md](xbox/gpu-port/VBLANK-SEMAPHORE.md).
- [x] Создать настоящий Vulkan offscreen presenter и включить GPU guest output refresh.
- [x] Захватить14кадров1280×720 в45секундном прогоне; offscreen-game.log, внешний timeout124.
- [x] Визуально подтвердить стартовый экран Mirror's Edge/Press START button в GPU readback, work/startup/offscreen-frame.png.
- [x] Сохранить снимок в Download планшета и отправить ACTION_VIEW; это неподвижный кадр, не live игра.
- [ ] Подключить live Android Surface/swapchain и запуск runtime из Activity с корректным lifecycle.
- [ ] Пройти Press START через настоящий game input bridge, открыть меню и загрузить уровень.
Подробности: [FIRST-GUEST-FRAME.md](xbox/gpu-port/FIRST-GUEST-FRAME.md).
- [x] Повторно подтвердить тёмные силуэты дальних зданий на позднем кадре, а не только первом захвате.
- [x] Снять54уникальных texture upload configurations; явных unknown load shader failures не найдено.
- [x] Сравнить native BC и существующий SDK compute decode в RGBA: артефакт сохраняется; эксперимент не принят как исправление.641regression checks на Y700.
- [ ] Выделить draw/shader дальнего фона и проверить texture bindings, alpha/blend и depth state. Причина тёмных артефактов пока не установлена.
Подробности: [TEXTURE-ARTIFACTS.md](xbox/gpu-port/TEXTURE-ARTIFACTS.md).
- [x] Снять46уникальных draw/shader состояний с blend/depth/RT/mask и первыми fetch bindings; draw-trace-game.log.
- [x] Проверить исключение PS5EBE814012217F48: убирает небо целиком, не является исправлением пятен.
- [x] Проверить исключение depth-only VS3DABEAE3153908FC: тёмные силуэты сохраняются, обход отклонён.
- [x] Получить дизассемблирование фонового VS/PS и256констант; старшие константы не нулевые, выбранный PS не читает текстуры.
- [ ] Установить проход, непосредственно формирующий проблемные пиксели, и проверить его vertex fetch/арифметику либо сопоставить исходный рендер. Причина пока не установлена.
Подробности: [DRAW-ARTIFACT-ISOLATION.md](xbox/gpu-port/DRAW-ARTIFACT-ISOLATION.md).

## 22 октября: сравнение Android-портов

- [x] Проверены выбранные исходники MW2, Liberty, Skate 3 и Unleashed; закреплены commit и URL.
- [x] Сопоставлены BC workaround, vertex endian/normalization, NaN clamp и depth handoff с нашим backend.
- [ ] Найти и подтвердить причину чёрных силуэтов: следующий приоритет — vertex colors / fetch.

Подробности: [ANDROID-PORT-RESEARCH.md](xbox/gpu-port/ANDROID-PORT-RESEARCH.md). Готового исправления артефактов пока нет.


## 22 октября: данные vertex colors

- [x] Добавлен ограниченный opt-in дамп фактических vertex bindings с проверкой отображения памяти.
- [x] На планшете получены слоты 95/94/93; CPU-декодирование выборки даёт конечные позиции и ненулевые цвета.
- [x] Финальная сборка: 641 self-test, exit0; диагностический игровой запуск завершён timeout35 без нового fault.
- [ ] Сопоставить CPU-декодирование с реальным GPU-выходом; исправить причину силуэтов.

[Подробности](xbox/gpu-port/VERTEX-COLOR-TRACE.md).


## 22 октября: живой диагностический просмотр

- [x] Передача реальных RGBA кадров в приложение по loopback, только при ME_GUEST_PREVIEW=1.
- [x] На планшете визуально подтверждён кадр №35, 1280×720.
- [x] APK обновлён через install -r прежней подписью; runtime641 self-tests.
- [ ] Полноценный runtime внутри приложения и прямой Surface/swapchain.
- [ ] Подключить игровой ввод, пройти START и проверить меню.

[Подробности](xbox/gpu-port/LIVE-PREVIEW.md).


## 22 октября: частота просмотра

- [x] Показ закрыт по просьбе пользователя; измерения выполнены без Activity.
- [x] Убрана пауза500мс между live captures: теперь100мс; PPM остаётся раз3с.
- [x] В коротком тесте через ADB получено7.33 кадра/с (2 уникальных изображения из12), это не FPS игры.
- [x] Гипотеза альфа-канала не подтверждена:255 в проверенных кадрах.
- [ ] Исправление графических артефактов, Surface и реальная производительность игры остаются открыты.


## 22 октября: изоляция фонового PS

- [x] Выполнены отдельные фоновые запуски с прямым входящим цветом и постоянным цветом одного PS.
- [x] При постоянном фиолетовом небе силуэты сохраняются серыми: поиск расширен на отдельные draw/pass и последующую обработку.
- [x] Зафиксирован RT0 HDR7e3 (2_10_10_10_FLOAT), это не подтверждённая причина.
- [ ] Найти проход, рисующий силуэты; проверить depth/blend и HDR resolve.

[Результаты](xbox/gpu-port/SHADER-COLOR-ISOLATION.md). Подмена отключена по умолчанию.


## 22 октября: четыре материала и композиция

- [x] Изолированы четыре PS: дорога, основной город, водная поверхность и небольшой проход24indices; силуэты сохраняются.
- [x] Снят заключительный PS80C8053F5C552351 и реальные константы, без изменения отрисовки.
- [x] Финальная сборка641 self-tests, exit0, без открытия Activity.
- [ ] Проверить1×1 экспозицию и HDR resolve→texture, затем динамическую LUT.

[Подробности](xbox/gpu-port/CITY-PASS-ISOLATION.md). Исправление артефактов пока не найдено.


## 22 октября: реальные GPU-данные экспозиции

- [x] Добавлен opt-in readback staging-buffer без записи в гостевую память.
- [x] Восемь образцов экспозиции обновляются и стабилизируются на512/65535; нулевой буфер в выборке не подтверждён.
- [x] Снят PS8F5851C1A202047B: совместно читает глубину/HDR и рассчитывает веса смешивания.
- [x] 641 self-test, exit0; просмотр оставлен закрытым.
- [ ] Проверить depth resolve и веса postprocess в проблемных областях.

[Подробности](xbox/gpu-port/EXPOSURE-GPU-READBACK.md).


## 22 октября: глубина и контроль depth-weighting

- [x] GPU-readback15 точек в3 кадрах с учётом tiled/endian; глубина конечна и ненулевая в выборке.
- [x] Проверено различие guest24_8_FLOAT и hostR32_SFLOAT; формула conversion согласуется с CPU-декодированием.
- [x] Контроль HDR sample вместо depth-weighted RGB: силуэты сохраняются, обход отвергнут как исправление.
- [x] Финальные641 self-test,exit0; просмотр закрыт.
- [ ] Сопоставить HDR перед фильтром и alpha промежуточных буферов.

[Подробности](xbox/gpu-port/DEPTH-GPU-READBACK.md).


## 22 октября: HDR до фильтра и resolve

- [x] Получены RGB/alpha15 точек RGBA16F; в выбранной левой тёмной области RGB уже0 до depth-aware фильтра.
- [x] Alpha0 в выборке, но этот проход использует только RGB входного цвета.
- [x] Прослежены записи128E0000: несколько источников и частичные resolves, exp_bias−3.
- [x] Финальные641 self-test,exit0; просмотр закрыт.
- [ ] Прочитать проблемную точку после каждой пересекающей resolve-записи в одном кадре, проверить split/alias.

[Подробности](xbox/gpu-port/HDR-GPU-READBACK.md).


## 22 октября: последовательность HDR resolve

- [x] Добавлен ограниченный последовательный GPU-readback target/control после пересекающих memory-range resolve.
- [x] В16 парах подтверждено различие target/control внутри одного кадра; ранние нули не объявляются ошибкой.
- [x] Установлено ограничение: memory extent не заменяет copy rectangle, позднее target может стать ненулевым.
- [x] Финальные641 self-test,exit0; просмотр закрыт.
- [ ] Снять точный copy rectangle и mapping split-resolve; сравнить source render target.

[Подробности](xbox/gpu-port/RESOLVE-POINT-SEQUENCE.md).


## 22 октября: точные resolve rectangles

- [x] Сняты64 HDR-прямоугольника в одном запуске с target/control readback.
- [x] Split1280×512 +1280×208 использует смещение512строк; обе точки внутри первого split.
- [x] Простое неправильное смещение второго фрагмента не подтверждено.
- [x] Финальные641 self-test,exit0; просмотр закрыт.
- [ ] Сравнить packed EDRAM до conversion/clear с resolved RGBA16F.

[Подробности](xbox/gpu-port/RESOLVE-RECTANGLES.md).


## 22 октября: EDRAM до конвертации

- [x] Добавлена staging-копия packed EDRAM до conversion/clear, mapping после fence.
- [x] В отдельном fallback1X запуске получены12 samples; есть исходный zeroRGB при ненулевой контрольной точке.
- [x] Контроль7e3 после масштаба−3 согласуется с ранее считанным HDR-цветом; это не полная валидация conversion.
- [x] Fallback визуально не убирает силуэты, по умолчанию отключён.
- [x]641 self-test,exit0.
- [ ] Сравнить host image→EDRAM dump, отдельно проверить MSAA split, исключённый из1Xпробы.

[Подробности](xbox/gpu-port/EDRAM-SOURCE-PROBE.md).


## 22 октября: MSAA source lanes

- [x] Реальный split-проход подтверждён как2X MSAA,selectk01.
- [x] В проверенной паре оба target samples0 доresolve, оба контрольных samples содержат ожидаемый цвет.
- [x] Эксперимент occlusion не дал срабатываний подмены; гипотеза не считается исключённой.
- [x] Финальные641 self-test,exit0; просмотр закрыт.
- [ ] Считать host MSAA image доdump, подтвердить tile owner и сравнить с packed EDRAM.

[Подробности](xbox/gpu-port/MSAA-SOURCE-PROBE.md).


## 22 октября: исходный host MSAA image

- [x] Найден owner00C88400,RGBA16F1280×1024,нативный2X(samples1/0).
- [x] Скомпилирован и подключён samplerless compute-readback доEDRAM dump,32 RGBA records.
- [x] TargetRGB уже0 в host image,контроль ненулевой; zero существует доdump/resolve в проверке.
- [x] Уточнено описание direct resolve: текущий SDK сам вызываетDumpRenderTargets.
- [x]641self-test,exit0;просмотр закрыт.
- [ ] Наблюдать host color после draw/clear с привязкойVS/PS/depth,найти первый проход.

[Подробности](xbox/gpu-port/HOST-COLOR-PROBE.md).


### 2026-10-22: завершение ограниченной ветки глубины

- [x] Парный замер входящей и сохранённой глубины одного вызова с проверкой покрытия.
- [x] Результат сохранён: ошибка глубины не подтверждена; экспериментальные режимы выключены по умолчанию.
- [ ] Проверить фактические выборки трёх текстур освещения материала зданий и промежуточный цвет.
- [ ] Подтвердить причину артефактов и исправление на обычном рендере.

- [x] Прочитать12 исходных BC1-блоков трёх lightmap-текстур из GPU shared memory; обнаружены различные цветовые данные.
- [ ] Сверить эти данные с VkImage и фактической выборкой материала (пока не проверено).

- [x] Исправить lifecycle последовательных GPU-readback в lightmap-диагностике; ранние чёрные tf1/tf2 признаны недостоверными.
- [x] Сопоставить192 source BC1/VkImage texels:3×64,mip0,2048×2048; все в допуске0.005.
- [ ] Проверить реальные UV/mip/filtering и промежуточный цвет материала.

- [x] Проверить широкую гипотезу затенения unlit-тестом4 материалов: силуэты сохранились; shadow-only проход не установлен, причина не подтверждена.


### Android → game input

- [x] Реальный START контроллера принят XamInputGetState; нажатие16 и отпускание0 подтверждены журналом.
- [ ] Пройти стартовый экран: остановка на XamShowSigninUI,exit42; пользователь подтвердил отсутствие изменения экрана.
- [ ] Постоянный локальный профиль,signin-state/XUID/settings и согласованные уведомления UI.
- [ ] Меню/загрузка уровня и реальные сохранения после этого ещё не проверены.

### 2026-10-22: сборка локального входа в профиль
- [x] Повторная проверка блокера1FBF3004: уже снят в VBLANK-SEMAPHORE.md (source0 handler сам сбрасывает1→0); дополнительных изменений GPU path не требуется.
- [x] Сборка после обрыва агента: entry.o компилировался, линковка падала duplicate symbol — XamShowSigninUI/XamUserGetSigninInfo реализованы в entry_probe.cpp, но отсутствовали в$taskImplemented build.ps1. Добавлены; сборка прошла.
- [x] Новый бинарник на Y700:646 self-tests,probe_exit0; work/startup/signin-build-self-test.log.
- [ ] Self-tests именно для XamShowSigninUI/XamUserGetSigninInfo ещё не добавлены; 646 — регрессии.
- [x] Игровой прогон с контроллером: APK17:44 (LocalPlayer/MEP1) установлен install -r без очистки данных; ME_GUEST_PREVIEW=1 ME_GUEST_INPUT=1 --gpu-explore-pool-scan, внешний timeout300. Журнал work/startup/signin-game.log.
- [x] Профиль от приложения получен (local_player_identity_received). Первое нажатие START (seq1207) вход не вызвало; второе (seq1312) → XamShowSigninUI → local_signin_completed, user0 «Runner», offline, 18 доставок уведомлений. Причина, по которой первое нажатие не сработало, не разбиралась.
- [x] Следующая граница: __imp__XMsgStartIORequest (не Ex), app r3=0xFB, LR829f9abc, thread61001000, exit42. Экран не изменился: последний кадр «Press START button» (work/startup/signin-screen.png).
- [x] Связать XamUserCreateAchievementEnumerator с local_player_signed_in: после входа создаётся настоящий handle (диапазон0x60000), без профиля по-прежнему1317 и outputs не меняются.
- [x] XamEnumerate для перечислителя достижений: метаданные из настоящего XDBF-ресурса тайтла45410850 (XACH44, сумма1000G, язык1); раскладка X_ACHIEVEMENT_DETAILS36байт+строки464 как в SDK; 122 при малом буфере, 18 в конце, 6 при неверном handle; NtClose закрывает.
- [x] XMsgStartIORequest app0xFB msg0x000B0006 (XUserSetContextEx), caller sub_829F9A78: буфер24байта {user, неинициализированное слово, u64 0, context id, value}. Значение хранится в памяти для user0; без профиля 0x80070525/last error0x525, user≥4 → 0x80070057/87.
- [x] Self-tests: 718 проверок, probe_exit0 на Y700 (work/startup/xgi-achievements-self-test.log), включая UTF-8→UTF-16, пагинацию, offset, flags0/7.
- [x] Игровой прогон work/startup/xgi-achievements-game.log: вход → перечислитель создан (flags0xFFFFFFFF, count50) → получено 44 достижения → context0=0 и 0x8001(presence)=1 → следующая граница __imp__XamShowDeviceSelectorUI, LR829bca88, thread610d1000, exit42.
- [ ] XamShowDeviceSelectorUI: выбор устройства хранения для сохранений. Реализовывать вместе с постоянным хранилищем сохранений (RELIABILITY-AND-DEVICES.md), без фиктивного device id.

Допущения XGI/достижений (не подтверждены консолью):
- Ни одно достижение не помечено полученным: у нового локального профиля разблокировок нет. Запись разблокировок (msg0x000B0008) не реализована и даст явную остановку.
- Ссылка на строку0xFFFF считается намеренно пустой строкой (unachieved у достижений80/81). Любой другой отсутствующий id считается ошибкой данных.
- Язык достижений = язык консоли1 (English); если таблицы языка нет, берётся язык XSTC по умолчанию, как в SDK.
- Значения XUserSetContextEx не проверяются по таблице контекстов SPA и не сохраняются между запусками; они живут только в сессии. Context id0 игра передаёт сама, смысл не установлен.
- Overlapped-варианты XMsgStartIORequest/XamEnumerate, другие тайтлы/XUID и другие сообщения XGI дают явную остановку, а не success.

Допущения текущей реализации входа (не подтверждены игрой):
- Вход завершается сразу, без системного UI: только при count==1, иначе87. SDK xam_user.cpp:478 тоже не показывает UI.
- Профиль ещё не получен от приложения (нет пакета MEP1) →21 ERROR_NOT_READY, без success. Код выбран нами, не взят с консоли.
- Порядок уведомлений: XN_SYS_UI(9)=1, XN_SYS_SIGNINCHANGED(0xA)=1, XN_SYS_UI=0. SDK шлёт только0xA и9=0; показ UI=1 добавлен нами.
- signin_state=1 (локальный, не Xbox Live); имя по умолчанию «Runner».
- XUID: постоянный в SharedPreferences offline-player-v1 (диапазон E000…). Он не входит в сохранения Xbox и не экспортируется; его потеря при очистке данных приложения сделает прогресс другим профилем — учесть в backup до передачи второму тестеру.
- XamUserGetSigninInfo при ошибке не трогает output (SDK обнуляет); соответствует политике проекта.
