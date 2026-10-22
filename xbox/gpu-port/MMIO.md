# Scalar MMIO из AOT-кода — 21 октября 2026

В guest_memory_access.h введён быстрый путь обычной памяти с исходной big-endian семантикой и отдельный путь GPU-окна 0x7FC80000..0x7FC8FFFF. Заголовок подключается принудительно при сборке всех игровых TU и загрузчика. Существующий generated ppc_context.h уже допускает переопределение PPC_LOAD/PPC_STORE, менять игровые исходники или XEX не потребовалось.

MeGpuMmio принимает адрес и host-order значение, не зависит от ABI PPCContext и не создаёт вторую память. Поддерживает только выровненные 32-битные обращения; прочие размеры, выход за границу, отсутствующий/отказавший обработчик отвергаются. В runtime отказ даёт gpu_mmio_unavailable и exit 95, вместо чтения PROT_NONE. На игровом пути callbacks пока НЕ установлены; фиктивных значений GPU-регистров нет. Доступ сериализован прежним execution gate, это не интерфейс для вызова из произвольного GPU worker.

На Y700: 136/136 loader checks. Проверены endian обычной памяти 8/16/32/64 бит, read/write callback, размеры/выравнивание/границы и unbound backend. Настоящая функция sub_82230088 из отдельно собранного AOT-объекта вызвана с искусственным MMIO-адресом в self-test: запись дошла до тестового callback без доступа к PROT_NONE. Тестовые callbacks удаляются до завершения self-test. Отдельный процесс --mmio-unbound-test вызывает ту же функцию и завершился ожидаемым кодом 95. Это проверяет транспорт, а не готовый GPU.

Все 300/300 игровых TU пересобраны в новый приватный libmirrorsedge_ppc_mmio.a. Реальный запуск с новой библиотекой дошёл до прежней явной остановки VdInitializeEngines (exit 42), без более ранней ошибки. APK не обновлялся, обновлён только native executable в /data/local/tmp.

Ограничения: SIMD memory ops и lwarx/stwcx используют прямые адреса и пока обходят scalar hooks. GPU-окно остаётся PROT_NONE, поэтому такой доступ не станет обычным RAM-регистром, но подробного MMIO fault-handler для него нет. Memory invalidation/write-watch, полноценный Rex worker и гостевые прерывания всё ещё нужны; CommandProcessor не создан, GPU не инициализирован, изображения нет.

Сборка из корня workspace:

```powershell
& work/venv/Scripts/python.exe outputs/mirrors-edge-native/xbox/startup/build_guest_mmio.py
& outputs/mirrors-edge-native/xbox/startup/build.ps1 -Workspace $PWD
```

Игра, generated исходники и бинарные результаты остаются в work/. Публичные файлы — собственные headers, scripts и отчёты.
