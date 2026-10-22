# Начальный часовой пояс гостевой консоли

2026-10-22. ExGetXConfigSetting USER category3, settings1..7.

Исходный гостевой caller82A099xx последовательно запрашивает смещение, два4-байтовых ASCII имени, две4-байтовые packed даты и два дополнительных смещения. Формат сопоставлен с pinned RexGlue xboxkrnl_xconfig.cpp и фактическим чтением полей в гостевом коде. Контекст RexGlue не используется.

Начальная конфигурация runtime явно выбрана как фиксированный UTC: Bias/StandardBias/DaylightBias=0, оба имени UTC\0, обе transition date=0. Нулевой месяц выключает сезонный переход. Это настройка гостевой консоли, не извлечение настроек Xbox и не автоматическое определение часового пояса Android. KeQuerySystemTime продолжает выдавать реальное UTC FILETIME без поправки timezone.

Сохранены существующие проверки output/required_size, big-endian данные, BUFFER_TOO_SMALL и size-query. На ошибке required_size0 соответствует существующему ABI wrapper. Language/video defaults сохранены.

Все506 self-tests прошли на Lenovo Y700: work/startup/timezone-self-test.log, exit0. Проверены значения всех семи полей, BE required_size и соседние bytes, короткий буфер без изменения output, size-only query и NULL output с ненулевой ёмкостью. Игровой прогон work/startup/timezone-game.log проверяет дальнейшую загрузку.

В timezone-game.log игра успешно выполнила все settings1..7 и остановилась на setting12, caller82A09BA4 (exit42). Параметр подтверждён гостевым li r4,12; pinned SDK определяет его как USER_RETAIL_FLAGS. Добавлен явный default0x40 по тому же SDK, не извлечённый из консоли. Проверены обычный запрос, size-query и короткий буфер;509 self-tests прошли, work/startup/retail-timezone-self-test.log. Следующий игровой лог — work/startup/retail-timezone-game.log.

В retail-timezone-game.log setting12 пройден; следующая остановка — RtlTimeToTimeFields (caller829FB368). Добавлены оба календарных преобразования в time_fields.h: григорианский календарь, epoch1601, воскресенье weekday0, миллисекундная точность TIME_FIELDS, UTC без host timezone. Поддерживается неотрицательный signed64 FILETIME. Decode вне этого диапазона останавливает probe явно; encode недопустимой даты возвращаетfalse, не меняя выходные8байт. Weekday входного encode игнорируется как в SDK.

На Y700 прошли528 self-tests (work/startup/time-fields-self-test.log). Независимые контрольные FILETIME для1970-01-01,2000-02-29 и2024-02-29 рассчитаны Python datetime. Проверены epoch1601/Monday, leap-day2000/Tuesday, leap-day2024/Thursday, миллисекунды789/999, void ABI, round-trip, запрет1900-02-29, hour24, month258 и milliseconds1000, потеря субмиллисекундной части и отсутствие записи при ошибке. Следующий игровой лог — work/startup/time-fields-game.log.

time-fields-game.log подтвердил прохождение RtlTimeToTimeFields: несколько преобразований реальной UTC-даты2026-10-22, weekday4. Следующая остановка — data import XboxKrnlVersion, ordinal344, IAT820007E4, caller8276B694 (exit42). Обратное преобразование пока подтверждено self-tests, а не отдельным игровым вызовом. Подробности следующего этапа: [версия гостевого ядра](KERNEL-VERSION.md).
