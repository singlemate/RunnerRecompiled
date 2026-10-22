# Блокировка гостевого исполнения в CP — 21 октября 2026

SDK CommandProcessor получил SetGuestExecutionHooks(enter, leave), который вызывается после UseExternalHostWorker и до Initialize. Hooks парные; повторный Initialize запрещён, менять worker/hooks после начала initialization запрещено. Для loader hooks должны вызывать execution_gate.lock/unlock. Init/Shutdown/Join нельзя выполнять владельцу, удерживающему этот gate, иначе worker не сможет закончить setup/cleanup. Автоматического подключения GraphicsSystem пока нет.

GuestExecutionGuard приобретает блокировку, поддерживает идемпотентные Release/Acquire, освобождает на выходе/exception. GuestExecutionBinding устанавливает worker-local active scope и восстанавливает previous pointer при выходе. Реальный CommandProcessor использует scope для внешнего SetupContext, каждой итерации worker (pending functions, чтение/исполнение primary ring, writeback) и ShutdownContext.

PrepareForWait/ReturnFromWait отпускают/возвращают active scope. В проверенном SDK ни Vulkan, ни D3D12 эти методы не переопределяют. Это применяется к idle ring loop и длинной WAIT_REG_MEM. Короткая WAIT_REG_MEM ранее только вызывала MaybeYield — теперь тоже освобождает gate и проверяет остановку. Внешняя Pause отпускает gate до condition_variable и возвращает после Resume. Hooks, особенно leave, должны быть корректны и не бросать исключение.

Y700: 209/209 loader checks. Девять новых проверок используют настоящий SDK guard: парность hooks, idempotent release/reacquire, restoration scope pointer, exception unwind. В конкурентной проверке HostWorker блокируется на настоящем execution_gate, пока guard не вызовет Release, затем завершает работу; guard повторно получает lock. Это проверка механизма, НЕ end-to-end тест PM4 WAIT_REG_MEM/паузы самого CommandProcessor.

Все 80 GPU/helper TU собраны; 35/35 GPU TU повторно скомпилированы после финального уточнения предиката setter. Legacy runtime dependencies остаются, строгая full plugin link возвращает ошибку с 12 unresolved symbols. На устройстве обновлён только native loader и запущен self-test, APK и XEX не менялись, реальный game startup в этом этапе не повторялся. Последняя подтверждённая остановка — VdInitializeEngines_requires_xenos_backend, GPU false.

Следующий шаг — внешний путь GraphicsSystem::SetupGuestGpu без обязательных FunctionDispatcher/KernelState и внешний VSync/video-mode. После него потребуется отдельный process для настоящего Vulkan CP SetupContext. До этого нельзя считать графический worker запущенным. Не решены allocation retention на время in-flight GPU submissions, cleanup частично созданного Vulkan context при failure и полная синхронизация video/interrupt callbacks; они требуют реальной интеграционной проверки. Изображения игры нет.

Обновлён cumulative rexglue-android-runtime.patch, включая guest_execution_guard.h. Применять только этот патч к чистому pinned SDK.
