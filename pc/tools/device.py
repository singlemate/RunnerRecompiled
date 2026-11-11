"""Installs the personal APK on an Android device and copies the game files to it, over USB or Wi-Fi.

RunnerBuilder runs this after a build, so that nothing has to be copied to the device by hand:

  python -I -X utf8 -u pc/tools/device.py --apk ready/RunnerRecompiled-pc-personal.apk \
      --game "C:/GOG Games/Mirror's Edge" --home C:/RunnerBuilder [--pair 192.168.1.5:37011 --code 123456] \
      [--connect 192.168.1.5:41061]

adb comes out of Google's platform-tools (only adb.exe and its two DLLs, through remotezip.py).
The app writes the game files itself: it is started with the extra receiveFromPc and listens on a
local socket that only adbd may reach (PcSetup.receive); files that adb pushed into Android/data
would belong to the shell user, and the app may not be able to read them. The protocol is in
PcSetup.java. Files already on the device with the same size are skipped, so a second run resumes.
"""
import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import builder  # noqa: E402
import remotezip  # noqa: E402
from builder import stage, stop, t  # noqa: E402

PACKAGE = "org.mirrorsedge.research"
SOCKET = PACKAGE + ".receive"
GAME_DIRS = ("Binaries", "Engine", "TdGame")
# platform-tools for Windows: URL, SHA-256 of the archive's list of files, the folder inside it.
PLATFORM_TOOLS = (builder.GOOGLE + "platform-tools_r37.0.1-win.zip",
                  "3ecd42407d52e5e1eb9d45650b9f59e16e189cb50d35b7ef9f411af5e5b21c0a", "platform-tools/")
ADB_FILES = r"(adb\.exe|AdbWinApi\.dll|AdbWinUsbApi\.dll)"


def ensure_adb(home, override=None):
    if override:
        return override
    folder = os.path.join(home, "tools", "platform-tools")
    adb = os.path.join(folder, "adb.exe")
    url, listing, inside = PLATFORM_TOOLS
    mark = os.path.join(folder, ".listing-" + (listing or "")[:16])
    if os.path.isfile(adb) and os.path.isfile(mark):
        return adb
    if not listing:
        stop(t("В этой сборке программы не указан adb. Сообщите об ошибке."))
    stage(t("Скачивание adb"))
    partial = folder + ".part"
    builder.shutil.rmtree(partial, ignore_errors=True)

    def choose(path):
        rest = path[len(inside):] if path.startswith(inside) else ""
        return rest if rest and re.fullmatch(ADB_FILES, rest) else None

    remotezip.fetch(url, choose, partial, listing)
    open(os.path.join(partial, os.path.basename(mark)), "w").close()
    builder.shutil.rmtree(folder, ignore_errors=True)
    os.replace(partial, folder)
    return adb


class Adb:
    def __init__(self, path):
        self.path, self.serial = path, None

    def run(self, *args, check=True, timeout=120):
        cmd = [self.path, *(["-s", self.serial] if self.serial else []), *args]
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                           timeout=timeout, creationflags=flags)
        out = (r.stdout + r.stderr).strip()
        if check and r.returncode != 0:
            raise RuntimeError(out or f"adb {' '.join(args)}: {r.returncode}")
        return out

    def devices(self):
        rows = []
        for line in self.run("devices").splitlines()[1:]:
            parts = line.split()
            if len(parts) >= 2:
                rows.append((parts[0], parts[1]))
        return rows


