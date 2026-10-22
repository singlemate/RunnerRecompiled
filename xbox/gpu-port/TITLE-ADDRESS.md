# Offline-состояние адреса игры

2026-10-22. NetDll_XNetGetTitleXnAddr(caller1,XNADDR*).

ABI,36-байтовый layout XNADDR и константы сопоставлены с pinned RexGlue xam_net.cpp. Структура содержит IP, online IP/port, MAC6байт и online identity20байт. Текущий собственный backend имеет только локальный loopback socket; title-address acquisition ещё не реализован.

Выбрана явная диагностическая ветка XNET_GET_XNADDR_NONE1 с обнулением36байт output. PENDING0 не используется: асинхронного получения адреса нет. STATIC/DHCP/ONLINE не заявляются. Из SDK не скопированы статический loopback XNADDR и MACизбайтCC. Ответ не является утверждением о реальных сетевых интерфейсах Android.

Невалидный output/неизвестный caller останавливают probe. Остальные аргументы и socket/port backend не меняются. Полноценный XNet, LAN и Xbox Live не реализованы.

На Y700 прошли560 self-tests (work/startup/title-address-self-test.log, exit0): статус NONE, сохранность r4/r5, весь нулевой output, guard words с двух сторон и сохранность socket/port. Проверена завершённая новая сборка145303128байт; первый случайно запущенный старый бинарник был остановлен и не засчитан. Новый игровой лог — work/startup/title-address-game.log.

title-address-game.log подтвердил обработку NONE и продолжение до запроса D:\TdGame\CookedXenon\TdMainMenu_Images.xxx (exit42: ещё не staged). Пакет229376байт извлечён из исходного SVOD и передан в приватный read-only root устройства. Следующий лог work/startup/menu-images-game.log проверяет чтение пакета и следующую границу. Запрос/подготовка изображений не подтверждают вывод меню на экран.

menu-images-game.log подтвердил полное чтение пакета: последний повторный проход offset0/read131072 +offset131072/read98304 =229376. Следующий SIGTRAP5 в sub823841A8 сопоставлен с trap stub для frsqrte82384344, а не с произвольной игровой assert. Host offset66FBB88 символизирован до замены бинарника. Подробности: [третий frsqrte меню](MENU-FRSQRTE.md).
