"""Qt-facing state object for the Smart Sentry dashboard.

One SentryBackend instance is exposed to QML as `sentry`. Drivers (fake or
BLE) push state into it via the set_* methods; QML reads the properties and
calls the slots. All calls happen on the single qasync event loop thread.
"""
from datetime import datetime

from PySide6.QtCore import QObject, Property, QTimer, Signal, Slot

CONN_STATES = ("scanning", "connecting", "authenticating", "secure")
ACTIVE_UPDATE_STATES = ("running", "uploading", "trial_pending", "checking_health", "confirming")
PENDING_UPDATE_STATES = ("queued",) + ACTIVE_UPDATE_STATES


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
    deviceChanged = Signal()
    updateChanged = Signal()
    jobsChanged = Signal()

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
        self._device_id = ""
        self._device_name = "No SS1 discovered"
        self._device_count = 0
        self._device_position = 0
        self._fw_version = "unknown"
        self._update_status = "idle"
        self._update_message = "Waiting for startup update check"
        self._latest_version = ""
        self._jobs = []
        self.driver = None  # assigned by main.py

        self._uptime_timer = QTimer(self)
        self._uptime_timer.setInterval(1000)
        self._uptime_timer.timeout.connect(self._tick_uptime)
        self._uptime_timer.start()

    # ---- driver-facing setters -------------------------------------------
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

    def set_device(self, device_id: str, name: str, position: int, count: int, fw_version: str = "unknown"):
        self._device_id, self._device_name = device_id, name
        self._device_position, self._device_count = position, count
        self._fw_version = fw_version or "unknown"
        self.deviceChanged.emit()

    def set_update_status(self, status: str, message: str, latest_version=None):
        self._update_status = status or "idle"
        self._update_message = message or ""
        self._latest_version = latest_version or ""
        self.updateChanged.emit()

    def set_jobs(self, jobs):
        self._jobs = jobs or []
        self.jobsChanged.emit()
        self.updateChanged.emit()

    def post(self, key: str, value):
        """Generic setter dispatch used by ble_driver.py (state name -> set_<name>)."""
        getattr(self, f"set_{key}")(value)

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
    def nextDevice(self):
        if self.driver and hasattr(self.driver, "next_device"):
            self.driver.next_device()

    @Slot()
    def previousDevice(self):
        if self.driver and hasattr(self.driver, "previous_device"):
            self.driver.previous_device()

    @Slot()
    def checkUpdates(self):
        if self.driver and hasattr(self.driver, "check_updates"):
            self.driver.check_updates()

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

    @Property(str, notify=deviceChanged)
    def deviceId(self): return self._device_id

    @Property(str, notify=deviceChanged)
    def deviceName(self): return self._device_name

    @Property(int, notify=deviceChanged)
    def deviceCount(self): return self._device_count

    @Property(int, notify=deviceChanged)
    def devicePosition(self): return self._device_position

    @Property(str, notify=deviceChanged)
    def fwVersion(self): return self._fw_version

    @Property(str, notify=updateChanged)
    def updateStatus(self): return self._update_status

    @Property(str, notify=updateChanged)
    def updateMessage(self): return self._update_message

    @Property(str, notify=updateChanged)
    def latestVersion(self): return self._latest_version

    @Property(int, notify=updateChanged)
    def updateProgress(self):
        active = next((j for j in self._jobs if j.get("state") in ACTIVE_UPDATE_STATES), None)
        return int(active.get("progress", 0)) if active else 0

    @Property(int, notify=jobsChanged)
    def pendingJobCount(self):
        return sum(1 for job in self._jobs if job.get("state") in PENDING_UPDATE_STATES)

    @Property(str, notify=jobsChanged)
    def activeUpdateDevice(self):
        active = next((j for j in self._jobs if j.get("state") in ACTIVE_UPDATE_STATES), None)
        return (active or {}).get("device_name") or (active or {}).get("device_id", "")

    @Property(str, notify=jobsChanged)
    def activeUpdateState(self):
        active = next((j for j in self._jobs if j.get("state") in ACTIVE_UPDATE_STATES), None)
        return (active or {}).get("state", "")

    @Property("QVariantList", notify=jobsChanged)
    def jobs(self): return self._jobs
