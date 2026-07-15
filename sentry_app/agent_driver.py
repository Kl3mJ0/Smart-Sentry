"""Thin BLE driver that talks to sentry_agent over its local IPC socket
instead of touching BLE directly - the dashboard becomes a client of the
agent service (see sentry_agent/ipc.py) rather than owning the adapter, so
OTA/monitoring keep running even if the UI restarts.

The dashboard is still single-device UI, so this driver locks onto the
first device_id it sees and reports only that one; a real multi-device
dashboard is separate, not-yet-built work.
"""
import asyncio
import json

SOCK_PATH = "/tmp/sentry_agent.sock"
RETRY_DELAY_S = 2.0


class AgentDriver:
    def __init__(self, backend):
        self.backend = backend
        self.loop = None  # set by main.py (unused here - no separate thread needed)
        self.device_id = None
        self._writer = None

    async def run(self):
        """Endless connect -> stream loop against the agent socket; never raises."""
        first = True
        while True:
            if not first:
                self.backend.post("reconnecting", True)
            try:
                await self._session()
            except Exception as e:
                print(f"[agent-driver] session ended: {e!r} - retrying in {RETRY_DELAY_S}s")
            self._writer = None
            self.backend.post("signal", 0)
            self.backend.post("conn_state", "scanning")
            first = False
            await asyncio.sleep(RETRY_DELAY_S)

    async def _session(self):
        reader, writer = await asyncio.open_unix_connection(SOCK_PATH)
        self._writer = writer
        try:
            while True:
                line = await reader.readline()
                if not line:
                    raise ConnectionError("sentry-agent socket closed")
                self._handle_line(line)
        finally:
            writer.close()

    def _handle_line(self, line: bytes):
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            return
        mtype = msg.get("type")
        if mtype == "snapshot":
            devices = msg.get("devices", [])
            if devices and self.device_id is None:
                self.device_id = devices[0]["device_id"]
            for d in devices:
                if d["device_id"] == self.device_id:
                    self._apply(d)
        elif mtype == "state":
            if self.device_id is None:
                self.device_id = msg["device_id"]
            if msg["device_id"] == self.device_id:
                self._apply({msg["key"]: msg["value"]})

    def _apply(self, fields: dict):
        for key, value in fields.items():
            if key in ("device_id", "address", "name"):
                continue
            try:
                self.backend.post(key, value)
            except AttributeError:
                pass  # field FleetManager reports that the dashboard doesn't expose yet

    # ---- QML-facing commands (called on the qasync loop thread directly -
    # no run_coroutine_threadsafe needed since nothing here runs on another
    # thread; the actual BLE work happens in the agent process) -----------
    def toggle_led(self):
        self._send_cmd("toggle_led")

    def reconnect(self):
        self._send_cmd("reconnect")

    def _send_cmd(self, cmd: str):
        if self._writer is None or self.device_id is None:
            return
        line = json.dumps({"cmd": cmd, "device_id": self.device_id}) + "\n"
        self._writer.write(line.encode())
