"""Qt-facing state object for the Smart Sentry dashboard.

One SentryBackend instance is exposed to QML as `sentry`. Drivers run their
asyncio loop on a worker thread and push state via post(), which crosses to
the Qt main thread through a queued signal; QML reads the properties and
calls the slots.
"""
from datetime import datetime

from PySide6.QtCore import QObject, Property, QTimer, Signal, Slot

CONN_STATES = ("scanning", "connecting", "authenticating", "secure")


class SentryBackend(QObject):
    connStateChanged = Signal()
    reconnectingChanged = Signal()
    tempChanged = Signal()
    humidityChanged = Signal()
    ledChanged = Signal()
    lastUpdateChanged = Signal()
    uptimeChanged = Signal()
    signalChanged = Signal()
    dataArrived = Signal()  # fires on every fresh sensor value (drives glow pulse)

    _post = Signal(str, object)  # thread crossing: driver thread -> Qt thread

    def __init__(self, parent=None):
        super().__init__(parent)
        self._conn_state = "scanning"
        self._reconnecting = False
        self._temp = 23.51
        self._humidity = 43.20
        self._led_on = False
        self._last_update = "--:--:--"
        self._uptime_sec = 0
        self._signal = 0
        self.driver = None  # assigned by main.py

        self._uptime_timer = QTimer(self)
        self._uptime_timer.setInterval(1000)
        self._uptime_timer.timeout.connect(self._tick_uptime)
        self._uptime_timer.start()

        self._post.connect(self._apply)  # queued when emitted off-thread

    # ---- driver-facing API (safe from any thread) -------------------------
    def post(self, key: str, value=None):
        """Update state from the driver thread: post("temp", 23.51) etc.
        Keys map to the set_<key> methods below."""
        self._post.emit(key, value)

    @Slot(str, object)
    def _apply(self, key, value):
        getattr(self, "set_" + key)(value)

    # ---- setters (Qt main thread only; use post() from drivers) -----------
    def set_conn_state(self, state: str):
        assert state in CONN_STATES, state
        if state == "secure" and self._conn_state != "secure":
            self._uptime_sec = 0
            self.uptimeChanged.emit()
        if self._conn_state != state:
            self._conn_state = state
            self.connStateChanged.emit()

    def set_reconnecting(self, flag: bool):
        if self._reconnecting != flag:
            self._reconnecting = flag
            self.reconnectingChanged.emit()

    def set_temp(self, value: float):
        self._temp = value
        self.tempChanged.emit()
        self._touch()

    def set_humidity(self, value: float):
        self._humidity = value
        self.humidityChanged.emit()
        self._touch()

    def set_led(self, on: bool):
        if self._led_on != on:
            self._led_on = on
            self.ledChanged.emit()

    def set_signal(self, level: int):
        level = max(0, min(4, level))
        if self._signal != level:
            self._signal = level
            self.signalChanged.emit()

    def _touch(self):
        self._last_update = datetime.now().strftime("%H:%M:%S")
        self.lastUpdateChanged.emit()
        self.dataArrived.emit()

    def _tick_uptime(self):
        if self._conn_state == "secure" and not self._reconnecting:
            self._uptime_sec += 1
            self.uptimeChanged.emit()

    # ---- QML-facing slots -------------------------------------------------
    @Slot()
    def toggleLed(self):
        if self.driver:
            self.driver.toggle_led()

    @Slot()
    def reconnect(self):
        if self.driver:
            self.driver.reconnect()

    @Slot()
    def cycleConnState(self):
        """Demo helper (status pill tap): only the fake driver honours it."""
        if self.driver and hasattr(self.driver, "cycle_conn_state"):
            self.driver.cycle_conn_state()

    # ---- QML-facing properties -------------------------------------------
    @Property(str, notify=connStateChanged)
    def connState(self):
        return self._conn_state

    @Property(bool, notify=reconnectingChanged)
    def reconnecting(self):
        return self._reconnecting

    @Property(float, notify=tempChanged)
    def temp(self):
        return self._temp

    @Property(float, notify=humidityChanged)
    def humidity(self):
        return self._humidity

    @Property(bool, notify=ledChanged)
    def ledOn(self):
        return self._led_on

    @Property(str, notify=lastUpdateChanged)
    def lastUpdate(self):
        return self._last_update

    @Property(str, notify=uptimeChanged)
    def uptime(self):
        h, rem = divmod(self._uptime_sec, 3600)
        m, s = divmod(rem, 60)
        return f"{h:02d}:{m:02d}:{s:02d}"

    @Property(int, notify=signalChanged)
    def signalLevel(self):
        return self._signal