def choose_device(adb, wanted):
    """The serial to work with; stops with advice when there is none or more than one."""
    for _ in range(10):  # a device just plugged in or connected shows up within a few seconds
        rows = adb.devices()
        if wanted:
            rows = [r for r in rows if r[0] == wanted]
        ready = [s for s, state in rows if state == "device"]
        if len(ready) == 1:
            return ready[0]
        if len(ready) > 1:
            stop(t("Подключено несколько устройств: {list}. Оставьте одно.", list=", ".join(ready)))
        time.sleep(1)
    states = {state for _, state in rows}
    if "unauthorized" in states:
        stop(t("Разрешите отладку на устройстве: на его экране появился запрос «Разрешить отладку по USB?». "
               "Отметьте «Всегда разрешать с этого компьютера», нажмите «Разрешить» и запустите установку ещё раз."))
    if "offline" in states:
        stop(t("Устройство видно, но не отвечает. Отключите и снова включите отладку на устройстве и запустите установку ещё раз."))
    stop(t("Устройство не найдено. Включите на нём отладку по USB и подключите кабель, "
           "или включите отладку по Wi-Fi и укажите адрес."))


def install(adb, apk):
    stage(t("Установка приложения на устройство"))
    out = adb.run("install", "-r", apk, check=False, timeout=900)
    print(out, flush=True)
    if "Success" in out:
        return
    if "INSTALL_FAILED_UPDATE_INCOMPATIBLE" in out:
        stop(t("На устройстве стоит приложение, подписанное другим ключом. Удалите его на устройстве "
               "(файлы игры и сохранения внутри приложения тоже удалятся) и запустите установку ещё раз."))
    if "INSTALL_FAILED_INSUFFICIENT_STORAGE" in out:
        stop(t("На устройстве не хватает места для приложения."))
    if "INSTALL_FAILED_USER_RESTRICTED" in out or "INSTALL_FAILED_ABORTED" in out:
        stop(t("Устройство не разрешило установку. На некоторых устройствах нужно включить «Установка через USB» "
               "в настройках для разработчиков или подтвердить установку на экране."))
    stop(t("Не удалось установить приложение: {reason}", reason=out.splitlines()[-1] if out else "?"))


def modified_utf8(s):
    """DataOutputStream.writeUTF: length, then Java's modified UTF-8."""
    b = bytearray()
    for unit in struct.unpack(f"<{len(s.encode('utf-16-le')) // 2}H", s.encode("utf-16-le")):
        if 0 < unit < 0x80:
            b.append(unit)
        elif unit < 0x800:
            b += bytes((0xC0 | unit >> 6, 0x80 | unit & 0x3F))
        else:
            b += bytes((0xE0 | unit >> 12, 0x80 | unit >> 6 & 0x3F, 0x80 | unit & 0x3F))
    if len(b) > 0xFFFF:
        raise ValueError(s)
    return struct.pack(">H", len(b)) + bytes(b)


def game_files(game):
    files = []
    for name in GAME_DIRS:
        for base, _, names in os.walk(os.path.join(game, name)):
            for f in sorted(names):
                path = os.path.join(base, f)
                files.append((path, os.path.relpath(path, game).replace(os.sep, "/"), os.path.getsize(path)))
    return files


class Link:
    def __init__(self, sock):
        self.sock = sock

    def read(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError(t("связь с устройством прервалась"))
            data += chunk
        return data

    def reply(self):
        code = self.read(1)[0]
        if code == 3:
            n = struct.unpack(">H", self.read(2))[0]
            stop(t("Устройство ответило: {reason}", reason=self.read(n).decode("utf-8", "replace")))
        return code


def connect(port, seconds=60):
    """The app needs a few seconds to start and open its socket; until then adbd closes the connection."""
    end = time.monotonic() + seconds
    while True:
        s = None
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=30)
            if Link(s).read(4) == b"RRX1":
                s.settimeout(120)
                return s
        except (OSError, ConnectionError):
            pass
        if s:
            s.close()
        if time.monotonic() > end:
            stop(t("Приложение на устройстве не ответило. Разблокируйте устройство, не закрывайте приложение и запустите установку ещё раз."))
        time.sleep(1)


