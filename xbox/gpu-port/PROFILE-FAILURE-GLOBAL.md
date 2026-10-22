# Отсутствующий профиль, capabilities и game-global null

2026-10-21, Y700 Gen4 / test device.

## Изменения

XamUserCreateAchievementEnumerator получил ограниченный диагностический absent-profile failure path. Валидируются user<4, count>0 и выходные указатели; неправильные параметры87, при отсутствии созданного пользователя1317 NO_SUCH_USER. Не создаётся фиктивный enumerator, output size/handle сохраняются. Это исследовательская failure policy, а не полный сервис достижений Xbox. Игра запросила count50 и обработала1317.

XamInputGetCapabilities: у standalone probe нет Android input assignment, результат1167 без изменения caps buffer. NULL caps отвергается160 BAD_ARGUMENTS до обращения к provider, как pinned XAM entry. Реальный ввод игры пока не подключён.

443/443 self-tests на планшете:436 предыдущих +4 achievement failure/untouched outputs/invalid user/count +3 caps absence/untouched buffer/NULL.

## Раннее ожидание

Добавлены ограниченные снимки worker object и pool entries. В одном зависшем запуске объект1014D3E0: vtable82139B38, thread handle20004, field12=2, field16/20=0, field32=1, pool83097678. В pool два элемента:20004 с field32=1 и20008 с field32=0.

Уточнение прежнего описания: поле32 нельзя называть busy/completion flag. В sub823856B0 оно выставляется1, когда выбирается гостевой CPU2. Getter826CCC08 просто читает его. Подменять поле0 без восстановления смысла нельзя. Адреса зависят от запуска, это снимок, не постоянная конфигурация.

## Новая достигнутая ошибка

device-profile-pool-long.log прошёл achievement failure и capabilities, создал thread12, затем остановился SIGSEGV11 на guest address0. Host offset30C7834 символизируется sub829A0400, LR829A0424.

Исходный код: sub829A0400 вызывает sub8223CF38, кладёт возвращённый указатель в this+268 и читает его vtable. Getter8223CF38 читает global830DD060; он оказался0. Записи в этот global найдены в82255468 (создание объекта) и822555F0 (очистка). Назначение объекта и причина неинициализированного/очищенного global пока не доказаны. Нельзя автоматически связывать это с профилем, сетью или графикой, нельзя заменять pointer на выдуманный объект либо отображать page0.

Следующий шаг: отследить вход/выход82255468,822555F0 и создание/очистку830DD060, найти конкретную ошибку инициализации. Раннее ожидание рабочего пула остаётся отдельной нерешённой веткой; длинный таймер не устраняет её.

Логи: device-achievement-absent-self-test.log, device-achievement-absent-long.log (ранний timeout), device-achievement-absent-repeat.log (caps boundary), device-worker-object-long.log, device-caps-self-test.log, device-caps-long.log (ранний timeout), device-profile-pool-self-test.log, device-profile-pool-long.log (null dereference).
