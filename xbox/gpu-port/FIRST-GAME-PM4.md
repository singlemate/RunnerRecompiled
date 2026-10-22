# Первые игровые PM4 обработаны — 21 октября 2026

В диагностическом режиме --gpu-explore настоящий startup Mirror’s Edge записал CP_RB_WPTR сначала 0x16, затем 0x19. На остановке CPU probe дал GPU thread 50 ms под освобождённым execution_gate. Настоящий SDK CP разобрал буфер, включая indirect buffer, и записал read pointer 25 в shared memfd; write pointer также 25. Прямой вывод gpu_stop_snapshot в device-gpu-explore.log. Read pointer не изменялся загрузчиком и synthetic completion не подставлялось. Это подтверждает потребление первой партии игровых команд; draw/кадр/меню НЕ подтверждены.

## Диагностический identifier режим

Default запуск сохраняет остановку GPU_system_buffer_identifier_nonnull_unsupported. Отдельный --gpu-explore только сохраняет и проверяет адрес 0xFFBF4008 в live physical allocation; не реализует completion/identifier kernel protocol и не синтезирует fence values. Такой режим нужен для измерения следующего фактического поведения, не объявляется нормальным полноценным runtime.

Проверены pinned RexGlue source и [первичный исходник Xenia](https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc): setter там остаётся пустым stub. Это НЕ доказательство полного hardware contract; поэтому diagnostic допущение не перенесено в default mode. KiApcNormalRoutineNop возвращает 0, как отдельный экспорт в SDK; это не реализация APC очередей. KeSetBasePriorityThread сохраняет guest hint, обновляет KTHREAD.priority (offset 0x70) и возвращает предыдущий guest hint. Host/Android scheduling priorities не меняются, текущий cooperative scheduler не гарантирует priority ordering.

Реализованы read-only guest video APIs: VdQueryVideoFlags=3 для выбранного 1280x720/widescreen профиля, display gamma type 2/power 2.22222233f, display information 0x58 bytes согласно изученной SDK layout. Это параметры guest profile, не Android panel calibration. VdCallGraphicsNotificationRoutines обрабатывает только event 1 с пустым списком подписчиков: register-notification API по-прежнему stop import, поэтому слушателей в этом runtime не может быть. Нет выдуманного registered callback. KeDebugMonitorData и KeCertMonitorData получили настоящие pointer cells со значением 0 (Xbox debug/cert monitor не подключены).

## Исправление indirect buffer

В ходе реального выполнения выявлена наша ошибка проверки границ: SDK RingBuffer хранит равные offsets как empty. Для linear IB прежний capacity=count*4 и write_offset=count*4 давали 0 доступных bytes, а наша проверка заголовка отвергала буфер. У linear IB теперь логическая capacity=(count+1)*4, write_offset=count*4. Дополнительный sentinel word никогда не читается/не пишется; фактический доступ ограничен доступным read_count. Count=0 сразу завершает буфер. Count и весь physical диапазон проверяются через GpuMemory::IsPhysicalRangeValid; MeRexGpuMemory использует allocation registry и raw view. Изменены оба linear entry paths (indirect и ExecutePacket(ptr,count)); primary circular ring сохраняет прежнюю wrap семантику. Частично прочитанный/неподдержанный пакет всё ещё latch fault и не подтверждает primary read pointer.

На Y700 synthetic CP probe теперь включает INDIRECT_BUFFER с полным 2-dword Type0 пакетом, расположенным в последних 8 bytes доступной страницы, за которой PROT_NONE. Реальный CP обработал direct Type0 и IB, регистр получил 0x5678, primary read index 5, shutdown прошёл, exit 0. Лог device-indirect-check.log. Это показывает, что sentinel не требовал чтения следующей недоступной страницы.

## Границы результата

На неподдержанном CPU import diagnostic stop отпускает gate на 50 ms, затем читает реальный writeback. Это окно диагностики, а не обычная асинхронная CPU/GPU scheduling policy. Несколько guest worker threads могут одновременно дойти до stop; текущие остановки — KeEnterCriticalRegion и KeWaitForSingleObject. В последнем run основной поток завершил процесс exit 42 после snapshot 25/25. Нормальный scheduler/ожидания и APC disable semantics ещё нужны. Automatic vblank/presentation отсутствуют.

Проверки: 228/228 loader checks на финальном binary; 6/6 host packet-boundary checks; synthetic Vulkan IB boundary process exit 0; full shared GPU link 80 objects / 0 unresolved. Default mode отдельно проверен: всё ещё exit 42 на non-null identifier. Все device процессы завершены. APK/XEX/игровые bundle-файлы не менялись. Новый native binary установлен ADB. Кумулятивный SDK patch обновлён, включая range validation и linear IB fix; старые patches поверх него не применять.

Следующая цель — корректные KeEnter/LeaveCriticalRegion и KeWaitForSingleObject для уже существующих событий, чтобы игровые worker threads могли продолжить работу без diagnostic drain окна. После этого искать следующую реальную партию команд/первый draw; не считать этот результат готовым renderer или работающей игрой.
