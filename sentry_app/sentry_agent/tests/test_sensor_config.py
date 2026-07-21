import asyncio
import struct
import sys
import unittest
from pathlib import Path

AGENT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AGENT_DIR))

from session import DeviceSession, INTERVAL_UUID, MODE_UUID, STATUS_UUID


class DummyClient:
    is_connected = True

    def __init__(self):
        self.writes = []

    async def read_gatt_char(self, uuid):
        if uuid == MODE_UUID:
            return bytes([2])
        if uuid == INTERVAL_UUID:
            return bytes([0])
        if uuid == STATUS_UUID:
            return struct.pack("<BBBBHH", 2, 4, 0x01, 5, 500, 0)
        raise AssertionError(uuid)

    async def write_gatt_char(self, uuid, value, response):
        self.writes.append((uuid, bytes(value), response))


class SensorConfigurationTests(unittest.IsolatedAsyncioTestCase):
    def make_session(self):
        self.events = {}
        return DeviceSession(
            "device", "address", "SS1",
            lambda _device, key, value: self.events.__setitem__(key, value),
            asyncio.Lock(),
        )

    async def test_decodes_thermistor_capabilities_without_fake_humidity(self):
        session = self.make_session()
        await session._read_configuration(DummyClient())
        self.assertEqual(self.events["sensor_mode_name"], "External Auto")
        self.assertEqual(self.events["sensor_kind_name"], "10k B3950 Thermistor")
        self.assertTrue(self.events["temp_available"])
        self.assertFalse(self.events["humidity_available"])
        self.assertEqual(self.events["sample_interval_ms"], 500)

    async def test_interval_write_is_range_checked(self):
        session = self.make_session()
        client = DummyClient()
        session.client = client
        await session.set_interval(30)
        self.assertEqual(client.writes[-1], (INTERVAL_UUID, bytes([30]), True))
        with self.assertRaises(ValueError):
            await session.set_interval(31)


if __name__ == "__main__":
    unittest.main()
