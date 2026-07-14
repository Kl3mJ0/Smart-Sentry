"""Smart Sentry dashboard - native Qt Quick app.

  python main.py                # fake data on Windows, real BLE on Linux/Pi
  python main.py --driver fake  # force simulated data
  python main.py --driver ble   # force real SS1 connection
  python main.py --fullscreen   # kiosk mode (touchscreen)
  python main.py --screenshot out.png   # debug: grab frame after 3s, quit

Architecture: Qt runs its normal event loop on the main thread; the driver
(fake or BLE) runs an asyncio loop on a daemon worker thread. State flows
driver -> backend.post() -> queued Qt signal -> QML. UI actions flow
QML slot -> driver.toggle_led()/reconnect() -> run_coroutine_threadsafe.
No qasync: it breaks with newer Qt/Python (timer assertions on Pi OS).
"""
import argparse
import asyncio
import sys
import threading
from pathlib import Path

from PySide6.QtCore import QTimer, QUrl
from PySide6.QtGui import QFontDatabase, QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
import PySide6.QtQuick  # noqa: F401  (preloads Qt6Quick.dll so the QML plugin resolves on Windows)

from backend import SentryBackend

APP_DIR = Path(__file__).resolve().parent


def pick_font(candidates, fallback):
    families = set(QFontDatabase.families())
    for name in candidates:
        if name in families:
            return name
    return fallback


def start_driver_thread(driver):
    """Run driver.run() on its own asyncio loop in a daemon thread."""
    ready = threading.Event()

    def runner():
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        driver.loop = loop
        ready.set()
        try:
            loop.run_until_complete(driver.run())
        except Exception as e:
            print(f"[driver] crashed: {e!r}")

    t = threading.Thread(target=runner, name="sentry-driver", daemon=True)
    t.start()
    ready.wait()
    return t


def main():
    parser = argparse.ArgumentParser(description="Smart Sentry dashboard")
    parser.add_argument("--driver", choices=("fake", "ble"),
                        default="ble" if sys.platform.startswith("linux") else "fake")
    parser.add_argument("--fullscreen", action="store_true")
    parser.add_argument("--screenshot", metavar="PATH", help="debug: save a frame and exit")
    args = parser.parse_args()

    app = QGuiApplication(sys.argv)
    app.setApplicationName("Smart Sentry")

    backend = SentryBackend()
    if args.driver == "ble":
        from ble_driver import BleDriver
        driver = BleDriver(backend)
    else:
        from fake_driver import FakeDriver
        driver = FakeDriver(backend)
    backend.driver = driver

    engine = QQmlApplicationEngine()
    ctx = engine.rootContext()
    ctx.setContextProperty("sentry", backend)
    ctx.setContextProperty("startFullscreen", args.fullscreen)
    ctx.setContextProperty("isFakeDriver", args.driver == "fake")
    ctx.setContextProperty("uiFont", pick_font(
        ["Space Grotesk", "Segoe UI", "DejaVu Sans"], "sans-serif"))
    ctx.setContextProperty("monoFont", pick_font(
        ["JetBrains Mono", "Cascadia Mono", "Consolas", "DejaVu Sans Mono"], "monospace"))

    engine.load(QUrl.fromLocalFile(str(APP_DIR / "qml" / "Main.qml")))
    if not engine.rootObjects():
        sys.exit("QML failed to load")
    window = engine.rootObjects()[0]

    if args.screenshot:
        def grab():
            img = window.grabWindow()
            ok = img.save(args.screenshot)
            print(f"screenshot {'saved to ' + args.screenshot if ok else 'FAILED'}")
            app.quit()
        QTimer.singleShot(3000, grab)

    start_driver_thread(driver)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
