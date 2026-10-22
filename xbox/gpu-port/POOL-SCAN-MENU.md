# Обход свободных workers и пакет меню

2026-10-22. Xbox 360 → Android ARM64.

## Причина наблюдавшегося зависания

Сырые PowerPC-инструкции исходного decoded image подтверждены независимо от C++-генерации:

- 8266A8C8: повторно прочитать pool.count;
- 8266A8D0: index=count-1;
- 8266A8F0: вызвать getter worker.field32;
- 8266A8FC: вычислить index-1 в r11;
- 8266A900: bge8266A8C8, то есть снова сбросить index.

При flag0 и последнем worker.field32=1 цикл не переходит к предыдущему worker. Field32 устанавливается конструктором для CPU2, не является completion/busy. Поэтому обнуление этого поля было бы неверным исправлением.

Трассировка return/enqueue установила зависимость от порядка завершения первых заданий:

- work/startup/pool-transition-long.log: CPU2-worker1014D3E0 возвращается раньше normal1014D3B0; массив [reserved,normal], загрузка идёт дальше.
- work/startup/loading-simple-long.log: normal возвращается раньше reserved; массив [normal,reserved], ранний timeout44.

Это подтверждает конкретное условие бесконечного обхода исходного алгоритма в нашем runtime. Почему штатное исполнение Xbox избегает этого условия и связан ли случай с версией игры/обновлением, ещё не установлено. Полноценное планирование по-прежнему не реализовано.

## Отдельный совместимый эксперимент

prepare_pool_scan_diagnostic.py проверяет исходные байты357EFFFF4080FFC8 и добавляет условный обход в приватную generated TU97. В режиме --gpu-explore-pool-scan index уменьшается и переход идёт к проверке предыдущего worker без повторного чтения count. Если подходящих нет, выполняется исходная ветка очереди. Getter, CPU2 flag, dispatch, удаление worker из массива и критические секции остаются гостевыми.

По умолчанию исходный путь сохранён; оригинальные default.xex/SVOD не изменяются. Режим также включает уже существующие диагностические poll-yield и guarded instruction helpers. Лимит этого режима240с, обычного long-poll-yield120с. Это проверяемый compatibility patch, не доказательство точной эмуляции scheduler Xbox.

## Проверка на устройстве

470 self-tests прошли на Y700, work/startup/pool-scan-self-test.log, exit0. Новые сценарии исполняют настоящую перекомпилированную8266A880 с контролируемыми guest objects: исходный подходящий порядок, экспериментальный обратный порядок, очередь при наличии только reserved, flag1 и пустой свободный пул. Проверяются фактический virtual dispatch, оставшийся массив и отсутствие изменения CPU2 flag.

Первый игровой прогон эксперимента work/startup/pool-scan-long.log прошёл до запроса TdMainMenu.xxx. LoadingSimple.bik полностью прочитан (179428байт); это не подтверждает вывод ролика. Первый прогон имел подходящий порядок, поэтому сам по себе не доказывает устранение проблемного порядка.

TdMainMenu.xxx40763392байт извлечён из пользовательского SVOD и передан в приватную read-only папку устройства. Следующий прогон work/startup/tdmainmenu-pool-scan.log пишет первые фактические срабатывания изменённого обхода как pool_scan_compatibility_step. Подготовка пакета не равна подтверждению его чтения или вывода меню.

## Результат прогона с пакетом меню

В work/startup/tdmainmenu-pool-scan.log зафиксирован именно проблемный порядок [normal1014D3B0,reserved1014D3E0]. Исправленный обход перешёл1→0, выбрал normal, enqueue завершился. Затем TdMainMenu.xxx полностью прочитан: handle30040, последний offset40632320/read131072, сумма40763392. Прогон завершился exit42 на следующем импорте XamUserReadProfileSettings (caller829C29B4). Это подтверждает прохождение конкретного pool stall, но ещё не визуальное меню и не общую устойчивость scheduler.

Для XamUserReadProfileSettings добавлена только синхронная диагностическая ветка отсутствующего локального пользователя: проверка аргументов, возврат1317 без записи фиктивных настроек и изменения size/data. XUID-array и overlapped явно неподдержаны. Постоянного профиля и сохранений пока нет. Все478 self-tests прошли на устройстве; лог work/startup/profile-read-self-test.log. Следующий игровой прогон — work/startup/profile-read-game.log.

В profile-read-game.log игра обработала два отказа1317 для3settings и продолжила до XamUserGetName (caller829B2DF4, exit42). Для имени добавлен соответствующий absent-profile HRESULT80070525 без изменения output; invalid arguments80070057. Теперь484 self-tests прошли на Y700, work/startup/user-name-self-test.log. Следующий игровой прогон — work/startup/user-name-game.log.

В user-name-game.log игра обработала GetName(user0,capacity32)→80070525 и продолжила до KeQuerySystemTime (caller829FB344). Для этого void API добавлено CLOCK_REALTIME в100нс от1601-01-01 UTC, BE64 output; NULL допускается как в pinned SDK. Невалидный output/ошибка host clock явно останавливают probe. Возвратные регистры не подменяются.488 self-tests прошли на устройстве, work/startup/system-time-self-test.log. Следующий прогон — work/startup/system-time-game.log.

В system-time-game.log вызов KeQuerySystemTime выполнен (system_time_utc), игра продолжила до ExGetXConfigSetting(category3,setting1), caller82A099DC, exit42. Параметры подтверждены по исходному гостевому caller: r3=3,r4=1,buffer_size4. Pinned SDK обозначает setting1 как XCONFIG_USER_TIME_ZONE_BIAS. Это следующая граница; новые timezone settings пока не реализованы. Все перечисленные прогоны завершены.
