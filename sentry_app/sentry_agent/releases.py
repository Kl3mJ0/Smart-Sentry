"""Local release discovery and automatic, downgrade-safe fleet scheduling.

Cloud support will later implement the same `scan()` contract; the rest of
the agent and dashboard will not care whether a signed image came from disk
or was downloaded into this release cache.
"""
import asyncio
import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path

FIRMWARE_ROOT = Path(__file__).resolve().parent.parent / "firmware"
CHECK_INTERVAL_S = 15.0


def version_tuple(value: str) -> tuple[int, ...]:
    numbers = re.findall(r"\d+", value or "")
    return tuple(int(n) for n in numbers[:4]) or (0,)


@dataclass(frozen=True)
class Release:
    version: str
    image_path: Path
    sha256: str


class LocalReleaseService:
    def __init__(self, fleet, inventory):
        self.fleet = fleet
        self.inventory = inventory
        self.status = "idle"
        self.message = "Waiting for startup update check"
        self.latest: Release | None = None
        self._listeners = []
        self._lock = asyncio.Lock()
        fleet.add_state_listener(self._on_fleet_state)

    def _on_fleet_state(self, _device_id, key, _value):
        # The startup scan can finish before an SS1 has authenticated and
        # reported its version. Re-check immediately when that fact arrives.
        if key == "fw_version":
            asyncio.create_task(self.check_now())

    def add_listener(self, fn):
        self._listeners.append(fn)

    def _publish(self, status: str, message: str):
        self.status, self.message = status, message
        for fn in list(self._listeners):
            fn(self.snapshot())

    def snapshot(self) -> dict:
        return {
            "status": self.status,
            "message": self.message,
            "latest_version": self.latest.version if self.latest else None,
        }

    def scan(self) -> list[Release]:
        found = []
        for manifest_path in FIRMWARE_ROOT.rglob("manifest.json"):
            try:
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                app_file = next(f for f in manifest["files"] if f.get("type") == "application")
                image = manifest_path.parent / app_file["file"]
                if not image.is_file():
                    continue
                version = str(app_file["version_MCUBOOT"]).split("+")[0]
                digest = hashlib.sha256(image.read_bytes()).hexdigest()
                expected_digest = manifest.get("sha256")
                if expected_digest and digest.lower() != str(expected_digest).lower():
                    raise ValueError("release SHA-256 does not match manifest")
                found.append(Release(version, image.resolve(), digest))
            except (KeyError, ValueError, OSError, json.JSONDecodeError, StopIteration) as exc:
                print(f"[releases] ignored {manifest_path}: {exc}")
        return sorted(found, key=lambda r: version_tuple(r.version), reverse=True)

    async def check_now(self) -> int:
        async with self._lock:
            self._publish("checking", "Checking local signed firmware releases…")
            releases = self.scan()
            if not releases:
                self.latest = None
                self._publish("no_release", "No local firmware release found")
                return 0
            self.latest = releases[0]
            queued = 0
            # Only schedule devices discovered in this agent lifetime. Stale
            # pre-permanent-ID inventory rows must never create ghost jobs.
            for device in self.fleet.list_devices():
                current = device.get("fw_version")
                if not current or version_tuple(current) >= version_tuple(self.latest.version):
                    continue
                device_id = device["device_id"]
                if self.inventory.has_target_job(device_id, self.latest.version):
                    continue
                self.inventory.enqueue_ota(device_id, str(self.latest.image_path), self.latest.version)
                queued += 1
            if queued:
                self._publish("queued", f"Queued {queued} SS1 device(s) for {self.latest.version}")
            else:
                self._publish("up_to_date", f"Fleet firmware is up to date ({self.latest.version})")
            return queued

    async def run(self):
        while True:
            try:
                await self.check_now()
            except Exception as exc:
                self._publish("error", f"Update check failed: {exc}")
            await asyncio.sleep(CHECK_INTERVAL_S)
