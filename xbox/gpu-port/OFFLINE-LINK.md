# Состояние гостевого сетевого подключения

2026-10-22. NetDll_XNetGetEthernetLinkStatus, достигнутый caller1.

ABI подтверждён по pinned RexGlue xam_net.cpp: возвращаемая маска без output-буфера. Текущий собственный сетевой backend создаёт только UDP loopback127.0.0.1. Поэтому guest status0 означает отсутствие внешнего link и не устанавливает speed/duplex flags. Наличие Wi-Fi или Bluetooth на Android не используется как доказательство гостевого Ethernet/Xbox Live.

Вызов не создаёт/закрывает сокеты, не отправляет пакеты, не меняет last-error и другие аргументы. Неизвестный caller вне исследованного режима останавливает probe явно. Новая реализация не расширяет WSAStartup или остальные сетевые сервисы автоматически.

На Y700 прошли542 self-tests, work/startup/ethernet-self-test.log, exit0. Дополнительно проверены flags0, сохранность r4, сокета/порта backend и KTHREAD last-error. Игровая проверка — work/startup/ethernet-game.log.

ethernet-game.log подтвердил ответ flags0 и продолжение игры до InterlockedPopEntrySList (caller82B8CB3C, thread610E1000, exit42). Подробности следующего этапа: [атомарные списки](SLIST.md).
