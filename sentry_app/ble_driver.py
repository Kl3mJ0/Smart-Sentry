"""Real SS1 driver for the Pi: BLE connect + cert handshake + live sensors.

Ports the proven logic from PI_CLIENT_HANDOFF.md / sentry_client.py into the
dashboard's driver interface, with an endless reconnect loop so the app
recovers from drops on its own (the RECONNECTING overlay shows meanwhile).

run() executes on the driver thread's asyncio loop (main.py starts it) —
bleak and its D-Bus machinery stay entirely on that thread. UI-triggered
actions (toggle_led/reconnect) are called on the Qt thread and hop over via
run_coroutine_threadsafe.

Requires (Pi only): pip install bleak cryptography
"""
import asyncio
import struct

from bleak import BleakClient, BleakScanner
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

from creds import CLIENT_CERT, CLIENT_KEY_PEM, CERT_LEN, NONCE_LEN, SIG_LEN

TARGET_NAME = "Joseph_BLE"

AUTH_CHALLENGE_UUID = "7e8f00a1-1111-2222-3333-123456789abc"
AUTH_RESPONSE_UUID = "7e8f00a2-1111-2222-3333-123456789abc"
TEMP_UUID = "00002a6e-0000-1000-8000-00805f9b34fb"
HUM_UUID = "00002a6f-0000-1000-8000-00805f9b34fb"
# Sentry service LED control characteristic. VERIFY ON FIRST PI RUN: the
# handoff lists the char as 7e8f0002-...; suffix assumed to match the auth
# service pattern. If the LED toggle no-ops, dump services with
# `bluetoothctl` or bleak's get_services() and correct this UUID.
LED_UUID = "7e8f0002-1111-2222-3333-123456789abc"

RETRY_DELAY_S = 2.0


def _sign_nonce(nonce: bytes) -> bytes:
    """ECDSA P-256/SHA-256, RAW r||s (64 B) - SS1 rejects DER."""
    key = serialization.load_pem_private_key(CLIENT_KEY_PEM, password=None)
    der = key.sign(nonce, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


class BleDriver:
    def __init__(self, backend):
        self.backend = backend
        self.loop = None  # asyncio loop on the driver thread, set by main.py
        self._client = None
        self._led_lock = asyncio.Lock()

    async def run(self):
        """Endless connect -> auth -> stream loop; never raises."""
        b = self.backend
        first = True
        while True:
            if not first:
                b.post("reconnecting", True)
            try:
                await self._session()
            except Exception as e:
                print(f"[ble] session ended: {e!r} - retrying in {RETRY_DELAY_S}s")
            self._client = None
            b.post("signal", 0)
            b.post("conn_state", "scanning")
            first = False
            await asyncio.sleep(RETRY_DELAY_S)

    async def _session(self):
        b = self.backend

        b.post("conn_state", "scanning")
        device = await BleakScanner.find_device_by_name(TARGET_NAME, timeout=30.0)
        if device is None:
            raise TimeoutError(f"{TARGET_NAME} not found")

        b.post("conn_state", "connecting")
        async with BleakClient(device, timeout=30.0) as client:
            self._client = client
            # Encrypted link required; pair() failures are non-fatal, the
            # encrypted read below triggers pairing implicitly (see handoff).
            try:
                await client.pair()
            except Exception as e:
                print(f"[ble] pair() note: {e} - continuing")

            b.post("conn_state", "authenticating")
            nonce = bytes(await client.read_gatt_char(AUTH_CHALLENGE_UUID))
            if len(nonce) != NONCE_LEN:
                raise ValueError(f"unexpected nonce length {len(nonce)}")
            response = CLIENT_CERT + _sign_nonce(nonce)
            assert len(response) == CERT_LEN + SIG_LEN
            # SS1 disconnects on failed verify; clean write means accepted.
            await client.write_gatt_char(AUTH_RESPONSE_UUID, response, response=True)

            b.post("conn_state", "secure")
            b.post("reconnecting", False)
            b.post("signal", 4)  # TODO: map live RSSI to 1-4 bars

            await client.start_notify(TEMP_UUID, self._on_temp)
            await client.start_notify(HUM_UUID, self._on_hum)

            try:
                led = await client.read_gatt_char(LED_UUID)
                if led:
                    b.post("led", bool(led[0]))
            except Exception as e:
                print(f"[ble] LED read note: {e}")

            while client.is_connected:
                await asyncio.sleep(1.0)
        raise ConnectionError("disconnected")

    def _on_temp(self, _char, data: bytearray):
        (v,) = struct.unpack("<h", data[:2])
        self.backend.post("temp", v / 100.0)

    def _on_hum(self, _char, data: bytearray):
        (v,) = struct.unpack("<H", data[:2])
        self.backend.post("humidity", v / 100.0)

    # ---- called on the Qt thread via backend slots -------------------------
    def toggle_led(self):
        asyncio.run_coroutine_threadsafe(self._do_toggle_led(), self.loop)

    async def _do_toggle_led(self):
        client, b = self._client, self.backend
        if not (client and client.is_connected and b.connState == "secure"):
            return
        async with self._led_lock:
            target = not b.ledOn
            try:
                await client.write_gatt_char(LED_UUID, bytes([1 if target else 0]), response=True)
                b.post("led", target)
            except Exception as e:
                print(f"[ble] LED write failed: {e}")

    def reconnect(self):
        """Force-drop the session; run() loops back through scanning."""
        asyncio.run_coroutine_threadsafe(self._do_reconnect(), self.loop)

    async def _do_reconnect(self):
        client = self._client
        if client and client.is_connected:
            self.backend.post("reconnecting", True)
            await client.disconnect()
