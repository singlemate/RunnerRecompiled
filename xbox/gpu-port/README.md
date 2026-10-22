Текущий этап: [XEX resources и реальные файлы](STARTUP-RESOURCES-FILES.md). TOC и Coalesced.ini прочитаны; новая остановка NetDll_XNetStartup. 333 проверки, GPU31/31.

Текущий этап: [отсутствующий debug volume](DEBUG-VOLUME.md). ShaderDumpxe query пройден, новая остановка KeEnableFpuExceptions; 306 проверок, GPU31/31. Полная файловая система ещё не реализована.

Текущий этап: [PCM submit](PCM-SUBMIT.md). AAudio принял стартовые игровые кадры, callback вернулся. Следующая граница NtQueryFullAttributesFile; 303 проверки, GPU31/31.

Текущий этап: [multiple-wait и выход на PCM submit](MULTIPLE-WAIT.md). Новые границы XAudioSubmitRenderDriverFrame и NtQueryFullAttributesFile. 294 проверки; GPU31/31.

Текущий этап: [AAudio endpoint и XAudio callback](AAUDIO-CLIENT.md). STARTED и вход callback подтверждены; новые зависимости wait-multiple/ANSI/volume-mask. 282 проверки; PCM ещё не подключён.

Текущий этап: [аудиопоток и IRQL](AUDIO-THREAD.md). Suspended/resume прошли, следующая остановка XAudioRegisterRenderDriverClient. 278 проверок; GPU31/31.

Текущий этап: [kernel semaphores](SEMAPHORES.md). Игра создала аудиосемафор, следующая остановка — флаги ExCreateThread. 269 проверок прошли; GPU31/31.

Текущий этап: [XMA context memory](XMA-CONTEXTS.md). Игра получила настоящий адрес XMA-контекстов; GPU read/write31/31. Следующая остановка KeInitializeSemaphore. 259 проверок прошли.

Текущий этап: [raised-IRQL spinlock](RAISED-SPINLOCK.md). GPU callback освободил оба lock и дошёл до IssueSwap; следующая остановка XMA MMIO 7FEA1800. 252 проверки прошли.

Текущий этап: [Vulkan pipeline и spinlock](PIPELINE-SPINLOCK.md). Драйвер создал игровой pipeline; GPU callback прошёл обычный spinlock, следующая остановка KeAcquireSpinLockAtRaisedIrql. 246 проверок прошли.

Текущий этап: [генерация VdSwap и исполнение PM4](SWAP-PM4.md). Read/write достигли 28/28, следующий блок 31/28 начал создание игрового graphics pipeline и не завершился до таймера. 242 проверки прошли.

Текущий этап: [системные GPU-метаданные и вход VdSwap](SYSTEM-BUFFER.md). Параметры VdSwap получены на планшете; генерация swap-пакетов ещё не реализована. 242 проверки прошли.

Текущий этап: [защита физической памяти](PHYSICAL-PROTECT.md). Четыре игровых вызова пройдены; следующая остановка VdGetSystemCommandBuffer. 242 проверки прошли, изображения пока нет.

Текущий этап: [EDRAM/HSIO](EDRAM-HSIO.md). Игра продолжила запуск при HSIO=false; следующая остановка MmSetAddressProtect. GPU read/write=25/25, 242 проверки; изображения пока нет.

Последний этап: [kernel waits и critical regions](KERNEL-WAITS.md), 242 проверки. Worker events работают; новая остановка VdRetrainEDRAMWorker, изображения нет.

Последний этап: [первые игровые PM4](FIRST-GAME-PM4.md) — в --gpu-explore read/write=25/25, 228 self-tests. Draw/изображение не подтверждены; default identifier stop сохранён.

Последний этап: [видеонастройка и настоящий игровой ring](XCONFIG-RING.md), 220 проверок. Ring 32 KiB создан; новая остановка — non-null system GPU identifier. Изображения нет.

Последний этап: [GPU в реальном startup игры](GAME-GPU.md), SetupContext SUCCESS, 214 self-tests; новая остановка ExGetXConfigSetting. Игрового ring/изображения ещё нет.

Последний этап: [настоящий Vulkan CP на Y700](EXTERNAL-CP.md): setup + PM4 Type0 + shutdown прошли, full link 0 unresolved. К игре GPU пока не подключён.

Последний этап: [блокировка гостевого исполнения в CP](GUEST-GATE.md), 209 проверок на Y700. Настоящий GPU CP ещё не запущен.

Последний этап: [готовность GPU-worker](READINESS.md), 200 проверок на Y700. Полный GPU ещё не запущен.

Последний этап: [внешний host-worker CommandProcessor](HOST-WORKER.md), 192 проверки на Y700. Полный GPU ещё не запущен.

Последний этап: [software physical invalidation](WRITE-WATCH.md), 184 проверки на Y700. CP/GPU пока не подключён.

Последний этап: [поток доставки GPU-прерываний](INTERRUPTS.md), 169 проверок на Y700. Полный GPU ещё не подключён.

Последний этап: [адаптер памяти GPU](MEMORY-INTERFACE.md), 148 проверок на Y700, 12 внешних символов при полной линковке. GPU ещё не подключён.

Последний этап: [MMIO из перекомпилированного кода](MMIO.md), 300/300 TU и 136 проверок на Y700. GPU пока не подключён.

Последнее изменение: [остановка при ошибке GPU-пакета](PACKET-FAILURE.md), 6 host-проверок; полная интеграция ещё не завершена.

Последний этап: [общая память и RingBuffer](SHARED-MEMORY.md), [результат](shared-memory-result.json). 121 loader-проверка и 10 host-проверок на Y700; до полной линковки остаются 16 runtime/UI символов.

# Android ARM64: перенос компонентов Xenos/Vulkan

