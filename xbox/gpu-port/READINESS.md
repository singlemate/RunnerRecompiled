# Подтверждение готовности worker — 21 октября 2026

SDK HostWorker::StartReady(setup, run) ожидает promise/future, который worker заполняет только после завершения setup. При false основной run не вызывается, thread присоединяется, возвращается false. При exception setup future передаёт отказ владельцу, Join гарантирует сохранение exception_ptr в failure(), возвращается false. Успешный setup означает готовность в этот момент, но не гарантирует отсутствие последующей ошибки run.

Настоящий CommandProcessor во внешней ветке вызывает StartReady(SetupContext, WorkerThreadMain(true)). Initialize теперь не возвращает true только по факту создания std::thread. Legacy XHostThread-ветка сохранена с прежним поведением. WorkerThreadMain(true) пропускает повторный SetupContext, штатный ShutdownContext после loop остаётся на GPU worker. Cleanup частично созданного Vulkan context при setup-failure ещё необходимо отдельно проверить; автоматический retry не поддержан. Проверка не вводит timeout/cancellation зависшего driver call: владельцу нельзя удерживать locks, которые нужны setup. На тестовом запуске процесс ограничен adb shell timeout 10.

На Y700 200/200 loader checks. Дополнительные восемь проверяют настоящий SDK HostWorker с тестовыми setup callbacks: setup выполняется в worker и завершён до возврата StartReady, run вызывается только при успехе, false и exception исключают run, exception сохранено, пустой setup отвергается. Это НЕ создание реального VulkanCommandProcessor. Полноценный графический worker инициализацию GPU ещё не проходил.

Все 80 GPU/helper TU пересобраны, отдельные shader/bootstrap/host executables слинкованы; на устройстве в этом этапе запускался только loader self-test. Строгая full GPU link по-прежнему заканчивается ошибкой с 12 legacy runtime symbols. plugin.so не готов. APK/XEX не менялись, native loader обновлён ADB. Последний подтверждённый настоящий game startup (из предыдущего этапа) останавливается на VdInitializeEngines_requires_xenos_backend; изображения нет.

Следующая конкретная работа: связать доступ CP с execution_gate, отпуская его на ожидании/паузе; подключить внешний VSync/video-mode и внешний путь SetupGuestGpu без FunctionDispatcher/KernelState; только затем создавать настоящий Vulkan CP, проверять device setup и передавать первые команды. Нельзя просто вернуть VdInitializeEngines=true.

Совокупный rexglue-android-runtime.patch обновлён; применять отдельно на чистый pinned SDK, не вместе с историческими patches.
