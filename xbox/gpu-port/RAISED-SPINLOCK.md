# Raised-IRQL spinlock и GPU callback

2026-10-21, Y700 test device. Добавлены KeAcquireSpinLockAtRaisedIrql и KeReleaseSpinLockFromRaisedIrql. Владение хранится BE32 адресом PCR, проверки выравнивания, диапазона, контекста и рекурсивного захвата; IRQL должен быть >=2 и не меняется. При конкуренции освобождается execution_gate; release уведомляет ожидающих. Это кооперативная модель, не аппаратное маскирование прерываний.

Наш внешний GPU callback теперь исполняется с гостевым IRQL_DISPATCH (2). RAII восстанавливает предыдущее значение и при исключении. Это выбранная модель внешней доставки отложенной работы, а не реконструкция аппаратного interrupt controller Xbox. Не переносится контекст RexGlue в XenonRecomp.

252 self-tests прошли на установленной сборке: добавлены пары acquire/release при уровнях2/3 с сохранением регистров, наблюдение IRQL2 внутри callback, восстановление IRQL после нормального выхода и исключения.

В реальном запуске callback прошёл обычный lock1004F7C8 и raised lock1004E118, оба освобождены. GPU продолжил WAIT_REG_MEM и дошёл до IssueSwap (backend сообщил PRESENT: NO PRESENTER). Это подтверждает достижение swap hook, не вывод и не полный RPTR31. Последний снимок WPTR31/RPTR28. Запуск затем остановился SIGSEGV на гостевом адресе7FEA1800, LR82A1FD88. В pinned SDK окно7FEA0000..7FEAFFFF принадлежит XMA audio decoder: следующая исследуемая зависимость — аудиоустройство, не ещё один GPU-регистр.

Журналы: device-raised-spinlock-self-test.log, device-raised-spinlock-explore.log. Общий план: ../../CHECKLIST.md. APK и оригинальные игровые файлы не изменены.
