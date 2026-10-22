# Vertex color trace — 22 октября 2026

Диагностический флаг `ME_DIAGNOSTIC_VERTEX_DATA=1` в prepare_draw_trace.py снимает максимум три снимка для VS 302B2B9F18E0017E. По умолчанию выключен. Используются фактические `vertex_bindings()`, максимум 48 слов на binding, только диапазоны, прошедшие IsPhysicalRangeValid. Запись только в /data/local/tmp/me_vertex_data_N.txt, без изменения guest memory или draw.

## Результат

Фактические fetch constants — 95 (position), 94 (packed attributes), 93 (lighting), endian=2. Для первого снимка: position 0x1406A800, attributes 0x14080000. Это адреса данного запуска, не постоянные настройки.

CPU-декодирование первых 12 слов position даёт конечные координаты. В слоте 94 слово offset=2 при stride=4 и unsigned normalized FMT_8_8_8_8 / swizzle zyxw даёт для первых 12 образцов два цвета:

- (0.458824, 0.607843, 0.725490, 0.458824)
- (0.631373, 0.772549, 0.882353, 0.631373)

Это ненулевые исходные цвета. Дамп содержит первые слова буферов, НЕ последовательность вершин по index buffer. Нельзя распространять результат на всю сцену либо считать подтверждением GPU-выхода шейдера.

Исходный FMT8888 путь RexGlue выполняет endian swap по fetch constant, извлечение компонентов и для unsigned normalized умножение на 1/255. Конкретный GPU результат ещё не считан.

## Проверки и ограничения

- Первая версия диагностики ошибочно читала слоты 0–2 напрямую, имеющие другие типы/неотображённые диапазоны: execution_fault signal11, exit43. Исправлено проверкой IsPhysicalRangeValid и выбором bindings из шейдера. Это ошибка диагностики, не доказательство ошибки игры или рендерера.
- Финальный игровой запуск: work/startup/vertex-bindings-game.log, остановка внешним timeout35, probe_exit=124, без нового execution_fault.
- Финальные встроенные self-tests: work/startup/vertex-bindings-self-test.log, 641 проверка, probe_exit=0.
- Дамп work/startup/vertex-bindings-0.txt; CPU результат work/startup/vertex-colors-analysis.json.
- Чёрные силуэты этим этапом не исправлены. Следующее: проверить фактическое чтение shared memory GPU и выход o6 / PS, затем blending, render target и resolve. Не заменять цвета константой как исправление.
