"""Start sentry-agent for the dashboard when no service already owns BLE.

Production installations may still run sentry-agent with systemd.  The app
first probes the IPC socket and only launches its bundled agent when that
socket is not live, so both deployment styles use the same dashboard command.
"""
import asyncio
import sys
from pathlib import Path


APP_DIR = Path(__file__).resolve().parent
AGENT_DIR = APP_DIR / "sentry_agent"
SOCK_PATH = "/tmp/sentry_agent.sock"
RESTART_DELAY_S = 3.0


async def agent_is_running() -> bool:
    """Return True only when something is accepting agent IPC connections."""
    try:
        _reader, writer = await asyncio.wait_for(
            asyncio.open_unix_connection(SOCK_PATH), timeout=0.75
        )
    except (OSError, asyncio.TimeoutError):
        return False
    writer.close()
    try:
        await writer.wait_closed()
    except (AttributeError, OSError):
        pass
    return True


class AgentProcessManager:
    """Ensure an agent exists without taking BLE ownership in the UI process."""

    async def run(self):
        if not sys.platform.startswith("linux"):
            raise RuntimeError("the sentry-agent integration requires Linux/BlueZ")

        if await agent_is_running():
            print("[dashboard] using existing sentry-agent service")
            return

        while True:
            print("[dashboard] no sentry-agent detected; starting bundled background agent")
            process = await asyncio.create_subprocess_exec(
                sys.executable,
                "-u",
                "agent_main.py",
                cwd=str(AGENT_DIR),
                start_new_session=True,
            )
            return_code = await process.wait()
            if await agent_is_running():
                print("[dashboard] another sentry-agent became available")
                return
            print(f"[dashboard] sentry-agent exited with status {return_code}; retrying")
            await asyncio.sleep(RESTART_DELAY_S)
