"""Per-device BLE session: connect, pair, cert auth, subscribe to sensors,
LED control - one instance per discovered SS1, run by FleetManager.

This is ble_driver.py's proven session logic (see PI_CLIENT_HANDOFF.md),
generalized so it's keyed by an explicit device/address instead of "find the
first thing named Joseph_BLE" - that assumption is exactly the fleet blocker
this package exists to remove.
"""
import asyncio
import struct
from bleak import BleakClient
from auth import authenticate
TEMP_UUID = "00002a6e-0000-1000-8000-00805f9b34fb"
HUM_UUID = "00002a6f-0000-1000-8000-00805f9b34fb"
LED_UUID = "7e8f0002-1111-2222-3333-123456789abc"
FW_VERSION_UUID = "00002a26-0000-1000-8000-00805f9b34fb"

RETRY_DELAY_S = 2.0
MAX_RETRY_DELAY_S = 20.0
# Doubles the delay after each consecutive failed attempt (capped at
# MAX_RETRY_DELAY_S), reset to RETRY_DELAY_S on any successful connection.
# Without this, a device that's mid-reboot/reflash gets hit with a fresh
# connect attempt every 2s indefinitely - visible as connect/disconnect
# churn on its own serial log (observed 2026-07-15 during a firmware
# downgrade).


class DeviceSession:
    """Owns one SS1 BLE connection end-to-end.

    `on_state(device_id, key, value)` is called on every update so
    FleetManager can fan it out to the inventory + IPC layer - mirrors
    SentryBackend.post()'s dispatch shape from the dashboard so the two stay
    easy to reconcile.
    """

    def __init__(self, device_id: str, address: str, name: str, on_state, discovery_lock: asyncio.Lock):
        self.device_id = device_id
        self.address = address
        self.name = name
        self.on_state = on_state
        # Serializes this session's connect() against FleetManager's own
        # discover() and any in-flight OtaClient connect - BlueZ runs one
        # discovery/connect operation adapter-wide, and connect() itself
        # (not the ongoing connection) is the part that collides. Observed
        # 2026-07-15: without this, DeviceSession's own reconnects hit
        # `org.bluez.Error.InProgress` and `GATT Protocol Error: Unlikely
        # Error` almost every time, since the fleet's discovery scan runs
        # ~86% of the time (DISCOVERY_SCAN_S=30 of every ~35s cycle).
        self.discovery_lock = discovery_lock
        self.client: BleakClient | None = None
        self.led_state = False
        self.led_lock = asyncio.Lock()
        # Set by FleetManager to keep this session parked (disconnected, not
        # retrying) while an OTA job owns the adapter for this device.
        self.paused = False
        self._retry_delay = RETRY_DELAY_S

    def _post(self, key, value):
        if key == "led":
            self.led_state = value
        self.on_state(self.device_id, key, value)

    async def run(self):
        """Endless connect -> auth -> stream loop for this one device; never raises."""
        first = True
        while True:
            if self.paused:
                await asyncio.sleep(RETRY_DELAY_S)
                continue
            if not first:
                self._post("reconnecting", True)
            try:
                await self._session()
            except Exception as e:
                print(f"[{self.name}/{self.address}] session ended: {e!r} - retrying in {self._retry_delay}s")
                self._retry_delay = min(self._retry_delay * 2, MAX_RETRY_DELAY_S)
            self.client = None
            self._post("signal", 0)
            self._post("conn_state", "scanning")
            first = False
            await asyncio.sleep(self._retry_delay)

    async def _session(self):
        self._post("conn_state", "connecting")
        client = BleakClient(self.address, timeout=30.0)
        async with self.discovery_lock:
            await client.connect()
        try:
            self.client = client
            self._post("conn_state", "authenticating")
            await authenticate(client)

            fw_version = bytes(await client.read_gatt_char(FW_VERSION_UUID)).decode("ascii")
            self._post("fw_version", fw_version)

            self._post("conn_state", "secure")
            self._post("reconnecting", False)
            self._post("signal", 4)  # TODO: map live RSSI to 1-4 bars
            self._retry_delay = RETRY_DELAY_S  # back off only compounds across consecutive failures

            await client.start_notify(TEMP_UUID, self._on_temp)
            await client.start_notify(HUM_UUID, self._on_hum)

            try:
                led = await client.read_gatt_char(LED_UUID)
                if led:
                    self._post("led", bool(led[0]))
            except Exception as e:
                print(f"[{self.name}] LED read note: {e}")

            while client.is_connected and not self.paused:
                await asyncio.sleep(1.0)
        finally:
            await client.disconnect()
        raise ConnectionError("disconnected")

    def _on_temp(self, _char, data: bytearray):
        (v,) = struct.unpack("<h", data[:2])
        self._post("temp", v / 100.0)

    def _on_hum(self, _char, data: bytearray):
        (v,) = struct.unpack("<H", data[:2])
        self._post("humidity", v / 100.0)

    # ---- commands from IPC clients (already on the agent's asyncio loop) --
    def toggle_led(self):
        asyncio.create_task(self._set_led(not self.led_state))

    async def _set_led(self, target: bool):
        client = self.client
        if not (client and client.is_connected):
            return
        async with self.led_lock:
            try:
                await client.write_gatt_char(LED_UUID, bytes([1 if target else 0]), response=True)
                self._post("led", target)
            except Exception as e:
                print(f"[{self.name}] LED write failed: {e}")

    def reconnect(self):
        asyncio.create_task(self._do_reconnect())

    async def _do_reconnect(self):
        client = self.client
        if client and client.is_connected:
            self._post("reconnecting", True)
            await client.disconnect()

    # ---- OTA hand-off: FleetManager calls these around an OTA job ---------
    async def pause_for_ota(self):
        """Disconnect and stop reconnect attempts so an OtaClient can take
        the address exclusively (SS1 accepts one GATT connection at a time)."""
        self.paused = True
        client = self.client
        if client and client.is_connected:
            await client.disconnect()

    def resume_after_ota(self):
        self.paused = False
