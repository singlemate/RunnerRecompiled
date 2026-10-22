# Packed EDRAM перед HDR resolve — 22 октября 2026

prepare_edram_probe.py устанавливает общий diagnostic_edram_probe.h в private SDK и компилирует render_target_cache.cpp / command_processor.cpp. Режим ME_DIAGNOSTIC_EDRAM_SOURCE=1 записывает staging-copy двух packed32 слов после DumpRenderTargets и ДО resolve conversion/clear. Mapping/fence/invalidate выполняются в IssueCopy после завершения Resolve. Нет guest writes. До12образцов, GPU thread only.

Условия: sourceformat3(7e3),MSAA1X,32bpp,scale1,copy_dest_base128E0000,width1280,height>=512,без packed source offsets. Координаты128,468 и640,180, EDRAM80×16 tile addressing с wrap2048tiles. Не охватываетMSAA2X/4X,format12 и произвольные смещения. Данные нельзя переносить на исключённые конфигурации.

## Два запуска

1. edram-source-game.log,45с/timeout124: стандартный direct resolve, образцов EDRAM нет. Перехват находился вне ветки direct_resolved; отсутствие образцов не означает отсутствие EDRAM-stage. Позднее установлено, что TryResolveCopyDirectly сам вызывает DumpRenderTargets; по этому запуску нет вывода об исходных значениях.
2. edram-fallback-game.log,45с/timeout124: при этом env дополнительно отключён direct_host_resolve, чтобы использовать DumpRenderTargets→EDRAM→copy. Это изменение пути, а не нейтральное наблюдение обычного direct resolve. Включать только диагностически; без env оригинальный direct setting сохраняется.

Получены12packed образцов. Пример sample7/frame177/source_color000302D0:

- target offset005C99C0,word00000000 →RGB0,alpha0;
- control offset0046A500,word318C16E7 →RGB(7.21875,8.3125,9.5),alpha0;
- ожидаемое масштабирование−3 даёт control(0.90234375,1.0390625,1.1875), совпадающий с ранее считанным HDR-контрольным цветом. Это полезная проверка адресации и7e3 декодирования; samples взяты в отдельных запусках, не строгая побитовая проверка всей конверсии.

Следовательно, в данном поддержанном1Xпроходе zeroRGB уже присутствует после host render target dump до conversion. Не доказано, что исходный host image до dump чёрный: сам dump ещё входит в исследуемую цепочку. Не доказана корректность MSAA split-path. Другие образцы содержат ненулевой target, поэтому нельзя говорить, что точка всегда чёрная.

Анализ:work/startup/edram-source-analysis.json, использована формула Float7e3To32(10bit) плюс2bitUNORMalpha. Source samples и destination samples не во всех случаях совпадают; alternate fallback требует отдельной валидации. Нельзя объявлять этот режим исправлением либо точным эквивалентом direct path.

edram-fallback-frame.png визуально проверен: тёмные силуэты остаются. Поэтому fallback не принят как обход артефакта.

Следующее: сравнить host render target до dump и packed EDRAM, отдельно охватить фактический MSAA split-source, а не обобщать1Xобразцы на все проходы. Диагностические GPU-copies должны оставаться до clear, CPU mapping — после fence.

Финальные self-tests без env641,exit0:edram-probe-self-test.log. Дополнительный обычный запуск без env:edram-probe-default-game.log. Просмотр не открывался. Исправление пока не найдено.
