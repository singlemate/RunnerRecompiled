# Производительность уровня: материалы и чек-лист проверок

2026-10-23. Собрано без планшета (подключение — в воскресенье). Цель: стабильные 30 кадров/с на уровне пролога (Xenia даёт 30, с патчами 60). Текущее: ~13,8 кадра/с с правильной картинкой; ~20 с `ME_GPU_PAGE_STATE=0`, но тогда чёрные объекты.

## Что уже известно по замерам

- Узкое место — хост-поток command processor GPU (RexGlue): игровой поток крутит `Sleep(0)` в ожидании потока рендера, тот опрашивает фенс GPU (страница 0x1FBF4000), CP отстаёт на 16–81 команду (HANDOFF, запись «Плато ~13,6»).
- GPU-стек был собран с -O0 → -O2 дал ~2×; код игры -O2 ещё ~+20 %.
- A/B: `clear_memory_page_state` off даёт +45 % (13,8 → 20,1), но у пользователя почернели вертолёт, красные двери и жёлтая сумка Фейт.
- Occlusion queries off не помогли (11,9).

## Внешние материалы

1. **Crackdown на RexGlue** — тот же SDK, тот же упор в CP.
   - `jack-kenney/Crackdown`, `sdk-patches/rexglue-0.10.0/` (копия: `work/references/crackdown-patches/`):
     - `type0-register-batching.patch` — TYPE0-пакеты через `WriteRegisterRangeFromRing` вместо `WriteRegister` на каждое слово; **61 → 92 кадра/с** в их сцене; цикл TYPE0 + запись регистров + поиск метаданных = 43 % сэмплов графического потока. **Перенесён** (см. ниже).
     - `single-range-allocation.patch` — `SharedMemory::RequestRanges` без `std::vector` для одного диапазона (на каждый draw).
     - `control-register-batching.patch`, `command-stream-reuse.patch`, `constant-packing.patch` — мелкие, без явного выигрыша у них.
   - `Unowa001/crackdown-recomp` PR #7 «Perf for 1.0» (страница сейчас 404, сведения из поиска): `clear_memory_page_state` off по умолчанию «как в Xenia Canary» — повторная загрузка 19–23 МБ/кадр → ~2 МБ; патч 0022 — lock-free проверки в запросах shared memory (88–94 % запросов); патч 0023 — пакетное копирование диапазонов регистров. CP был на 100 %.
2. **Xenia Canary**: `clear_memory_page_state` по умолчанию **off**; включают точечно для игр с артефактами (метка `requires_clear_memory_page_state_true`), т.к. дорого каждый кадр.
3. **Чёрные текстуры при off — известный класс ошибок**: fork PR `peerloomllc/xenia-canary#93` — GPU пометил страницы valid без watch, гость позже выделил и заполнил их, GPU не узнал → чёрные текстуры (Eternal Sonata). Исправление: инвалидация GPU-копий при **выделении** физической памяти (`PhysicalHeap::Alloc/AllocFixed/AllocRange`), не только при освобождении. Похожие: `Ecredes/xenia-canary#64` (XFile::Write мимо watch), `#58`, `#68`.
4. **UnleashedRecomp/XenosRecomp**: не эмулируют GPU Xenos — транслируют рендер игры в современный API (шейдеры заранее). Отсюда 60 кадров/с. Стратегическая альтернатива для нас, если CP-путь упрётся.
5. **Adreno/Turnip**: минимизировать число render pass и разрывы проходов (выгрузка/загрузка GMEM); проверять, где транслированный рендер рвёт проходы; новые сборки Mesa/Turnip заметно быстрее старых.
6. **Xenia docs**: синхронное чтение resolve посреди кадра — огромная потеря; у нас `readback_resolve=none`, `vulkan_readback_memexport=false`.

## Изменения, подготовленные без планшета (собраны, НЕ проверены на устройстве)

| Изменение | Где | Флаг |
|---|---|---|
| Лог `external_gpu_wait_begin` INFO → DEBUG (~800 строк/с в потоке CP) | RexGlue `src/graphics/command_processor.cpp` | всегда |
| TYPE0 batching (из Crackdown) | RexGlue `command_processor.cpp` `ExecutePacketType0` | всегда |
| `clear_memory_page_state` снова ON по умолчанию (правильная картинка) | `xbox/startup/gpu_bridge.cpp` | `ME_GPU_PAGE_STATE=0` выключает |
| Инвалидация GPU-копий при выделении/освобождении физпамяти (как PR #93) | `entry_probe.cpp` `map_physical`, `rex_gpu_memory.h` `InvalidateAllocatedRange` | `ME_GPU_ALLOC_INVALIDATE=1` |
| Переключатель occlusion queries | `gpu_bridge.cpp` | `ME_GPU_OCCLUSION_QUERIES=0` |

## Чек-лист на воскресенье (по порядку)

1. `work\startup\build-test.ps1` — выложить, self-tests (ожидается 765; если упали — смотреть строку в сообщении `expect`).
2. Базовый прогон на уровне (`run-game-sequence.ps1` / `ab-level.ps1`): эффект TYPE0 batching + DEBUG-лога (было 13,8).
3. **Ключевой тест чёрных объектов**: `ME_GPU_PAGE_STATE=0 ME_GPU_ALLOC_INVALIDATE=1`, пользователь играет до вертолёта/красных дверей/сумки. Если цвета правильные — сделать оба дефолтом (≈+45 %). Если нет — искать другие пути, где страницы становятся valid без watch (RexGlue `SharedMemory`: GPU-written/resolve/`MakeRangeValid`), и ставить на них watch.
4. Если всё ещё < 30: перенести `single-range-allocation.patch`; профиль CP на -O2 (доли vulkan.adreno.so / XXH3 / UpdateBindings / IssueDraw); посчитать разрывы render pass за кадр.
5. Падение lr 8249b888 (нулевой указатель, при подгрузке Edge_Pt2) — HANDOFF.
6. Стратегически: оценить объём трансляции рендера в стиле XenosRecomp.

## Источники

- https://github.com/jack-kenney/Crackdown (sdk-patches/rexglue-0.10.0)
- https://github.com/Unowa001/crackdown-recomp/pull/7
- https://github.com/xenia-canary/xenia-canary/wiki/Options
- https://github.com/xenia-project/game-compatibility/labels/requires_clear_memory_page_state_true
- https://github.com/peerloomllc/xenia-canary/pull/93
- https://github.com/Ecredes/xenia-canary/issues/64
- https://github.com/hedge-dev/XenosRecomp
- https://docs.qualcomm.com/bundle/publicresource/topics/80-78185-2/mobile_best_practices.html
- https://xenia.jp/updates/2021/04/27/leaving-no-pixel-behind-new-render-target-cache-3x3-resolution-scaling.html
