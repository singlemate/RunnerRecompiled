# Внешний host-worker CommandProcessor — 21 октября 2026

В настоящий CommandProcessor SDK добавлен UseExternalHostWorker(), который вызывается один раз до Initialize. Выбранный HostWorker использует std::thread, а не XHostThread/KernelState. Initialize передаёт ему настоящий WorkerThreadMain; Shutdown останавливает основной loop, сигналит event, будит возможную паузу и join; ошибки worker сохраняются и доступны через failure(), Shutdown логирует их. IsCurrentThread использует thread_local identity, не обращается к std::thread object в момент его запуска.

Pause/Resume внешней ветки работают через condition_variable вместо принудительного Suspend. RequestPause происходит до постановки pause-задачи: ранний Resume не теряется. Join выполняет Resume. Владелец обязан вызывать Shutdown до уничтожения CP, не join из worker и не удерживать execution_gate, если worker его ожидает. Ошибка SetupContext/готовность GPU пока не передана синхронно в Initialize — до реального подключения требуется readiness handshake и обработка startup failure.

Исправлен доступ к pending_fns_: push/pop/empty защищены mutex, fn вызывается вне mutex, постановка задачи сигналит write_ptr event. Это проверено сборкой настоящего CP, но его конкурентное end-to-end выполнение ещё не тестировалось.

На Y700 192/192 loader checks. Добавленные восемь проверок запускают настоящий SDK HostWorker отдельно от CP: identity, ожидание pause до Resume, отказ второго Start, join, ранний Resume до PauseCurrent и сохранение exception. Эти проверки НЕ означают, что полноценный графический поток запущен: GraphicsSystem/CommandProcessor в loader по-прежнему не созданы, UseExternalHostWorker на игре ещё не вызван.

Native binary установлена ADB; APK не менялся. Прежняя подтверждённая остановка игры — VdInitializeEngines_requires_xenos_backend, GPU false. Реальный старт после только этого изменения не повторялся. Следующие необходимые части: GPU worker ↔ execution_gate на командных/ожидающих границах, синхронное подтверждение SetupContext, внешний VSync/video-mode и выбор adapters при создании GraphicsSystem. Legacy runtime branches сохранены; полная link пока не завершена. Изображения игры нет.

Совокупный rexglue-android-runtime.patch обновлён и включает новый host_worker.h. Применяется один на чистый pinned SDK, без добавления прежних patches.

Проверка сборки: 80/80 GPU/helper TU собраны; shader/bootstrap/host-service executables слинкованы, повторно на устройстве не запускались. Полная строгая link остаётся ошибкой с 12 external symbols (legacy ветки ещё присутствуют), plugin.so не готов.
