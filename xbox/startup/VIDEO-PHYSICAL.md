# Видеорежим, физическая память и Vulkan — 21 октября 2026

На Y700 реальный запуск прошёл XGetVideoMode и первое выделение физической памяти. Теперь остановка VdInitializeEngines, LR 8281872C. Диагностический обработчик сохраняет параметры и останавливает процесс; инициализация Xenos не подменена успешным возвратом. Отрисовки и меню пока нет. Сводка video-physical-result.json.

## Гостевой режим

XGetVideoMode и VdQueryVideoMode записывают одинаковую структуру 48 байт в big endian: 1280×720 progressive, widescreen/HD, float 60 Гц, video standard 1, два совместимых служебных поля и нулевой reserved. Формат сверён с закреплёнными локальными xvideo.h и xboxkrnl_video.cpp RexGlue. Это профиль гостевой консоли, не фактический режим панели Android и не свидетельство наличия surface/swapchain. Служебные поля взяты из исследовательской модели RexGlue, их полная аппаратная семантика не установлена.

## Физическая память

Реализована отдельная модель MePhysicalMemory. Общий Linux memfd размером 512 МиБ служит backing store, страницы отображаются только для выделенных областей. По источнику xmemory.cpp поддержаны алиасы A0000000 + physical (64-КиБ представление), C0000000 + physical (16-МиБ представление) и E0000000 + physical − 1000 (4-КиБ представление). Все три MAP_SHARED-view действительно обращаются к одним байтам; они не являются тремя копиями буфера. Исходное 4-ГиБ адресное пространство принадлежит загрузчику, MAP_FIXED применяется только внутри него.

Поддержаны MmAllocatePhysicalMemoryEx для flags=0/RW и указанных cache hints, ограничение физического диапазона, выравнивание и размеры страниц 4 КиБ/64 КиБ/16 МиБ, MmGetPhysicalAddress, MmQueryAllocationSize и MmFreePhysicalMemory. Доступный исследовательский physical pool — 01000000..1FBFFFFF; нижние 16 МиБ пока оставлены для будущих GPU/kernel областей. Это не полная карта памяти Xbox и не ОС-доступ к физическим адресам Android.

PAGE_WRITECOMBINE/NOCACHE пока не воспроизводят аппаратную политику кэширования: CPU views являются обычной shared memory. Согласование с будущим Vulkan-buffer и GPU-кэшем ещё отсутствует. Read-only/executable protection, MMIO и неизвестные flags останавливаются. Отображение рассчитано на подтверждённые 4-КиБ host pages Y700. При повторном выделении общий буфер обнуляется; освобождение снимает все три view, backing store живёт до выхода процесса.

Игра запросила 4800 байт, protection 00000404, min=0/max=FFFFFFFF, alignment=32. Выделено 8192 байта, возвращён FFBFD000. Затем она дошла до VdInitializeEngines с parameter=1DDD0000, callback=82818618, argument=0, PFP=820F4BF0 и ME=820F5070. Реальные ресурсы GPU пока не созданы.

## Проверки на Y700

108 проверок среды прошли в ARM64-процессе, включая прежние проверки потоков и новые: границы 48-байтовой структуры видеорежима, порядок байтов/float, reserved; выравнивание и размер physical allocation; видимость записей между тремя алиасами; перевод адреса и query size; отказ диапазона, не вмещающего размер; освобождение и повторное выделение с нулевыми байтами; 64-КиБ страница. 16-МиБ вариант реализован по тому же правилу, но в этом прогоне отдельно не проверялся. Журнал device-physical-self-test.log. После проверки добавлен только диагностический обработчик параметров GPU, затем повторён игровой запуск (device-video-final.log).

## Фактический GPU

Отдельный небольшой NDK/Vulkan-процесс (vulkan_capabilities.cpp) опросил текущий драйвер устройства, не меняя его:

- Adreno (TM) 830, Vulkan 1.3.284;
- independentBlend, fragmentStoresAndAtomics, vertexPipelineStoresAndAtomics, geometryShader, fillModeNonSolid и tessellationShader доступны;
- VkDevice с пятью базовыми feature-битами отбора RexGlue действительно создан и уничтожен;
- sampled attachment support подтверждён для D24_UNORM_S8_UINT, R16G16_SNORM, R16G16B16A16_SNORM, R16G16_SFLOAT, R16G16B16A16_SFLOAT, R16G16B16A16_UNORM;
- VK_EXT_fragment_shader_interlock не объявлен драйвером. В исследуемом бэкенде есть отдельный путь host render targets, поэтому отсутствие расширения само по себе не доказывает невозможность порта. Полная совместимость этого пути с игрой ещё не проверена.

Это базовая проверка создания Vulkan device и части требований, не запуск RexGlue GPU и не полный тест всех форматов, MSAA, texture formats, memory limits, SPIR-V или shaders. Журнал device-vulkan-capabilities.log. Сборка capability-процесса: NDK clang++ --target=aarch64-linux-android35, C++17, отдельная компиляция .o, затем линковка -static-libstdc++ -lvulkan.

## Следующий этап

Нужно подключить реальный Xenos command processor, регистры/ring buffer, память для GPU, перевод шейдеров в SPIR-V и Vulkan render targets. Для картинки затем потребуется Android surface/swapchain. Локальные Vulkan backend sources RexGlue содержат часть Android presenter, но runtime пока не связан с ними, а несовместимость PPCContext ранее подтверждена. VdInitializeEngines в upstream исследовательском коде просто возвращает 1; такой возврат не переносился.

APK не изменялся. Производный игровой бинарник и приватные данные остаются в work и на личном планшете; в проект сохранены только собственные исходники и журналы проверок. Процессы проверок завершены, host/guest GPU-потоки в фоне не оставлены.
