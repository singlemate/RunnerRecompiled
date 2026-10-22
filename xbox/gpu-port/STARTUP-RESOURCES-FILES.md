# Пакет startup: XEX-ресурсы и реальные игровые файлы

2026-10-21. Lenovo Y700 test device. Изменения проверены в --gpu-explore; приложение/APK и пользовательские сохранения не менялись.

## Достигнуто
- KeEnableFpuExceptions(0): маскирует ARM FPCR IOE/DZE/OFE/UFE/IXE/IDE, сохраняет rounding/FZ. Включение гостевых trap по-прежнему stop.
- XexGetModuleHandle(NULL) возвращает уже установленный основной модуль60001000. Именованные модули пока stop.
- XexGetModuleSection ищет имя<=8байт в оригинальном resource header0x2FF, проверяет покрытие диапазона объединением загруженных фрагментов, пишет адрес/размер. NOT_FOUND оставляет outputs. HashSec(83160000,451байт) найден реальной игрой.
- RtlUnicodeToMultiByteN: проверенный ASCII subset UTF16BE, truncation по capacity, bytes-written, временный буфер для overlap. Не-ASCII требует codepage и останавливается.
- NtCreateFile: только OPEN existing read-only, D: и проверенный набор access/options, без APC/event. NtReadFile: явное смещение или current position, реальный pread, короткое чтение/EOF/ошибка в IOSB. NtClose закрывает собственный file fd. NtQueryInformationFile поддерживает class34 NetworkOpenInformation56байт с реальными EOF/allocation, readonly attrs. Original Xbox timestamps недоступны, возвращаются0; другие классы stop.
- MmQueryStatistics:104байт, 512MiB research profile, actual physical/committed virtual/image/stack counters; доступный budget консервативно вычитает tracked backing из physical pool492MiB. Это не Android free RAM и не полноценный unified allocator. Поля unsupported categories0. Экспорт explore-only; ошибка размера BUFFER_TOO_SMALL.
- XamGetSystemVersion: explore-only value0 как pinned SDK. Это диагностический legacy feature baseline, не подлинная версия Xbox и не обещание всех старых API.

## Подготовка ресурсов
Старый bundle содержал только10PE-секций и пропускал ресурсы вне их VirtualSize. Через XenonUtils извлечён полный BASIC-decoded buffer0x1168000байт. Security header imageSize0x1170000 больше на32KiB; первоначальная попытка записать imageSize не удалась, её пустой вывод не использован. dump_private_image.cpp теперь вычисляет фактическую длину из BASIC blocks и проверяет границы до записи. Это локальная утилита для BASIC-compressed XEX, не универсальный extractor.

prepare_xex_resources.py добавляет оригинальные resource ranges к приватному bundle:12секций,18078639байт. На устройстве используется me_native_entry_sections_resources.bin, старый bundle сохранён. Полные образ/ресурсы остаются только work/xbox-game-private и на планшете. resource-bundle-result.json содержит только размеры/адреса/хеши.

## Read-only файловый provider
extract_svod_file.py читает оригинальный GOD/SVOD и избирательно извлекает ресурс в work/xbox-game-private/readonly-files. Он также создаёт authoritative inventory1521entry. Хеш-дерево SVOD/подпись не валидируются этим инструментом; оригинал не изменяется. Существующий staged файл не перезаписывается, если содержимое отличается.

На планшете root/data/local/tmp/me_game_readonly содержит staging-копии, не основное хранилище будущего приложения. Provider нормализует D:/ paths в lowercase, отклоняет '..', другие volumes/неASCII. inventory.txt поддерживает LF/CRLF. Если путь отсутствует в оригинальном inventory — NO_SUCH_FILE; если есть, но не перенесён, — диагностическая остановка вместо ложного отсутствия. Read-only open использует O_NOFOLLOW для final component; root/родительские каталоги создаются нашим staging-tool, полное приложение потребует более строгого directory-fd mount. Индекс names-only: directories ещё не реализованы. Максимум256дескрипторов за процесс, slots не переиспользуются. Асинхронное I/O, directory handles, запись, saves, полный mount/VFS пока не реализованы.

Игра реально открыла и прочитала целиком:
- D:\TdGame\Xbox360TOC.txt —110189байт;
- D:\TdGame\Config\Xenon\Cooked\Coalesced.ini —263038байт.
Отсутствие Binaries/EpicInternal.txt подтверждено оригинальным inventory. Сырой Xbox partition0 не подключён: возвращён NO_SUCH_DEVICE, caller прошёл дальше. Не открываются диски Windows/Android. STATUS_NO_SUCH_DEVICE и NO_SUCH_FILE переводятся в DOS2 по pinned SDK error table.

## Проверки и следующая граница
333 self-tests прошли: read-only file/offset/short-read/EOF/close/запрет pwrite, missing vs not-staged и CRLF index, path traversal/volume rejection; FPU state preservation, module handle, original HashSec/not-found, Unicode truncation/zero capacity, size checking и layout memory statistics.

В игре GPU31/31, PCM bootstrap как ранее. После чтения конфигурации/статистики выбран legacy XAM path. Следующая остановка NetDll_XNetStartup, LR829C2798,caller1. Нужна отдельная offline/network policy; успешная регистрация сети/онлайн-служб пока не имитируется.

Журналы device-startup-batch-self-test.log, device-xam-version-explore.log, device-coalesced-explore.log, device-indexed-files-explore.log. SHA256 оригинального default.xex неизменен:3ed882fd1900ff2bbccf1de745fb7e92c7d2168bb06069cbab311ac8eff28e1b.
