# Прямоугольники resolve — 22 октября 2026

prepare_resolve_rect.py добавляет bounded ME_DIAGNOSTIC_RESOLVE_RECT=1 в GetResolveInfo, до уменьшения координат относительно EDRAM-тайлов. Логирует до64 HDR операций формата32/pitch1280: x0/y0/x1/y1, исходную и adjusted destination base, memory extent, packed destination offsets и source color/depth. Наблюдение только метаданных, не меняет данные или отрисовку. Компилируется util_draw.cpp.o.

В одном фоновом запуске включены rectangle trace и чтение target/control после resolve. resolve-rect-game.log:40с,timeout124. Сохранены64 записи:work/startup/resolve-rect-analysis.json. Получены:

| raw base | rect x0,y0,x1,y1 | extent length | source color |
| --- | --- | --- | --- |
|128E0000|0,0,1280,720|7536640|000302D0 и000C02D0|
|128E0000|0,0,32,32|8192|000302D0|
|128E0000|0,0,1280,512|5242880|00030400|
|12DE0000|0,0,1280,208|2293760|00030400|

Для всех этих операций adjusted_base=raw_base и packed destination offsets0. При RGBA16F8байт/texel,pitch1280 разность12DE0000−128E0000=5242880 соответствует512строкам. Первый split охватывает строки0..511, второй локальный0..207 попадает в глобальные512..719. Обе точки target(128,468)/control(640,180) находятся внутри первого прямоугольника. Подозрение на простое неверное смещение второй части этим trace не подтверждено.

Ограничение: правильные координаты и readback результата не доказывают правильность source render target/EDRAM dump/resolve compute. Нужен следующий контроль: прочитать соответствующий packed7e3 цвет из EDRAM после DumpRenderTargets, ДО conversion/copy/clear, и сопоставить с RGBA16F после resolve. При этом GPU staging copy надо записать до clear, а mapping делать только после fence; не читать уже очищенный EDRAM как источник кадра.

Финальная сборка без env:641 self-test,probe_exit0,resolve-rect-self-test.log. Фоновые пробы завершены; Activity просмотра не открывалась. Артефакт пока не исправлен, никакие материалы не отключены по умолчанию.
