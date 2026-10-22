# Kernel mutant и NtResumeThread

2026-10-21, Y700 Gen4 / test device.

Добавлен собственный MeMutants: unnamed handles40000+, владелецKTHREAD, глубина рекурсивного захвата, ожидание свободного/своего объекта, запрет release чужим потоком, invalid handle, abandoned acquisition80 при завершении владельца. NtCreateMutant, NtReleaseMutant, NtWaitForSingleObjectEx и NtClose связаны с моделью. Release будит state_changed; конец обычного гостевого потока освобождает принадлежащие ему объекты как abandoned.

Объём ограничен: named attributes, закрытие owned object, embedded KMUTANT, handle duplication, ссылки через ObReferenceObjectByHandle и multiple wait пока не реализованы. Такие достигнутые режимы должны останавливаться; они не заявлены готовыми. NtClose owned mutant явно останавливается, чтобы не терять владение молча.

На игре подтверждено создание unnamed mutant40000 и создание suspended thread11. Следующая граница была NtResumeThread, LR829FB724. Добавлен resume по handle с выводом предыдущего suspend count, без изменения native affinity; использует существующую suspend_count/CV модель.

392/392 self-tests: 14 проверок mutant model, 4 интеграционных create/wait/release/close и 2 resume-count проверки дополнительно к372. Проверка прежнего suspended-thread поведения сохранена.

Игровой прогон device-resume-explore.log закончился timeout44 раньше новой границы. Повтор device-resume-repeat.log прошёл дальше и остановился на XMsgInProcessCall, LR82B5A198, r3FA, thread61091000, exit42. Это новая явная граница, а не подтверждение завершения потоковой синхронизации всего порта.

Логи: device-mutant-self-test.log, device-mutant-explore.log, device-resume-self-test.log, device-resume-explore.log, device-resume-repeat.log.

Далее: разобрать аргументы и реального адресата XMsgInProcessCall; системные сервисы не заменять общим success.
