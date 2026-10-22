# Большие пакеты, уведомления и отсутствие профиля

2026-10-21, Y700 Gen4 / test device.

## Загрузка ресурсов и таймер

Подтверждено чтение Tp.xxx (163840 байт). Следующий пакет TdGame.xxx (19070976 байт) перенесён из собственной Xbox-копии. Короткие20с прогоны прерывали его чтение примерно на4МБ; это не доказательство зависания.

Добавлен отдельный --gpu-explore-long-poll-yield: тот же экспериментальный runtime, но24 снимка через5с, предел120с вместо20с. ADB-внешний timeout125с. Обычный режим сохранил20с. В длинном запуске TdGame дочитан: последняя запись offset19038208/read32768. Далее прочитан Startup_int.xxx (2719744 байт). Ранее замеченные иные ранние остановки не объявлены исправленными этим изменением.

## Уведомления

MeNotifications: собственные handles50000+, mask64bit, version filter, FIFO сообщения, выбор по match_id, ручной сигнал пока очередь непуста, close. Категория id=(id>>25)&63, версия=(id>>16)&511, по pinned SDK. Бюджеты256listeners/1024сообщения, переполнение broadcast проверяется до любых изменений и явно останавливается.

Реализованы XamNotifyCreateListener и XNotifyGetNext. Single-handle wait и count1 NtWaitForMultipleObjectsEx видят состояние очереди без потребления сообщения. XMP setter теперь рассылает0A000003 реальным подходящим listeners и будит ожидания; прежнее предположение о пустом множестве удалено из кода. В самой игре создан listener50000 с mask1.

Модель не генерирует вымышленных событий sign-in/device/network. Handle duplication, object-pointer waits, иные broadcast producers/system listeners ещё не реализованы.

## Профиль

XamUserGetSigninState в диагностическом профиле сообщает0, потому что постоянный пользователь ещё не создан. XamUserGetXUID при существующем диапазоне слота возвращает HRESULT80070525 (NO_SUCH_USER) и нулевой BE64 output. Нулевой output/некорректный слот возвращает80070057. Не создаёт временный XUID, не заявляет успешного входа и ничего не записывает в сохранения.

Игра обработала эти результаты и дошла до XamUserCreateAchievementEnumerator, LR829F9E74, основной поток61001000. Это новая явная граница; профиль и достижения пока не реализованы. Сохранения всё ещё требуют полноценного постоянного профиля, стабильных путей, записи/чтения/backup и проверки после обновлений.

## Проверки

436/436 на планшете: предыдущие412 +10 model notification queue/filter/version/FIFO/match/close +7 integration XMP→listener→manual wait→read→timeout→close +4 unsigned signin slots +3 XUID failure/output checks.

Логи: device-tdgame-long.log, device-startup-int-long.log, device-notification-long.log, device-notify-integration-self-test.log, device-signin-absent-long.log, device-xuid-self-test.log, device-xuid-long.log. Метаданные новых ресурсов — bootstrap-*-extraction.json и startup-int-extraction-result.json. Игровые байты остаются в work/ и на устройстве.

Далее: фактический вызов achievement enumerator и согласованная модель отсутствующего пользователя/локального профиля. Не возвращать фиктивный успешный enumerator или сохранение без хранилища.
