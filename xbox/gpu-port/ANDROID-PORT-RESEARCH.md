# Сопоставление открытых Android-портов — 22 октября 2026

Проверены документация и выбранные исходники четырёх репозиториев. Снимки исходников и manifest с commit/URL сохранены в `work/references/android-port-research/`. Удалённые скрипты не запускались. Исправление артефактов Mirror’s Edge пока не найдено и не заявляется.

| Проект | Проверенный материал | Применимость |
| --- | --- | --- |
| [MW2 Android](https://github.com/OpenPort-Labs/mw2-android/blob/1c62070cca647a439731b3ca4a23d454c690e2ef/runtime/gpu/vulkan/texture_cache.cpp) | DecodeInSoftware и Restage: повторное декодирование, проверка VkFormat и размера payload, явная ошибка декодера | Полезная проверка соответствия представления данных Vulkan-изображению. У нас другой кеш и compute upload, прямое копирование не подходит. |
| [MW2 translator](https://github.com/OpenPort-Labs/mw2-android/blob/1c62070cca647a439731b3ca4a23d454c690e2ef/runtime/gpu/vulkan/shader_translator.cpp) | ReadGuestDword, DecodeVertexFormat, NClamp | Подсказки проверить endian и signed/normalized packed vertex colors. NClamp уже присутствует в нашем RexGlue; это не новая починка. |
| [Liberty](https://github.com/vaduur/LibertyRecompAndroid/blob/bc5e88607636a96f7c4ab6244b6f96e215fc384d/glue/rexglue-sdk-main/src/graphics/gta4_native/depth_handoff_util.h) | Отдельный gta4_native depth handoff: capabilities, layout-specific stages/access, fallback причины | Это специальный GTA4 путь. Нельзя переносить его stencil/reference либо depth transport в общий Xenos-путь Mirror’s Edge без анализа. |
| [Skate 3](https://github.com/andrewnakas/skate3-android/blob/af641a6b0abeacc482b27a00a2a0c0b2248a4858/HANDOFF-0.1.24.md) | Измерения теплового ограничения, лимит кадров и бюджет GPU | Пригодится для производительности после корректного кадра. Не объясняет нынешние силуэты и не является готовым shader fix. |
| [Unleashed](https://github.com/SansNope/UnleashedRecomp-Android/tree/f65c40ba2788ebd7318361670bcbb82cfdef6ae8) | Changelog и vertex_element_cache.h | Последний содержит таблицы конкретной игры, а не универсальную правку декодера. Не переносить таблицы в Mirror’s Edge. |

## Сопоставление с нашими измерениями

- Переключение native BC → RGBA уже проверено на устройстве: силуэты сохраняются (см. TEXTURE-ARTIFACTS.md). Поэтому найденный MW2 обход BC не включён по умолчанию.
- В RexGlue `spirv_translator.cpp`, `spirv_translator_alu.cpp`, `spirv_translator_rb.cpp` уже используется GLSLstd450NClamp. Нужен анализ конкретной инструкции, а не массовая замена clamp.
- У выбранного фонового PS нет texture fetch. Цвет приходит через r6 от VS, поэтому непосредственная BC-декодировка не объясняет цвет именно этого прохода. Это не исключает ошибки других проходов.
- Совет из MW2 mobile pipeline про depth STORE_OP_DONT_CARE нельзя применять ко всем нашим attachments: содержимое Xbox EDRAM может потребоваться следующим проходам/resolve. Сначала установить lifetime конкретного ресурса.

## Следующие проверки по приоритету

- [ ] Для VS 302B2B9F18E0017E снять ограниченный vertex fetch trace: адрес/stride/endian/format/num_format/signed и исходные packed color слова. Сопоставить CPU-декодирование и формулы SPIR-V, не менять цвет на глаз.
- [ ] Проверить кеш текстур: один TextureKey должен выбирать совместимые host format, load shader и image view; отдельно повторные загрузки/инвалидация. Не переносить MW2 Restage буквально.
- [ ] Если данные VS корректны, проверить blend/target format, resolve и lifetime depth/color между проходами.
- [ ] Затем A/B системный Vulkan и совместимый пользовательский драйвер через app-local loader; это отдельная гипотеза, а не обещанная починка.

В этом этапе не выполнялась новая сборка и не менялся рендерер на планшете. Исторические 641 self-test относятся к предыдущему выполнению, не к отдельному новому запуску.