21 октября 2026. На Y700 проверены **реальный переводчик шейдеров RexGlue** и **реальное создание VulkanInstance/VulkanDevice RexGlue**. Полный command processor ещё не связан с нашим загрузчиком и VdInitializeEngines всё ещё останавливает игру. Изображения Mirror’s Edge нет.

## Подтверждённый результат

- Все 35 исходных единиц rexgpu-xenos для выбранной конфигурации Vulkan успешно скомпилированы NDK r30, target aarch64-linux-android35, C++23. В составе — обработчик команд, SPIR-V translator и Vulkan backend.
- Собраны ещё 8 вспомогательных единиц core и 6 единиц Vulkan/UI bootstrap, исходные fmt/glslang и их статические зависимости. Включён RTTI у glslang, согласованный с наследованием SpirvBuilder в RexGlue.
- Из компонентов **без фиктивных реализаций отсутствующих служб** слинкованы два отдельных ARM64-процесса, оба реально выполнены на Y700.
- `shader_translation_check`: собственные минимальные Xenos vertex/pixel CF-программы EXEC_END переведены в SPIR-V размером 5684 и 4724 байта. Adreno создал оба VkShaderModule с VK_SUCCESS. Эти программы синтетические: они не взяты из игры, не проверяют её ALU/fetch/texture-пути, не создают graphics pipeline, не исполняют draw и не являются полной SPIR-V валидацией.
- `vulkan_bootstrap_check`: использованы непосредственно классы VulkanInstance/VulkanDevice RexGlue, а не только прямой тест NDK Vulkan API. Создано устройство Adreno 830/Vulkan 1.3.284 с with_gpu_emulation=true, без surface/swapchain. Реальный бэкенд сообщил maxStorageBufferRange=2147483647. Создание полного GraphicsSystem/command processor не выполнялось.
- Повторная сборка через сохранённый `build.ps1` завершилась успешно: 35+8+6 компиляций и обе линковки. Подробности result.json, repro-build.log, device-shader-check.log, device-rex-vulkan.log.

## Патч SDK

SDK остаётся на c94f5ebdcb3c9d1a460ca48e04f9758448f8d518; 14 используемых submodule checkout сверены с gitlinks. В submodules.json записаны фактические commits. Не загружались готовые SDK-бинарники, ранее заблокированные Defender; все исполняемые компоненты здесь собраны из исходников.

Патч `rexglue-android-gpu.patch` содержит:

1. Расширение уже существовавшего Apple libc++ clock_cast compatibility блока на Android NDK libc++. Арифметика преобразования часов не изменена; runtime-поведение всех chrono conversions отдельно не проверялось.
2. 64-битный аргумент ассемблерной инструкции записи FPCR на ARM64. Это не меняет layout PPCContext.
3. Отсутствовавший `rex/ui/surface_android.h`: собственная обёртка ANativeWindow с acquire/release и проверкой актуального ненулевого размера. После неё компилируется Android-ветка VulkanPresenter. Реальная Java Surface/Activity lifecycle и создание swapchain пока не проверены.

Полный CMake SDK по-прежнему требует отдельного переноса: верхний build выбирает Linux, а прежняя проверка POSIX fibers показала отсутствие getcontext/makecontext/swapcontext в NDK. Для текущего этапа используется отдельная сборка GPU-компонентов; не заявляется, что весь rexruntime собирается на Android.

## Что действительно мешает интеграции

Первая строгая линковка всех GPU-объектов без зависимостей дала 171 отсутствующий символ. После сборки core/glslang/Vulkan bootstrap повторная полная линковка с --no-undefined оставляет **86 символов** (link-integration-result.json). Это список зависимостей, а не 86 ошибок рекомпиляции игры. В него входят дополнительные UI/Vulkan utilities, VMA, memory/ring buffer, host threading, XThread/XHostThread и вызов гостевых interrupt callbacks через FunctionDispatcher.

Главная архитектурная работа — соединить **память и потоки GPU с той же гостевой памятью**, в которой уже работает игра. Создать второй изолированный Memory/KernelState и объявить успех нельзя: GPU тогда будет читать другие байты. Нужно согласовать physical aliases/memfd, invalidation/write-watch, MMIO и ring-buffer, затем callbacks и перевод между несовместимыми PPCContext XenonRecomp/RexGlue. Совпадение sizeof контекстов не даёт ABI-совместимости; смещения FPR и механизм вызовов уже измерены ранее.

План следующей интеграции: собрать оставшиеся независимые GPU/UI utilities; вынести и адаптировать контракт memory/thread/interrupt host services; подключить command processor к общей памяти; проверить packets и read-pointer writeback; только после этого разрешать игровому VdInitializeEngines возвращать успех. Далее потребуются реальные игровые шейдеры/ресурсы и Android presentation. Отсутствие на драйвере fragment shader interlock оставляет исследовать host-render-target path; совместимость конкретных сцен ещё не доказана.

## Воспроизведение

Из корня рабочего каталога запустить `outputs/mirrors-edge-native/xbox/gpu-port/build.ps1 -Workspace <абсолютный корень>`. Требуются уже установленные NDK r30, venv CMake/Ninja/Python и закреплённый SDK с перечисленными submodules и применённым патчем. Скрипт не скачивает зависимости и не меняет устройство. Он собирает собственные проверки и архив объектов, **не готовый GPU plugin**.

Рабочие бинарники находятся в work/gpu-port; здесь сохранены исходники проверок, патч, скрипты и журналы. GPU-компоненты не содержат извлечённого proprietary game code. APK и игровой AOT-loader в этом этапе не изменены; игра повторно не запускалась после прежней подтверждённой остановки VdInitializeEngines. Проверочные процессы завершились, драйвер и настройки устройства не менялись.
