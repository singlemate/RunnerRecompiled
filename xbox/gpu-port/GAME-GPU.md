# GPU в реальном старте Mirror’s Edge — 21 октября 2026

Настоящий AOT startup игры впервые вызвал успешный SetupExternalGpu: STATUS_SUCCESS, VulkanCommandProcessor на Adreno 830 инициализирован внутри того же процесса, который исполняет PPC AOT. MeRexGpuMemory ссылается на существующие gpu_physical и gpu_mmio загрузчика; второй memfd/снимок игровой памяти не создаётся. GPU получает execution_gate, который отпускается владельцем на время StartReady, а затем используется CP. Результат подтверждён device-game-gpu-start.log, событием game_gpu_setup_result/status=00000000 и следующим исполненным игровым кодом. Это не прежний отдельный synthetic CP probe.

## Связь с загрузчиком

Добавлены gpu_bridge.h/.cpp и link_gpu_loader.py. Bridge компилируется отдельно от XenonRecomp заголовка и принимает непрозрачные pointers адаптеров, gate functions и integer-only interrupt function. SDK не получает PPCContext XenonRecomp. SDK/helper объекты, bridge, loader и игровой AOT archive статически связаны в один executable, с --no-undefined. Все proprietary/generated данные по-прежнему только work/. Адаптер MeRexGpuMemory не копируется между ABI.

VdInitializeEngines теперь возвращает 1 только после настоящего SetupContext; при отказе останавливается GPU_real_context_setup_failed. r4 этого API — callback очистки, он НЕ используется как graphics IRQ. Для IRQ выделены собственный loader PPCContext/stack/PCR/KTHREAD/TLS и MeGpuInterruptWorker. Игра отдельно вызывает VdSetGraphicsInterruptCallback: зарегистрировала 0x82813F58 с argument 0x1004B680. Реальные IRQ в текущем run не генерировались. Автоматического vblank ещё нет; это явно отмечено в диагностике.

Реализованы forwarding imports VdInitializeRingBuffer и VdEnableRingBufferRPtrWriteBack с проверкой allocation/границ/выравнивания. В текущем игровом run они ещё НЕ вызваны. Нельзя считать, что уже прошли настоящие игровые PM4 пакеты: установка кольца и чтение ресурсов находятся дальше по startup.

## Следующие пройденные вызовы

ExRegisterTitleTerminateNotification сохраняет routine/priority и удаляет первое совпадение по routine, как изученный SDK. Проверяется guest descriptor 16 B и AOT mapping, capacity 64. Игра зарегистрировала 0x82818638 и 0x828186B0. Это регистрация; execution callbacks на завершении title ещё не реализован. Probe diagnostic _exit — не нормальный title shutdown. VdShutdownEngines остаётся unsupported, фиктивного teardown/reinit нет.

VdGlobalDevice и VdGlobalXamDevice получили по одной 4-byte BE ячейке по 32-byte aligned guest addresses 0x60003000/0x60003020. Начальные значения 0; игра записала свою D3D device pointer вместо fault прежнего guard. Это память guest exports, не подставленный host device. Ячейки в metadata virtual region, не физической heap: MmGetPhysicalAddress для них пока не поддержан. Прочие неизвестные video variables по-прежнему защищены guards.

После этого настоящий startup остановился на __imp__ExGetXConfigSetting, LR=0x828187B0, category=3, setting=10 (видно в AOT callsite). API ещё не реализован, фальшивый успех не возвращается. Следующий шаг — разобрать и реализовать конкретную guest video config, затем дойти до создания игрового ring и первых PM4. Изображения, меню и draw по-прежнему нет.

## Проверки и ограничения

На подключённом Y700 прошли 214/214 loader self-tests, включая добавленные register/remove callbacks и game-device export cells. Финальный настоящий startup завершился ожидаемым diagnostic exit 42 на новом syscall; log сохранён. Новый native executable 130843456 B установлен в /data/local/tmp/me_native_memory_probe. APK не менялся, файлы игры/sections/header bundle не менялись. После запросa USB trust serial test device снова доступен.

GPU/IRQ state в bridge рассчитан на время жизни ограниченного probe-процесса; stop через _exit завершает host workers. Cleanup частичного Vulkan setup, повторная инициализация, normal process lifetime, automatic vblank, allocation retention при in-flight submissions и presentation требуют дальнейшей работы. Alarm игрового probe увеличен с 5 до 20 s; adb shell timeout 25 s. Ошибка/зависание не оставляет фоновый бесконечный probe. Это технические ограничения текущей сборки, не готовый игровой runtime.

Сборка: startup/build.ps1 теперь включает bridge и все 80 уже собранных SDK объектов через link_gpu_loader.py. Перед ней на чистом workspace нужны GPU build.ps1 и приватный build_guest_mmio.py. Изменения SDK в этом этапе не требовались; cumulative SDK patch из предыдущего этапа остаётся актуален.
