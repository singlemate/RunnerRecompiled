# XAudio client: AAudio endpoint и начало guest callback

2026-10-21, Lenovo Y700 test device. Это ограниченный диагностический путь --gpu-explore, не готовая звуковая система.

Добавлен реальный AAudio output stream: shared, PCM float, stereo,48000Hz, blocking mode. Перед успешной регистрацией проверяются фактический формат/каналы/частота и переход в STARTED с ограниченным ожиданием. Ошибка открытия/старта возвращает E_FAIL, нулевой/невалидный callback — E_INVALIDARG. Повторный клиент пока диагностически останавливается. Нет генерации тестового тона или записи PCM в stream на этом этапе.

XAudioRegisterRenderDriverClient проверяет8байт callback descriptor и4байта output, создаёт отдельные host-thread и XenonRecomp context/PCR/KTHREAD/TLS/stack. Нормальное аудиовыполнение, не GPU interrupt: TLS сохранён, IRQL исходный0. В r3 передаётся адрес BE32 wrapped argument, как в pinned AudioSystem; обёртка находится в зарезервированной области loader metadata PCR+1800 вне KTHREAD. Возвращается SDK-compatible driver handle41550000. PPCContext RexGlue не используется и не приводится к XenonRecomp.

Предусмотрено только4начальных разрешения на callback по нижнему пределу очереди pinned SDK. Успешно проигранные кадры не выдумываются; таймер не пополняет очередь. SubmitFrame/Unregister пока остаются stop imports. Если все4 callback вернутся, diagnostic worker завершится без утверждения, что PCM проигран. Часть ресурсов живёт до завершения диагностического процесса, полноценная остановка/перерегистрация клиента ещё не реализована. Исключение guest callback явно завершает probe вместо молчаливой потери worker.

На устройстве подтверждены endpoint STARTED, регистрация callback82834FA8, argument10056EEC, wrapped pointer61071800 и вход callback credit0. Callback дошёл до KeWaitForMultipleObjects. Параллельно главный поток дошёл до RtlInitAnsiString(LR829FD038), а аудиопоток — XAudioGetVoiceCategoryVolumeChangeMask. Все три пока stop imports; основной финальный stop — RtlInitAnsiString. GPU31/31.

282 self-tests прошли, включая4новые проверки настоящего AAudio stream: STARTED, отказ повторного open того же объекта, идемпотентное close, повторное открытие после close. Это не тест слышимого звука или XMA-декодирования. Доставка реального guest callback проверена игровым запуском, возврат callback пока не подтверждён.

Журналы: device-aaudio-client-started-explore.log, device-aaudio-client-self-test.log. Следующая работа: правильный wait-any/wait-all для гостевых объектов в KeWaitForMultipleObjects, остальные системные зависимости, затем PCM conversion/submission и пополнение credit после реального потребления.
