# XMP query и FIFO execution gate

2026-10-21, Y700 Gen4 / test device.

XMsgInProcessCall FA/7001B — XMPGetPlaybackController. По pinned SDK структура12байт: client2, controller_ptr, locked_ptr. Реализован только этот диагностический запрос: проверенные BE32 outputs0/0 для профиля без системного музыкального клиента, без изменения звука игры. Остальные app/messages/параметры останавливаются. На неосновном потоке точка переключения10мс соответствует текущему SDK profile. Проверки нулевого и явного12 размера, outputs и соседней памяти прошли.

Добавлен MeExecutionGate с FIFO ticket order вместо std::mutex для сериализации гостевого исполнения. State_changed переведён на condition_variable_any. Сохраняется один владелец, блокирующие kernel waits отпускают gate; это не вытесняющая многозадачность. При повторном захвате текущий поток больше не может обогнать уже стоящих в очереди. Тест с двумя ожидающими и повторным захватом проверил порядок1,2,3, а все прежние проверки threaded/kernel/GPU boundaries прошли.

399/399 self-tests на устройстве. Однако FIFO не устранил все ранние timeout: несколько прогонов достигли44 раньше XMP. Нельзя считать причиной всех остановок исключительно unfair mutex.

В device-fifo-repeat.log подтверждён xmp_playback_controller и следующий запрос IpDrv.xxx. Пакет65536байт извлечён из своей копии и перенесён на устройство. device-ipdrv-explore.log снова timeout44; повтор device-ipdrv-repeat.log дошёл до новой границы XMsgStartIORequestEx, LR82B5A120, exit42.

## Следующая граница

По исходному callsite: appFA, message7001A (XMPSetPlaybackController), overlapped0, args12байт, optional unknown r8. Пакет аргументов содержит client/controller/playback_client. SDK меняет playback_client и рассылает kMsgPlaybackControllerChanged. Default playback_client в SDK — title1, system0. Пока не подменять этот запрос простым success: сначала проверить фактические аргументы и обработку уведомлений/last_error; ненужный async overlapped не объявлять реализованным.

Логи: device-xmp-self-test.log, device-xmp-explore.log, device-xmp-repeat.log, device-fifo-self-test.log, device-fifo-final-self-test.log, device-fifo-explore.log, device-fifo-repeat.log, device-ipdrv-explore.log, device-ipdrv-repeat.log.
