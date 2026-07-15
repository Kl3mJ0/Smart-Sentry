"""Discovers SS1 devices and runs one DeviceSession per device instead of
the dashboard's single global connection - the #1 fleet blocker GPT flagged.

Every SS1 currently advertises the same name ("Joseph_BLE"), so today
"discovery" really means "one Joseph_BLE happened to be in range"; multiple
real units would collide here until they advertise distinguishable
names/IDs (firmware TODO). BLE address is used as the interim per-device key
(see inventory.resolve_device_id).
"""
import asyncio

from bleak import BleakScanner

from inventory import Inventory, resolve_device_id
from session import DeviceSession

TARGET_NAME_PREFIX = "Joseph_BLE"
DISCOVERY_INTERVAL_S = 5.0
# ble_driver.py's proven scanner call uses timeout=30s (find_device_by_name) -
# SS1's advertising cadence needs that much room; a shorter window measurably
# misses it (verified 2026-07-14: a 12s bare BleakScanner.discover() and even
# a fresh 30s find_device_by_name() both missed a unit that bluetoothctl's
# already-cached discovery still listed as known).
DISCOVERY_SCAN_S = 30.0


class FleetManager:
    def __init__(self, inventory: Inventory | None = None):
        self.inventory = inventory or Inventory()
        self.sessions: dict[str, DeviceSession] = {}
        self._tasks: dict[str, asyncio.Task] = {}
        self.state: dict[str, dict] = {}
        self._state_listeners = []
        # BlueZ only runs one discovery session adapter-wide; an OTA client's
        # own connect-by-address scan (see ota.py/SMPBLETransport) collides
        # with this loop's discover() otherwise (observed 2026-07-14:
        # `org.bluez.Error.InProgress`). agent_main's ota_worker holds this
        # for the whole OTA job so the two never overlap.
        self.discovery_lock = asyncio.Lock()

    def add_state_listener(self, fn):
        """fn(device_id, key, value) - called on every state update, e.g. by the IPC server."""
        self._state_listeners.append(fn)

    def remove_state_listener(self, fn):
        self._state_listeners.remove(fn)

    def _on_state(self, device_id, key, value):
        self.state.setdefault(device_id, {})[key] = value
        if key == "conn_state":
            self.inventory.set_status(device_id, value)
        for fn in self._state_listeners:
            fn(device_id, key, value)

    async def run(self):
        """Continuous discovery loop; spawns a DeviceSession for each new device.

        Scanning while other DeviceSessions hold active GATT connections is
        supported by BlueZ but can add latency on constrained adapters; 15s
        between 5s scans is a conservative starting point, not a tuned value.
        """
        while True:
            try:
                async with self.discovery_lock:
                    devices = await BleakScanner.discover(timeout=DISCOVERY_SCAN_S)
            except Exception as e:
                print(f"[fleet] scan failed: {e!r}")
                devices = []
            for d in devices:
                name = d.name or ""
                if not name.startswith(TARGET_NAME_PREFIX):
                    continue
                device_id = resolve_device_id(d.address, name)
                self.inventory.upsert_device(d.address, name, status=self.state.get(device_id, {}).get("conn_state", "discovered"))
                if device_id not in self.sessions:
                    self._spawn_session(device_id, d.address, name)
            await asyncio.sleep(DISCOVERY_INTERVAL_S)

    def _spawn_session(self, device_id, address, name):
        session = DeviceSession(device_id, address, name, self._on_state, self.discovery_lock)
        self.sessions[device_id] = session
        self._tasks[device_id] = asyncio.create_task(session.run())
        print(f"[fleet] tracking {name} ({address}) as device_id={device_id}")

    def toggle_led(self, device_id: str):
        session = self.sessions.get(device_id)
        if session:
            session.toggle_led()

    def reconnect(self, device_id: str):
        session = self.sessions.get(device_id)
        if session:
            session.reconnect()

    def list_devices(self) -> list[dict]:
        out = []
        for device_id, session in self.sessions.items():
            entry = {"device_id": device_id, "address": session.address, "name": session.name}
            entry.update(self.state.get(device_id, {}))
            out.append(entry)
        return out

    # ---- OTA hand-off -------------------------------------------------
    async def pause_for_ota(self, device_id: str) -> bool:
        session = self.sessions.get(device_id)
        if not session:
            return False
        await session.pause_for_ota()
        return True

    def resume_after_ota(self, device_id: str):
        session = self.sessions.get(device_id)
        if session:
            session.resume_after_ota()
