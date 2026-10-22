# MSAA-образцы исходного EDRAM — 22 октября 2026

Расширен prepare_edram_probe.py/diagnostic_edram_probe.h: до4lanes на точку (2точки,8words), word_count и MSAA metadata. Условие source.msaa_samples<=2 покрывает1X/2X/4X, но в этом запуске наблюдались1X и2X;4X не считается проверенным.

Адресация сверена с resolve_full_64bpp_cs.h:sampleX=(lane>>1)&1,sampleY=lane&1,pixelX масштабируется2 при4X,pixelY масштабируется2 при2X/4X. Затем80×16samples/tile,pitch_tiles,mod2048. Копируются отдельные packed32 слова доconversion/clear, mapping послеfence. Остальные ограничения источника7e3/scale1/фиксированной области остаются. ME_DIAGNOSTIC_EDRAM_SOURCE по-прежнему отключает direct resolve только для диагностики; это alternate fallback, не доказанный эквивалент production path.

## Подтверждённый2X-проход

edram-msaa-game.log:45с/timeout124. Source_color00030400,msaa2,copy_sample_select4=k01.

Первое наблюдение sample3/frame175: обе точки нулевые; это могло быть первоначальное заполнение и не объявляется ошибкой.

Следующее sample8/frame176:

| point | lane | packed RGB decode | alpha |
| --- | --- | --- | --- |
| target128,468 |0|0,0,0|0|
| target128,468 |1|0,0,0|0|
| control640,180 |0|7.21875,8.3125,9.5|1|
| control640,180 |1|7.21875,8.3125,9.5|1|

Оба sample target уже чёрные после host→EDRAM dump доresolve; контроль оба одинаковые. Это не похоже на ошибку только одного из двух samples или на появление нуля из-за усреднения именно этой пары. Сам host image доdump ещё не считан, dump ещё не исключён как причина. Позже/в других точках значения могут отличаться. Анализ:work/startup/edram-msaa-analysis.json,analyze-edram-msaa.py.

## Гипотеза occlusion query

prepare_occlusion_probe.py добавляет ME_DIAGNOSTIC_OCCLUSION_VISIBLE=1: в WriteGuestOcclusionResult только нулевой host result заменяется1000 и логируется (до32 результатов). Явное not_a_fix=true. По умолчанию отключено. Обычный base fallback SDK уже использует fake_sample_count1000.

occlusion-visible-game.log:40с/timeout124; маркеров external_occlusion_visibility_probe нет. Это значит, что экспериментальный участок не наблюдался в этом запуске, а не что все виды occlusion culling исключены. occlusion-visible-frame.png проверен: силуэты сохраняются. Не принят как workaround.

Следующее: считать соответствующий MSAA host render target до DumpRenderTargets и сравнить packed2X samples послеdump; отдельно подтвердить owner/image/subresource для соответствующих EDRAM tiles. Нельзя проверять эту гипотезу чтением уже очищенного render target.

Финальная сборка без diagnostic env641 self-test,exit0:msaa-probe-self-test.log. Все пробы завершены, Activity не открывалась; артефакт остаётся.
