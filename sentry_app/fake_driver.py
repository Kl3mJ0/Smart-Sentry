"""Simulated SS1 for developing the UI without Bluetooth hardware.

Reproduces the demo behaviour of the approved design: values random-walk
every 2 s, the status pill can be cycled by tapping it, and Reconnect plays
the scanning -> connecting -> authenticating -> secure sequence.
"""
import asyncio
import random

TEMP_MIN, TEMP_MAX = 15.0, 35.0
HUM_MIN, HUM_MAX = 0.0, 100.0
CONN_ORDER = ("scanning", "connecting", "authenticating", "secure")


def _clamp(v, lo, hi):
    return max(lo, min(hi, v))


class FakeDriver:
    def __init__(self, backend):
        self.backend = backend
        self._temp = 23.51
        self._hum = 43.20
        self._sequencing = False  # a connect sequence is in flight

    async def run(self):
        b = self.backend
        await self._connect_sequence()
        b.set_temp(self._temp)
        b.set_humidity(self._hum)
        while True:
            await asyncio.sleep(2.0)
            if b.connState != "secure" or b.reconnecting:
                continue
            self._temp = _clamp(self._temp + random.uniform(-0.3, 0.3), TEMP_MIN, TEMP_MAX)
            self._hum = _clamp(self._hum + random.uniform(-0.7, 0.7), HUM_MIN, HUM_MAX)
            b.set_temp(self._temp)
            b.set_humidity(self._hum)

    async def _connect_sequence(self):
        b = self.backend
        self._sequencing = True
        try:
            b.set_signal(0)
            for state in ("scanning", "connecting", "authenticating"):
                b.set_conn_state(state)
                await asyncio.sleep(0.9)
            b.set_conn_state("secure")
            b.set_signal(random.randint(3, 4))
        finally:
            self._sequencing = False

    # ---- called from QML via backend slots --------------------------------
    def toggle_led(self):
        self.backend.set_led(not self.backend.ledOn)

    def reconnect(self):
        asyncio.get_event_loop().create_task(self._do_reconnect())

    def cycle_conn_state(self):
        b = self.backend
        if b.reconnecting or self._sequencing:
            return
        nxt = CONN_ORDER[(CONN_ORDER.index(b.connState) + 1) % len(CONN_ORDER)]
        b.set_conn_state(nxt)
        b.set_signal(random.randint(3, 4) if nxt == "secure" else 0)

    async def _do_reconnect(self):
        b = self.backend
        if b.reconnecting or self._sequencing:
            return
        b.set_reconnecting(True)
        await self._connect_sequence()
        b.set_reconnecting(False)
