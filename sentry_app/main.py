"""Smart Sentry dashboard - native Qt Quick app.

  python main.py                # fake data on Windows, real BLE on Linux/Pi
  python main.py --driver fake  # force simulated data
  python main.py --driver ble   # force real SS1 connection
  python main.py --fullscreen   # kiosk mode (touchscreen)
  python main.py --screenshot out.png   # debug: grab frame after 3s, quit
"""
import argparse
import asyncio
import sys
from pathlib import Path

from PySide6.QtCore import QTimer, QUrl
from PySide6.QtGui import QFontDatabase, QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
import PySide6.QtQuick  # noqa: F401  (preloads Qt6Quick.dll so the QML plugin resolves on Windows)

# Must come after the PySide6 imports above: qasync picks its Qt binding by
# checking sys.modules, and defaults to PyQt5 (if installed) when nothing
# Qt-related has been imported yet. Importing PySide6 first - or importing
# qasync while nothing else is present - makes qasync silently wrap the
# wrong binding, which desyncs it from the running QGuiApplication: its
# QSocketNotifier/QTimer never get serviced, so no scheduled asyncio task
# (driver.run(), sensor updates, LED toggle) ever executes.
import qasync

from backend import SentryBackend

APP_DIR = Path(__file__).resolve().parent


def pick_font(candidates, fallback):
    families = set(QFontDatabase.families())
    for name in candidates:
        if name in families:
            return name
    return fallback


def main():
    parser = argparse.ArgumentParser(description="Smart Sentry dashboard")
    parser.add_argument("--driver", choices=("fake", "ble", "agent"),
                        default="ble" if sys.platform.startswith("linux") else "fake")
    parser.add_argument("--fullscreen", action="store_true")
    parser.add_argument("--screenshot", metavar="PATH", help="debug: save a frame and exit")
    args = parser.parse_args()

    app = QGuiApplication(sys.argv)
    app.setApplicationName("Smart Sentry")
    loop = qasync.QEventLoop(app)
    asyncio.set_event_loop(loop)

    backend = SentryBackend()
    if args.driver == "ble":
        from ble_driver import BleDriver
        driver = BleDriver(backend)
    elif args.driver == "agent":
        from agent_driver import AgentDriver
        driver = AgentDriver(backend)
    else:
        from fake_driver import FakeDriver
        driver = FakeDriver(backend)
    backend.driver = driver
    driver.loop = loop

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

    loop.create_task(driver.run())
    with loop:
        loop.run_forever()


if __name__ == "__main__":
    main()
