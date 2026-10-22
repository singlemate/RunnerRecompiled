# Создание и освобождение XMA-контекстов

2026-10-22. XMACreateContext/XMAReleaseContext.

API и предел320контекстов сопоставлены с pinned RexGlue xboxkrnl_audio_xma.cpp/xma_decoder.cpp. Реализованы только владение слотами и64-байтовые guest descriptors. Никакой codec/decoder completion этим этапом не заявляется.

Один ensure_xma_context_memory обслуживает и MMIO ContextArrayAddress, и API создания. Поэтому обе стороны видят один физический массив320×64байта. Create выделяет свободный слот, обнуляет64байта и пишет BE32 указатель через A-алиас. Исчерпание даёт output0/STATUS_NO_MEMORY C0000017. Release проверяет физическое смещение,64-байтовое выравнивание и владение, обнуляет слот и возвращает его в пул. Невалидное или повторное API release останавливает probe, вместо фиктивного успеха.

Владение хранится в отдельном MeXmaContextPool, включаемом только loader TU. Общий MeXmaRegisters layout в forced guest header сохранён, ABI существующих guest TUs не менялся. Доступ к пулу выполняется под execution gate. Декодирование, enable/busy/drain/release активного декодера ещё не реализованы.

На Y700 прошли570 self-tests (work/startup/xma-context-self-test.log, exit0):320уникальных адресов в реальном массиве, нулевые descriptor bytes, исчерпание321-го выделения, release/reuse с очисткой данных, отказ неверного смещения/повторного освобождения в модели, возврат всех слотов и совпадение physical base с MMIO. Тест не заявляет наличие320работающих декодеров.

Игровой лог — work/startup/xma-context-game.log. Последний завершённый бинарник145305432байта. Воспроизведение XMA и непрерывный вывод PCM остаются отдельными незавершёнными задачами.

## Игровое создание и команда Clear

xma-context-game.log подтвердил реальное создание BF4AB000, совпадающее с MMIO physical base1F4AB000. Следующая остановка — write7FEA1A80/value01000000, exit42. register_table.inc определяет index06A0 (offset1A80) как Context0Clear, а не Lock (Lock начинается0690).

Добавлена обработка10Clear banks по7FEA1A80..7FEA1AA4 с little-endian device mask. Она допускается только для принадлежащих пулу контекстов; decoder ещё не запускался и контексты idle. Все выделенные биты проверяются до мутаций. По XmaContext::ClearLocked очищаются input0/input1/output valid, output read/write offsets; input read offset становится32(kBitsPerPacketHeader). Остальные поля descriptor сохраняются, владение не освобождается. Готовность/завершение декодирования не синтезируются.

575 self-tests прошли на Y700 (work/startup/xma-clear-self-test.log, exit0), включая точные BE masks для первого/последнего слота0/319, сохранение остальных слов и соседних контекстов. Игровая проверка — work/startup/xma-clear-game.log. Новый бинарник145306456байт.

xma-clear-game.log подтвердил игровое создание BF4AB000 и Clear bank0/mask1. Следующая остановка — ещё не staged Textures.tfc, exit42. Textures.xxx отсутствует в исходном inventory и корректно не подменяется. Textures.tfc520159232байт извлечён из пользовательского SVOD; перед передачей свободно113447120KiB на устройстве. Факт получения файла не равен загрузке текстур в Vulkan/выводу изображения.

Файл передан полностью; SHA256 на ПК и планшете совпал:4b36052079fef878193668127f8eceac084c2efa1c126228526657f7bdb5b72c. Следующий игровой лог — work/startup/textures-game.log.