def send_game(adb, game, test=False):
    stage(t("Копирование игры на устройство"))
    files = game_files(game)
    total = sum(size for _, _, size in files)
    # --test-transfer: the app takes the files into a folder of its own and deletes it after counting them.
    adb.run("shell", "am", "start", "-S", "-n", PACKAGE + "/.PcLauncherActivity", "--ez", "receiveFromPc", "true",
            *(["--ez", "receiveTest", "true"] if test else []))
    port = int(adb.run("forward", "tcp:0", "localabstract:" + SOCKET).split()[-1])
    try:
        sock = connect(port)
        link = Link(sock)
        sock.sendall(b"RRX1" + b"T" + struct.pack(">q", total))
        done = shown = 0
        started = time.monotonic()
        for path, rel, size in files:
            sock.sendall(b"F" + modified_utf8(rel) + struct.pack(">q", size))
            if link.reply() == 1:
                with open(path, "rb") as f:
                    left = size
                    while left:
                        chunk = f.read(min(1 << 20, left))
                        if not chunk:
                            stop(t("Файл игры изменился во время копирования: {name}", name=rel))
                        sock.sendall(chunk)
                        left -= len(chunk)
                        done += len(chunk)
                        percent = done * 100 // max(total, 1)
                        if percent >= shown + 5:
                            shown = percent - percent % 5
                            speed = done / max(time.monotonic() - started, 0.1) / 2 ** 20
                            print(t("  копирование, {speed} МБ/с: {p}%", p=shown, speed=f"{speed:.0f}"), flush=True)
                link.reply()
            else:
                done += size
        stage(t("Проверка файлов на устройстве"))
        sock.sendall(b"E")
        link.reply()
        sock.close()
    finally:
        adb.run("forward", "--remove", f"tcp:{port}", check=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apk", required=True, help="личный APK")
    ap.add_argument("--game", required=True, help="папка установленной игры из GOG")
    ap.add_argument("--home", required=True, help="папка инструментов RunnerBuilder")
    ap.add_argument("--pair", help="адрес и порт сопряжения (отладка по Wi-Fi), вместе с --code")
    ap.add_argument("--code", help="код сопряжения")
    ap.add_argument("--connect", help="адрес и порт устройства (отладка по Wi-Fi)")
    ap.add_argument("--serial", help="устройство из adb devices")
    ap.add_argument("--adb", help="свой adb вместо скачанного")
    ap.add_argument("--skip-install", action="store_true", help="только скопировать игру")
    ap.add_argument("--test-transfer", action="store_true", help="проверка: передать всё во временную папку и удалить")
    ap.add_argument("--lang", default="ru")
    args = ap.parse_args()
    builder.set_language(args.lang)
    builder.check_game(os.path.abspath(args.game))
    adb = Adb(ensure_adb(os.path.abspath(args.home), args.adb))
    stage(t("Поиск устройства"))
    adb.run("start-server", timeout=60)
    serial = args.serial
    if args.pair:
        if not args.code:
            stop(t("Для сопряжения нужен код с экрана устройства."))
        out = adb.run("pair", args.pair.strip(), args.code.strip(), check=False, timeout=60)
        print(out, flush=True)
        if "Successfully paired" not in out:
            stop(t("Сопряжение не удалось: проверьте адрес, порт и код. Код действует, пока открыто окно сопряжения на устройстве."))
    if args.connect:
        out = adb.run("connect", args.connect.strip(), check=False, timeout=60)
        print(out, flush=True)
        if "connected to" not in out:
            stop(t("Не удалось подключиться к {address}. Устройство и компьютер должны быть в одной сети Wi-Fi, "
                   "а адрес и порт — с экрана «Отладка по Wi-Fi» (порт меняется при каждом включении).", address=args.connect))
        serial = serial or args.connect.strip()
    adb.serial = choose_device(adb, serial)
    print(f"device {adb.serial}", flush=True)
    if not args.skip_install:
        install(adb, os.path.abspath(args.apk))
    send_game(adb, os.path.abspath(args.game), args.test_transfer)
    stage(t("Готово: откройте приложение на устройстве"))
    print("INSTALLED", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as e:
        stop(str(e))
