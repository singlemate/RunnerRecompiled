# Первый визуально проверенный GPU-кадр

2026-10-22, Lenovo Y700 Gen4/Adreno830.

Подключён настоящий VulkanPresenter в offscreen режиме до SetupExternalGpu. Новый явный SetupExternalPresenter создаёт provider с presentation capabilities и presenter без WindowedAppContext. Обычный SetupPresentation для external host по-прежнему отключён; Android Surface не подключена. Подготовка SDK и пересборка graphics_system.cpp теперь включены в startup/build.ps1 через prepare_external_presenter.py.

GPU IssueSwap теперь может RefreshGuestOutput, а отдельный readback worker каждые3секунды вызывает CaptureGuestOutput. Он не держит guest execution gate во время Vulkan fence wait. Проверяются размеры/stride/объём RawImage; RGB сохраняется в P6PPM через временный файл и atomic rename. Это process-lifetime diagnostic worker, нормальный shutdown/lifecycle ещё не реализован.

offscreen-self-test.log:641checks/exit0. offscreen-game.log: успешный presenter setup,14реальных1280×720captures,45секундный внешний timeout124. Self-tests не проверяют качество картинки; оно проверено отдельно просмотром захвата.

work/startup/offscreen-frame.ppm получен через ADB. Перекодирован без изменения пикселей в work/startup/offscreen-frame.png (PNG/zlib, RGB8). Визуально подтверждены логотип Mirror's Edge, город, copyright и Press START button. Это стартовый экран, не доказательство игрового уровня или работающего ввода. Снимок содержит5049уникальных RGBцветов, но именно визуальный просмотр определяет содержание.

Для пользователя сохранена копия /sdcard/Download/me-native-first-frame.png. MediaStore id1000009893, передан ACTION_VIEW для открытия снимка на планшете. Это неподвижный кадр в просмотрщике, не live игра. В APK не добавлялись игровые ресурсы или screenshot asset.

Следующий этап — запуск runtime с Android Activity/Surface, создание live swapchain и lifecycle. Отдельный shell-процесс не может использовать ANativeWindow из другого процесса как обычный pointer; потребуется корректная интеграция, а не передача числового адреса.
