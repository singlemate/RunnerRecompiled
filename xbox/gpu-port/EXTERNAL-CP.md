# Настоящий Vulkan CommandProcessor на Y700 — 21 октября 2026

Достигнут первый полный native headless запуск GPU: SetupExternalGpu вернул STATUS_SUCCESS, настоящий VulkanCommandProcessor создал контекст на Adreno 830/API 1.3.284 и host worker. MMIO CP_RB_WPTR принял индекс 2, CP обработал синтетический Type0 пакет записи PA_SC_WINDOW_OFFSET (0x2080)=0x1234 и записал big-endian read pointer 2 в memfd. Затем GraphicsSystem::Shutdown завершил worker и Vulkan context; процесс вернул 0. Лог device-real-cp-check.log.

В тесте НЕ использовались игровые данные, AOT функции или память работающего loader: отдельный процесс создал memfd 512 MiB virtual reservation с одной доступной страницей 4 KiB и MeRexGpuMemory того же класса. Это проверяет реальные SDK GraphicsSystem/CommandProcessor/PM4, MMIO, guest gate, readback и lifecycle. Это НЕ draw, меню, игра, EDRAM rendering или показ изображения. IRQ dispatcher теста отвергает запросы; прерывания в этом сценарии не генерировались. GPU-путь loader пока не подключён, его VdInitializeEngines всё ещё намеренно останавливается. Последний loader self-test 209 относится к предыдущему этапу, повторно здесь не запускался.

## Внешний runtime

Добавлен SetupExternalGpu(GpuMemory, GuestExecutionHooks, interrupt dispatcher, width/height/refresh). Он создаёт provider без presentation, выбирает внешний HostWorker, передаёт gate, ждёт настоящий SetupContext и регистрирует MMIO. QueryGuestVideoMode выдаёт заданный guest profile; Vulkan/D3D swap code использует его вместо прямой зависимости от kernel video. Профиль не объявляется параметрами физического дисплея.

Vblank во внешнем режиме должен явно вызывать владелец через PumpExternalVblank под gate; автоматического VSync thread пока нет. Не удерживать gate при Initialize/Shutdown/Join. Периодическое pumping и игровая interrupt delivery ещё требуют интеграции с loader. MMIO binding живёт в memory adapter: в тесте оно явно снято после Shutdown, до уничтожения GraphicsSystem. Запуск повторно на том же частично инициализированном объекте не поддержан; cleanup setup-failure ещё не аудирован, probe на failure завершает процесс.

## Полная линковка

В согласованной сборке всех 80 объектов задан REX_GPU_EXTERNAL_HOST=1. Legacy XHostThread/FunctionDispatcher ветки и window setup исключены условной компиляцией, не заменены фиктивными реализациями. SetupGuestGpu legacy и SetupPresentation в этом варианте возвращают NOT_IMPLEMENTED; прикрепить Window к Presenter запрещено. SDK без этого define сохраняет прежние ветки, но его полная сборка в этом этапе не проверялась. Это headless build, из него нельзя автоматически получить экран Android.

Строгая shared link с --no-undefined впервые прошла с 0 внешних неразрешённых символов. Приватный результат work/gpu-port/librexgpu_xenos_external.so и CP probe остаются в work/. Старый external-gpu-build.log включает промежуточный audit с одним video symbol; актуальный итог подтверждают link-integration-extended-result.json (0/linked=true), external-final-link.log и успешная линковка executable. После исправления video route заново собраны 35 GPU TU; остальные 45 объектов той же headless конфигурации сохранены.

## Воспроизведение и следующий шаг

build.ps1 теперь собирает и link_command_processor_check.py; исходник probe command_processor_check.cpp использует SDK, а не извлечённые/имитированные методы. Запуск на устройстве /data/local/tmp/me_vulkan_cp_check ограничен alarm 45s и shell timeout 50s. Финальная версия с detach MMIO повторно прошла exit 0. APK, исходная игра и loader binary в этом этапе не менялись.

Дальше нужно подключить созданный GraphicsSystem к реальной памяти loader, его interrupt worker и игровым Vd imports, затем проверить первые настоящие игровые пакеты. Не считать успешный синтетический Type0 доказательством совместимости всех PM4, shaders, allocation lifetime или игровых ресурсов. До изображения и работающего уровня ещё существенная работа.

Обновлён cumulative rexglue-android-runtime.patch (пять новых headers + все изменения SDK); применять один к чистому pinned SDK c94f5ebdcb3c9d1a460ca48e04f9758448f8d518. Build scripts отдельно задают headless define во всех object groups.
