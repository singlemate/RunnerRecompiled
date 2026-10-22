# Атомарные гостевые SList

2026-10-22. InterlockedPopEntrySList и InterlockedFlushSList.

ABI и layout сопоставлены с pinned RexGlue xtypes.h/xboxkrnl_threading.cpp: header8байт (BE32 next,BE16 depth,BE16 sequence), node4байта (BE32 next). Guest pointer возвращается как32-битный адрес, без host pointer. Заголовок требует8-байтового выравнивания и доступных8байт, pop проверяет4-байтовое выравнивание и доступность узла.

Pop использует seq_cst64-bit compare-exchange непосредственно над гостевой памятью, с преобразованием порядка байтов. Непустой pop уменьшает16-bit depth и сохраняет sequence; пустой не меняет header. Узел не освобождается и его next не затирается. Flush атомарно обнуляет весь header и возвращает прежнюю голову, как pinned SDK.

Штатный runtime по-прежнему выполняет guest code под execution gate. Эта реализация не заявляет безопасное освобождение узлов произвольными сторонними native threads или полноценный механизм reclamation/ABA. Узлы должны оставаться доступными во время операции.

На Y700 прошли552 self-tests (work/startup/slist-self-test.log, exit0): цепочка из2узлов, depth/sequence/порядок байтов, пустой pop/flush и сохранность detached nodes. Дополнительный тест вызывает реальный export из4host threads без сериализации gate над стабильными64guest nodes; проверяется ровно64уникальных результата и итоговый пустой header с сохранённой sequence.

Игровой лог — work/startup/slist-game.log.

В slist-game.log прежняя граница пройдена; следующая остановка — ExCreateThread_probe_capacity_exceeded, caller82A01930 (exit42), после16records. Следующий этап: [расширение thread capacity](THREAD-CAPACITY.md).
