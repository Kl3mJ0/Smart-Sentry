"""Local IPC for sentry-agent: a Unix domain socket exposing fleet state to
the dashboard (and any other local client) as newline-delimited JSON.

This is what lets the dashboard become a thin client instead of owning BLE
itself - see fleet.py's docstring for why that split matters.

Wire format (newline-delimited JSON, both directions):
  server -> client: {"type": "snapshot", "devices": [...]}                        (on connect, or reply to list_devices)
                    {"type": "state", "device_id": ..., "key": ..., "value": ...}  (on every update)
                    {"type": "ack", "cmd": ..., "ok": true|false, "error": "...", ...}
  client -> server: {"cmd": "list_devices"}
                     {"cmd": "toggle_led", "device_id": ...}
                     {"cmd": "reconnect", "device_id": ...}
                     {"cmd": "enqueue_ota", "device_id": ..., "image_path": ..., "target_version": ...}
                     {"cmd": "confirm_device", "device_id": ...}   (health-check + confirm outside the OTA queue -
                                                                     e.g. a trial image left pending across an agent
                                                                     restart; blocks up to HEALTH_CHECK_TIMEOUT_S)
"""
import asyncio
import json
from pathlib import Path

SOCK_PATH = Path("/tmp/sentry_agent.sock")


class IpcServer:
    def __init__(self, fleet, inventory, confirm_fn=None):
        self.fleet = fleet
        self.inventory = inventory
        self.confirm_fn = confirm_fn  # async (device_id) -> str, see agent_main.confirm_device_now
        self._clients: set[asyncio.StreamWriter] = set()
        fleet.add_state_listener(self._broadcast_state)

    async def start(self):
        if SOCK_PATH.exists():
            SOCK_PATH.unlink()
        server = await asyncio.start_unix_server(self._handle_client, path=str(SOCK_PATH))
        SOCK_PATH.chmod(0o660)  # local-user-only; not a substitute for real auth on the BLE side
        return server

    def _broadcast_state(self, device_id, key, value):
        line = json.dumps({"type": "state", "device_id": device_id, "key": key, "value": value}) + "\n"
        for writer in list(self._clients):
            self._send(writer, line)

    def _send(self, writer, line: str):
        try:
            writer.write(line.encode())
        except Exception:
            self._clients.discard(writer)

    async def _handle_client(self, reader, writer):
        self._clients.add(writer)
        try:
            self._send(writer, json.dumps({"type": "snapshot", "devices": self.fleet.list_devices()}) + "\n")
            await writer.drain()
            while True:
                line = await reader.readline()
                if not line:
                    break
                await self._handle_command(writer, line.decode().strip())
        except (ConnectionResetError, asyncio.IncompleteReadError):
            pass
        finally:
            self._clients.discard(writer)
            writer.close()

    async def _handle_command(self, writer, line: str):
        if not line:
            return
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            return
        cmd = msg.get("cmd")
        ok, err, extra = True, None, {}
        try:
            if cmd == "list_devices":
                self._send(writer, json.dumps({"type": "snapshot", "devices": self.fleet.list_devices()}) + "\n")
                await writer.drain()
                return
            elif cmd == "toggle_led":
                self.fleet.toggle_led(msg["device_id"])
            elif cmd == "reconnect":
                self.fleet.reconnect(msg["device_id"])
            elif cmd == "enqueue_ota":
                job_id = self.inventory.enqueue_ota(
                    msg["device_id"], msg["image_path"], msg.get("target_version")
                )
                extra["job_id"] = job_id
            elif cmd == "confirm_device":
                if self.confirm_fn is None:
                    ok, err = False, "confirm not wired up"
                else:
                    extra["result"] = await self.confirm_fn(msg["device_id"])
            else:
                ok, err = False, f"unknown cmd {cmd!r}"
        except Exception as e:
            ok, err = False, str(e)
        self._send(writer, json.dumps({"type": "ack", "cmd": cmd, "ok": ok, "error": err, **extra}) + "\n")
        await writer.drain()
