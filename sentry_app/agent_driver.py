"""Thin BLE driver that talks to sentry_agent over its local IPC socket
instead of touching BLE directly - the dashboard becomes a client of the
agent service (see sentry_agent/ipc.py) rather than owning the adapter, so
OTA/monitoring keep running even if the UI restarts.

The dashboard presents one selected device at a time while retaining the
whole discovered fleet and OTA queue for navigation and status display.
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
        self.devices = {}
        self.order = []
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
                self.backend.set_update_status(
                    "agent_offline", "Starting or reconnecting to the background fleet agent…"
                )
            self._writer = None
            self.backend.post("signal", 0)
            self.backend.post("conn_state", "scanning")
            first = False
            await asyncio.sleep(RETRY_DELAY_S)

    async def _session(self):
        reader, writer = await asyncio.open_unix_connection(SOCK_PATH)
        self._writer = writer
        self.backend.post("reconnecting", False)
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
            self.devices = {d["device_id"]: d for d in devices}
            self.order = [d["device_id"] for d in devices]
            if self.device_id not in self.devices:
                self.device_id = self.order[0] if self.order else None
            self._apply_selected()
            self.backend.set_jobs(msg.get("jobs", []))
            updates = msg.get("updates", {})
            self.backend.set_update_status(updates.get("status"), updates.get("message"), updates.get("latest_version"))
        elif mtype == "state":
            if self.device_id is None:
                self.device_id = msg["device_id"]
            self.devices.setdefault(msg["device_id"], {"device_id": msg["device_id"], "name": msg["device_id"]})
            if msg["device_id"] not in self.order:
                self.order.append(msg["device_id"])
            self.devices[msg["device_id"]][msg["key"]] = msg["value"]
            if msg["device_id"] == self.device_id:
                self._apply({msg["key"]: msg["value"]})
                self._publish_device()
        elif mtype == "jobs":
            self.backend.set_jobs(msg.get("jobs", []))
        elif mtype == "update_status":
            self.backend.set_update_status(msg.get("status"), msg.get("message"), msg.get("latest_version"))
        elif mtype == "ack" and not msg.get("ok", False):
            self.backend.set_update_status(
                "error", f"{msg.get('cmd', 'Agent command')} failed: {msg.get('error') or 'unknown error'}"
            )

    def _apply_selected(self):
        if self.device_id and self.device_id in self.devices:
            self._apply(self.devices[self.device_id])
        self._publish_device()

    def _publish_device(self):
        d = self.devices.get(self.device_id, {})
        pos = self.order.index(self.device_id) + 1 if self.device_id in self.order else 0
        self.backend.set_device(
            self.device_id or "", d.get("name", "No SS1 discovered"), pos, len(self.order), d.get("fw_version", "unknown")
        )

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

    def next_device(self):
        self._move_device(1)

    def previous_device(self):
        self._move_device(-1)

    def _move_device(self, delta):
        if not self.order:
            return
        index = self.order.index(self.device_id) if self.device_id in self.order else 0
        self.device_id = self.order[(index + delta) % len(self.order)]
        self._apply_selected()

    def check_updates(self):
        self.backend.set_update_status("checking", "Checking local signed firmware releases…")
        if not self._send_cmd("check_updates", include_device=False):
            self.backend.set_update_status("agent_offline", "Fleet agent is not ready yet; retry in a moment")

    def _send_cmd(self, cmd: str, include_device=True):
        if self._writer is None or (include_device and self.device_id is None):
            return False
        payload = {"cmd": cmd}
        if include_device:
            payload["device_id"] = self.device_id
        line = json.dumps(payload) + "\n"
        self._writer.write(line.encode())
        asyncio.create_task(self._drain_writer())
        return True

    async def _drain_writer(self):
        try:
            if self._writer:
                await self._writer.drain()
        except (ConnectionError, OSError):
            pass
