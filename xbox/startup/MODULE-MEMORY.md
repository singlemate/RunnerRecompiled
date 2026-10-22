# Модуль и начальная виртуальная память — 21.10.2026

На Lenovo Y700 (TB322FC, test device) рекомпилированный _xstart прошёл прежнюю остановку XexExecutableModuleHandle. Затем оригинальный CRT-код зарезервировал 1 МиБ гостевой памяти и закоммитил первые 64 КиБ через реализованное подмножество NtAllocateVirtualMemory. Следующая диагностическая остановка — KeGetCurrentProcessType (LR 0x829FF2C0). Меню и геймплей не достигнуты.

## Реализовано

XexExecutableModuleHandle указывает на настоящий для этого loader гостевой объект загруженного модуля: переменная 0x60000000 → LDR entry 0x60001000 → исходный XEX header 0x60010000. В LDR entry заполнены image base/size, entry point и xex_header_base по формату Xbox. Поля списка загрузки, kernel object handle, поиск по имени и разгрузка не реализованы. Это минимальная модель одного главного модуля, а не полный loader Xbox.

В гостевую память скопированы 12 288 байт настоящего XEX-заголовка пользователя, затем его страницы защищены от записи. Исходный файл не менялся. RtlImageXexHeaderField обрабатывает три формы: встроенное значение (tag 0), указатель на встроенную ячейку (tag 1), указатель на внешние данные внутри заголовка (остальные tags). Проверяются границы записей и payload. Доступ разрешён только к зарегистрированному заголовку нашего модуля; другие указатели вызывают диагностическую остановку.

Игра запросила ключ 0x00020401. Его нет в исходных optional headers: возврат 0 соответствует реальному отсутствию поля, а не заглушке успеха. После этого код игры использовал собственное значение по умолчанию для размера кучи.

NtAllocateVirtualMemory поддерживает положительный размер, reserve/commit в собственном гостевом диапазоне 0x10000000..0x40000000, страницы 4/64 КиБ, read/write, HEAP/LARGE_PAGES/NOZERO flags. Состояния reserved и committed разделены. Reserve оставляет PROT_NONE; commit реально вызывает Android mprotect и даёт доступ к анонимным нулевым страницам. Повторный commit не обнуляет содержимое. Успех возвращается только после успешного отображения и записи guest big-endian выходных параметров.

Другие protection/debug/особые allocation flags останавливаются с диагностикой. MEM_FREE/DECOMMIT/RESET, TOP_DOWN, 16-МиБ страницы, отрицательные размеры и полная семантика NTSTATUS ещё не реализованы. Выходные указатели поддерживаются в диагностическом стеке или уже committed heap; глобальные/прочие области требуют дальнейшей поддержки. На устройстве host page size 4096; общая совместимость этого менеджера с 16-КиБ страницами Android не заявлена.

## Подтверждение на планшете

```text
module_installed: entry=829fab98, original header bytes=12288
xex_header_query: key=00020401, found=false
virtual_allocate: type=60002000, base=10000000, size=1048576, status=0
virtual_allocate: type=60001000, base=10000000, size=65536, status=0
unimplemented_function_import: KeGetCurrentProcessType, lr=829ff2c0
```

Exit 42 — преднамеренная остановка. device-memory.log содержит полный вывод; device-memory-result.json — контрольные суммы и события. device-module.log отражает промежуточный запуск с одним модулем до реализации allocation.

## Проверки и сборка

- header-tests-result.json: 15 проверок формата, границ, переполнения адреса и отсутствующего ключа на искусственном заголовке; дополнительно все 15 optional headers настоящего XEX сверены с независимым Python parser.
- memory-tests-result.json: 16 проверок reserve/commit, конфликтов, округления, сохранения состояния при повторном commit и ошибки отображения. Логика проверена на Windows x64; реальный mprotect и большой guest stack дополнительно исполнены самим стартовым кодом на Android.
- Подготовлены 273 останавливающиеся импортные функции; реально реализованы две функции и одна переменная. Другие переменные сохраняют guard pages. Это не RexGlue runtime: он пока не связан, ABI-мост не реализован.

Воспроизводимая сборка текущего loader:

```powershell
./outputs/mirrors-edge-native/xbox/startup/build.ps1 -Workspace $PWD
```

Скрипт использует существующие NDK, generated-fourth-final и libmirrorsedge_ppc_fourth.a в work/, сам генерирует диагностические import endings, компилирует loader и связывает архив. Проверен реальным запуском сборки. Результат: work/startup/current/me_memory_probe.stripped. На устройстве проверен идентичный исходный вариант из work/startup/me_memory_probe.stripped; хеш именно проверенного файла записан в отчёт. Игровой архив и XEX-заголовки в outputs не включены.

Файлы на планшете: /data/local/tmp/me_native_memory_probe и /data/local/tmp/me_native_xex_headers.bin; прежние bundle и entry probe также сохранены. Штатный APK не менялся. Процесс завершился на диагностике.

Следующий этап: модель типа процесса и начального потока, затем синхронизация/куча по реально достигаемым API. Нельзя считать резервирование первых страниц готовой игровой кучей, реализацией потоков или запуском UE3.

Формат проверен по локальным исходникам RexGlue c94f5ebd:
- include/rex/system/xmodule.h (X_LDR_DATA_TABLE_ENTRY)
- src/system/user_module.cpp (Load/GetOptHeader)
- src/kernel/xboxkrnl/xboxkrnl_memory.cpp (API и флаги)
