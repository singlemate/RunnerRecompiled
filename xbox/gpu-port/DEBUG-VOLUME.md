# Первый файловый запрос: отсутствующий debug volume

2026-10-21, Y700 test device. Декодированы X_OBJECT_ATTRIBUTES (root+0,name_ptr+4,attributes+8) и X_ANSI_STRING для NtQueryFullAttributesFile. Проверяются input descriptor, длина1..1024, доступность строки и56байт output. Не-ASCII/управляющие символы возвращают OBJECT_NAME_INVALID. В журнале backslash/quote экранируются как JSON.

Реальный первый путь: ShaderDumpxe:\CompareBackEnds, root ObDosDevices FFFFFFFD. В diagnostic окружении developer volume ShaderDumpxe не подключён. Только для этого имени тома (case-insensitive) возвращается STATUS_NO_SUCH_FILE C000000F; буфер атрибутов остаётся неизменным. По pinned SDK xboxkrnl_error.cpp этот статус переводится RtlNtStatusToDosError в ERROR_FILE_NOT_FOUND2. Другие тома/relative roots пока останавливаются file_query_mapping_not_implemented. Это НЕ полная реализация файловых атрибутов, чтения игровых данных, mount или сохранений.

После этого игра продолжила до KeEnableFpuExceptions, LR829F7824,r3=0. GPU31/31, стартовые PCM callbacks работают как ранее. Следующий шаг — семантика гостевых FPU exceptions и соответствие ARM FP state, без установки произвольных host signal modes. Затем продолжать собирать фактические игровые файловые обращения.

306 self-tests прошли:3новые проверки отсутствующего debug-path с неизменным выходом, mapping статуса и отказа управляющего символа. Реальный normal game/content path ещё не проверен.

Журналы device-debug-volume-self-test.log, device-debug-volume-explore.log. Исходные игровые файлы/данные пользователя не изменялись.
