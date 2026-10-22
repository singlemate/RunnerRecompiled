# Импорт данных XboxKrnlVersion

2026-10-22. Следующая граница после календарных преобразований.

Игра читает ordinal344 (0x158), IAT820007E4 как указатель на8-байтовую структуру: BE16 Major, Minor, Build, QFE. Формат сопоставлен с pinned RexGlue xboxkrnl_module.cpp и фактическими lhz в гостевой829F9678. Достигнутый caller проверяет старую ветку2.0.build<5611.

В закреплённом SDK используется профиль2.FFFF.FFFF с approval-bit; этот завышенный профиль не скопирован. Заголовок пользовательского XEX содержит xboxkrnl.exe target2.0.7645.0 и minimum2.0.6717.0. Новый bounded parser MeXexHeaders::import_version читает реальные записи, проверяя размер таблицы, строки/индексы и размер library records.

В качестве явной политики совместимости loader выбирает XEX target version. Структура размещена60003100 и привязана к IAT. Approval flags не заявляются. Это guest runtime metadata, не реальная версия Android/консоли и не подтверждение реализации всех возможностей kernel7645. Остальные неподдержанные сервисы сохраняют явные остановки. Невалидный заголовок/неподдержанный профиль останавливают loader, а не дают случайную версию.

На Y700 прошли535 self-tests (work/startup/kernel-version-self-test.log, exit0). Проверены реальные target/minimum из заголовка пользователя, BE layout, привязка IAT, отсутствующее имя библиотеки и отказ на повреждённом string-table size. Следующий игровой лог — work/startup/kernel-version-game.log.

kernel-version-game.log прошёл прежний data import и дошёл до XamInputSetState (caller829F9728, exit42). Для этого вызова добавлена синхронная absent-provider ветка:1167, NULL vibration pointer160, valid4-byte packet не изменяется. Это соответствует отсутствию Android→game provider именно в native probe, а не утверждению о физически отключённом Bluetooth-контроллере. Фактическая вибрация и игровой ввод ещё не реализованы.

540 self-tests прошли на Y700 (work/startup/vibration-self-test.log). Следующий игровой прогон — work/startup/vibration-game.log.

vibration-game.log подтвердил обработку user0→1167 и продолжение игры. Следующая явная остановка — NetDll_XNetGetEthernetLinkStatus, caller8275E0D0, exit42. Этот сервис пока не реализован; loopback XNet backend не означает внешний Ethernet/Xbox Live. Все перечисленные прогоны завершены.
