# Третий достигнутый frsqrte

2026-10-22. После чтения TdMainMenu_Images.xxx.

SIGTRAP в work/startup/menu-images-game.log, host offset66FBB88 старой сборки, символизирован в __imp__sub_823841A8. В приватной generated TU36 найден единственный соответствующий unsupported instruction stub: frsqrte f13,f0 по82384344. Сырые bytes исходного decoded image FDA00034 подтверждены независимым PowerPC decoder; совпадают с двумя ранее достигнутыми frsqrte.

prepare_frsqrte_diagnostic.py расширен на этот третий сайт с проверкой всех трёх исходных opcodes и идемпотентной проверкой generated markers. Только приватная TU36 пересобрана с прежними ARM64/O1 flags и заменена в собственном архиве. Оригинальные XEX/SVOD не изменяются.

Новый сайт вызывает существующий me_frsqrte_diagnostic: только диагностический polling mode и finite-positive input; float seed1/sqrt(x), не bit-exact Xbox estimate. Другие входы/режимы останавливают probe. Остальной гостевой вычислительный код сохранён. Логирование ограничено первыми двумя вызовами каждого сайта в каждом потоке.

560 regression self-tests прошли на Y700, work/startup/menu-frsqrte-self-test.log, exit0. Это проверка runtime после замены TU, не доказательство прохождения данного игрового вызова. Игровой лог — work/startup/menu-frsqrte-game.log.

Дополнительно подготовлены все21пакета TdMainMenu_Audio0..20.xxx из пользовательского SVOD, суммарно39518208байт. Они переданы в приватный read-only root после завершения предыдущего процесса. Подготовка не означает чтение или воспроизведение аудио; непрерывный audio/XMA остаются открыты.

menu-frsqrte-game.log подтвердил новый вызов frsqrte82384344 с finite-positive input3388892.75, seed0.00054321414791047573. Прежний trap пройден. Затем Audio0.xxx полностью прочитан: последний offset2490368/read32768, сумма2523136. Следующая остановка — XMACreateContext, caller8282C120, exit42. Подробности: [слоты XMA-контекстов](XMA-CONTEXT-POOL.md).
