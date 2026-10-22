# Выборочный GPU-readback глубины — 22 октября 2026

prepare_depth_readback.py добавляет ME_DIAGNOSTIC_DEPTH_READBACK=1 для PS8F5851C1A202047B. В конце draw считываются5 точек, максимум3 кадра. Используется фактический fetch0, GetTiledOffset2D(...,bytes_per_block_log2=2), pitch из fetch. VkBufferCopy→staging, transfer→host barrier, AwaitAllQueueOperationsCompletion, mapping и invalidate non-coherent; гостевая память не изменяется. По умолчанию отключено, диагностическая синхронизация влияет на скорость.

## Формат и результаты

Гостевой формат23 — k_24_8_FLOAT, не k_32_FLOAT (36). Vulkan cache конвертирует его в R32_SFLOAT (load shader DepthFloat). Первый байтовый порядок fetch endian2=8-in-32, depth в старших24битах, stencil в младших8. CPU-декодирование использует формулу XenosFloat20e4To32; встроенная SPIR-V disassembly texture_load_depth_float_cs.h подтверждает shift8, mantissa20, exponent+bias112, mantissa<<3.

В depth-gpu-readback-game.log получены15 точек, кадры188–190:1280×720,pitch1280,tiled1,endian2. Пять координат:(128,468),(640,180),(640,576),(1152,468),(640,360). Это выбранные экранные области, не подтверждённая попиксельная привязка каждого артефакта; камера меняется.

Float20e4 значения в выборке0.002989618..0.010506988, все конечны и ненулевые. Формула игрового PS1/(z*c2.z-c2.w) даёт1050.8046..5021.0316, без NaN/Inf. Значения меняются по кадрам. Сырые слова и расчёты:work/startup/depth-gpu-analysis.json. Это исключает пустой буфер именно в выбранных точках, но не доказывает корректность всей depth-карты/выборки/фильтрации.

Фоновый запуск40с завершился timeout124; просмотра не было.

## Контроль RGB этого прохода

В prepare_shader_probe.py добавлен ME_DIAGNOSTIC_DEPTH_COLOR_SAMPLE=1, только PS8F5851C1A202047B: окончательный RGB заменяется на r6.xyz, последний раз записанный tfetch1 offset(0,3), alpha исходного output сохранена. Это соседний HDR sample, не точный passthrough центра; намеренно меняет расчёт и яркость. По умолчанию выключен, не исправление.

Фоновый depth-color-sample-game.log timeout40/exit124. depth-color-sample.png визуально проверен: сильный пересвет, тёмные контуры сохраняются. Это не позволяет объявить единственной причиной depth-weighting RGB; при сохранённой alpha ещё не исключены alpha-ветвь и последующая обработка. Следующее — сравнить HDR до фильтра и альфа-канал соответствующих промежуточных текстур.

Финальная сборка без diagnostic env:641 self-test,probe_exit0 (depth-readback-self-test.log). Все пробы завершены; приложение просмотра не открывалось. Артефакты пока не исправлены.
