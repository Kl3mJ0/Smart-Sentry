"""Manual test helper for a running sentry-agent (agent_main.py must already
be up) - talks over its IPC socket instead of writing raw JSON by hand.

    python3 cli.py list
    python3 cli.py toggle-led <device_id>
    python3 cli.py reconnect <device_id>
    python3 cli.py enqueue-ota <device_id> <image_path> [target_version]
    python3 cli.py confirm <device_id>   (health-check + confirm outside the OTA queue; blocks up to ~90s)
    python3 cli.py jobs <device_id>
"""
import asyncio
import json
import sys

SOCK_PATH = "/tmp/sentry_agent.sock"


async def _request(msg: dict) -> dict:
    """Send `msg` and return its reply, ignoring any "state" broadcast lines
    that arrive first - the server pushes those unsolicited on every sensor
    update, which land ahead of a slow command's ("confirm_device" can take
    up to ~90s) actual "ack"/"snapshot" reply on the same socket."""
    reader, writer = await asyncio.open_unix_connection(SOCK_PATH)
    await reader.readline()  # discard the unsolicited snapshot sent on connect
    writer.write((json.dumps(msg) + "\n").encode())
    await writer.drain()
    while True:
        reply = json.loads(await reader.readline())
        if reply.get("type") != "state":
            writer.close()
            return reply


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd, args = sys.argv[1], sys.argv[2:]

    if cmd == "list":
        print(json.dumps(asyncio.run(_request({"cmd": "list_devices"})), indent=2))
    elif cmd == "toggle-led":
        print(asyncio.run(_request({"cmd": "toggle_led", "device_id": args[0]})))
    elif cmd == "reconnect":
        print(asyncio.run(_request({"cmd": "reconnect", "device_id": args[0]})))
    elif cmd == "enqueue-ota":
        device_id, image_path = args[0], args[1]
        msg = {"cmd": "enqueue_ota", "device_id": device_id, "image_path": image_path}
        if len(args) > 2:
            msg["target_version"] = args[2]
        print(asyncio.run(_request(msg)))
    elif cmd == "confirm":
        print(asyncio.run(_request({"cmd": "confirm_device", "device_id": args[0]})))
    elif cmd == "jobs":
        # not exposed over IPC yet - read straight from the inventory DB
        from inventory import Inventory
        for job in Inventory().jobs_for_device(args[0]):
            print(job)
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
