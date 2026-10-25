# Позиции сэмплов Xenos MSAA (перенос из Xenia Canary)

2026-10-22. Повод: отчёт совместимости Xenia Canary для 45410850 (xenia-canary/game-compatibility#517) рекомендует `window_offset_in_edram = false` и `xenos_sample_positions = true` против «MSAA tiling issue (black triangles)».

## Что перенесено

Патч `rexglue-xenos-sample-positions.patch` относительно локального состояния RexGlue (pinned c94f5eb + наши Android/диагностические изменения, локальный снимок 08d6f67 в ветке me-android-state). На чистый c94f5eb применять после `rexglue-android-runtime.patch` и диагностических prepare_*.py.

- **6579cc2a4c** полностью: `ViewportInfo::xy_offset` знаковый, начало viewport может быть отрицательным (нижний тайл predicated tiling рисуется с отрицательным смещением).
- **cc4981a465**, Vulkan-часть: таблица позиций Xenos и `GetHostSampleXenosPositions`; cvar `xenos_sample_positions` (по умолчанию false, как в Xenia); `VK_EXT_sample_locations` в устройстве; позиции в `BeginRenderPass`, в барьерах depth (`TransitionRenderTarget`, 10 мест, включая нашу диагностическую пробу), в графических pipeline и pipeline переносов; флаг `SAMPLE_LOCATIONS_COMPATIBLE_DEPTH` у depth-изображений.
- `gpu_bridge.cpp` включает `xenos_sample_positions` для этой игры; `ME_XENOS_SAMPLE_POSITIONS=0` выключает, `ME_VULKAN_RENDER_PASSES=1` выключает dynamic rendering (диагностика).

## Что не перенесено и почему

- **e1f4a0d009** (смещение тайла в базу EDRAM). При рекомендованном для этой игры `window_offset_in_edram = false` все его новые ветки выключены, а прежнее поведение (смещение через viewport/scissor) у RexGlue уже есть. Полный перенос — около 900 строк в render target cache без эффекта в этом режиме.
- D3D12, DXBC, HLSL — не собираются для Android.
- FSI-часть `spirv_shader_translator_rb` — на Adreno 830 нет `VK_EXT_fragment_shader_interlock`.
- Поправка viewport для масштабирования разрешения из cc4981a465 — работает только при scale > 1, у нас 1×.

## Отличия от Xenia (допущения)

- В RexGlue рисование без attachments и с rasterizer discard идёт на 1×. Позиции подключаются к pipeline только при совпадении числа сэмплов с числом позиций, иначе Vulkan-состояние было бы недопустимым.
- Путь dynamic rendering (по умолчанию в RexGlue) в Xenia отсутствует: там нет автоматических переходов layout, позиции передаются через pipeline и барьеры.
- Корректность на Adreno подтверждена только отсутствием ошибок и новых артефактов в кадре; validation layers не запускались.

## Проверка на Y700

- Сборка: 35+6+8+31 GPU-объектов пересобраны (изменены общие заголовки), runtime собран.
- Self-tests: 718, probe_exit0 (`work/startup/msaa-self-test.log`). GPU MSAA-путь self-tests не покрывают.
- Лог игры: `requested=true, extension=true, 2x=2, 4x=4` — позиции действительно установлены.
- Кадры в одинаковые моменты (35 и 50 с), `work/startup/msaa-{before,after,renderpass}-*.png`, доля тёмных пикселей в полосе горизонта:

| Кадр 50 с | над швом (400–511) | под швом (512–623) |
|---|---|---|
| до | 3,6 % | 0 % |
| после (dynamic rendering) | 4,3 % | 0 % |
| после (render pass) | 4,1 % | 0 % |

**Результат: силуэты не исчезли, гипотеза не подтвердилась.** Новых артефактов и ошибок нет. Рекомендация Xenia описывает чёрные треугольники на моделях персонажей, а не обязательно наши силуэты горизонта.

## Новое наблюдение

Силуэты обрываются ровно на строке 512 — границе тайлов predicated tiling (1280×512 + 1280×208). Тёмные только в верхнем тайле. Следующая проверка — как рисуется и восстанавливается depth верхнего тайла относительно полного depth prepass (тема e1f4a0d009, но для верхнего тайла), а не позиции сэмплов.
