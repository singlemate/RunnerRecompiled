# Очередь и достигнутые frsqrte

2026-10-21, Y700 Gen4 / test device.

Диагностический native probe запускается ADB без Activity/Surface; APK не обновлялся. Пользователь не должен ожидать изменений экрана планшета. Подключение Vulkan presentation остаётся отдельной незавершённой задачей.

## Что установлено

При Sleep(0) caller82372140 очередь831076DC первоначально имеет producer=consumer10210020, task0. Затем оба указателя продвигаются к10210890. Следовательно этот наблюдавшийся worker poll сам по себе не доказывает потерю задания: в успешном по расписанию прогоне очередь обрабатывается.

Новый запуск завершился exit133. Crash buffer Android показал SIGTRAP в sub82EBBDE8; символизация PC638A5F4 и дизассемблирование подтвердили BRK на ранее оставленной unsupported frsqrte. Теперь SIGTRAP также перехватывается диагностикой.

## Ограниченное продолжение

Два сайта82EBBF18 и82EBC040 в private TU298 переведены на guarded helper me_frsqrte_diagnostic. Подготовка воспроизводится собственным скриптом prepare_frsqrte_diagnostic.py, который проверяет единственность точных сайтов. Затем TU298 пересобран с прежними -O1/target/includes, архив обновлён и probe перелинкован.

Helper включён только в --gpu-explore-poll-yield и принимает конечные положительные входы. Возвращает double(float(1/sqrt(value))), как pinned RexGlue floating_point builder. Это диагностическая совместимость, не bit-exact реализация оценки Xenon, FPSCR/NaN/negative/zero/overflow/underflow semantics не реализованы. Неподдержанные режимы или результаты явно прекращают запуск97. Третий frsqrte и dcbst остаются traps.

На устройстве достигнуты оба сайта: input1024→seed0.03125 и input2048→seed0.022097086533904076. Последующая игровая последовательность Newton refinement остаётся исходной. Следующая явная граница NtCreateMutant, LR82B5EA00, thread61091000; probe_exit42. Аудиодекодирование/воспроизведение ролика этим не подтверждено.

372/372 self-tests: предыдущие362 и10 проверок положительного seed/Newton refinement для0.25,1,2,4,48000. Это не тесты битовой эквивалентности PPC.

Логи: device-job-queue.log, device-frsqrte-self-test.log, device-frsqrte-explore.log. Игровые данные и сгенерированные TU остаются приватно в work/.

Далее: kernel mutant с владельцем, рекурсивным захватом, release и интеграцией в wait; нельзя просто возвращать фиктивный handle/success.
