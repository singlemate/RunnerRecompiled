# XMP setter, input absence, dcbst и handle wait

2026-10-21, Lenovo Y700 Gen4 / test device.

## Реализовано и проверено

XMsgStartIORequestEx поддерживает только appFA/message7001A, overlapped0, args12байт и валидный optional extra8байт. Проверяются сочетания client/controller и playback0/1, сохраняется музыкальный playback client, last_error KTHREAD+160 очищается. В профиле нет listeners: XamNotifyCreateListener остаётся unsupported import, поэтому рассылка0A000003 имеет0получателей, как SDK broadcast в пустое множество. Доставка уведомлений и overlapped не объявлены реализованными. Игра запросила previous1→client1, то есть оставила музыкальное управление title.

XamInputGetState отражает отсутствие Android input assignment у standalone ADB probe: error1167 без чтения/изменения out_state, включая NULL query. Это не утверждение, что физический контроллер планшета отключён; Activity input bridge к гостевой игре пока отсутствует. В device-input-absent-third.log игра прошла опрос users0..3.

После этого SIGTRAP возник в sub82EBE5B8. Символизация и disassembly подтвердили оставленный dcbst82EC0968. Подготовочный скрипт prepare_frsqrte_diagnostic.py теперь дополнительно меняет только этот сайт на guarded me_dcbst_diagnostic(r11). Реализован исключительно coherent CPU VM диапазон128байт: проверка committed mapping и seq_cst barrier, данные не изменяются. Physical/device/GPU/code-cache semantics не поддержаны и останавливаются98. Реальная игра прошла диапазон11760000..11760180 и продолжила. Это не универсальный no-op для cache instructions.

NtWaitForMultipleObjectsEx поддержан для фактически достигнутого count1: валидный handles array, WaitAny/WaitAll индекс0, существующие single-handle acquire/timeout/abandoned semantics. Count>1, APC/absolute/прочие неподдержанные режимы останавливаются. Kernel pointer multiple wait — отдельный существующий API.

## Ресурсы и прогоны

Игра полностью прочитала IpDrv.xxx, затем Ts_LOC_int.xxx (720896байт) и Ts.xxx (65536байт). Следующим запросила Tp.xxx (163840байт): он подготовлен из своей копии и перенесён на планшет. Последний bounded resource staging run завершился timeout44 до подтверждения чтения Tp. Новую readiness/menu отметку не ставим.

412/412 self-tests на устройстве: 399 предыдущих, 3 setter state/last_error, 3 input absence/untouched output/NULL, 3 coherent VM handling и 4 count1 handle wait/auto-reset/timeout/close.

Логи: device-xmp-set-explore.log, device-input-absent-third.log, device-dcbst-self-test.log, device-dcbst-explore.log, device-handle-wait-self-test.log, device-handle-wait-explore.log, device-ts-localization-explore.log, device-ts-explore.log и device-assets-*.log. Метаданные выбранных ресурсов — *-extraction-result.json / asset-*-extraction.json, игровые байты только в work/ и на устройстве.

Далее: подтвердить чтение Tp.xxx и обработать следующий фактический запрос. Ранние остановки по таймеру всё ещё требуют отдельного устранения. Vulkan presentation, реальный ввод, непрерывный звук/декодирование и сохранения остаются незавершёнными.
