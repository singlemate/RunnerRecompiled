# Остановка GPU при ошибке пакета

Изменён настоящий CommandProcessor RexGlue: короткий Type1 отвергается до чтения payload и записи регистров; пустое первичное кольцо не читается; при ошибке фиксируется fault, worker останавливается до writeback, OnPrimaryBufferEnd не вызывается. Возвращаемый индекс остаётся на начале неудавшегося пакета. Ошибка indirect buffer передаётся в primary. Уже совершённые побочные эффекты не откатываются, повтор пакета запрещён до пересоздания CommandProcessor.

Проверка: 35/35 GPU translation units собраны NDK r30 для aarch64-linux-android35. check_packet_boundaries.py: 6/6 host-проверок извлечённых из SDK методов ExecutePrimaryBuffer/ExecutePacketType1 с настоящим SDK RingBuffer. Сценарии: пустое кольцо, отсутствующий/неполный Type1 payload, полный пакет, wraparound, ошибка после успешного пакета. Dispatcher и provider в harness упрощены; это не выполнение полного GPU и не тест на планшете.

Патч rexglue-packet-failure.patch применяется дополнительно к rexglue-android-gpu.patch. Тест запускается Python из work/venv из корня workspace. Приватные игровые файлы и APK не менялись. Полная интеграция всё ещё требует Memory/MMIO/invalidation, host-worker и guest-interrupt адаптеров; игра останавливается на VdInitializeEngines.
