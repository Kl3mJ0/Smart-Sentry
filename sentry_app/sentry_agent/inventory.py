"""Persistent SQLite inventory of SS1 devices + the OTA job queue.

Current firmware advertises only "Joseph_BLE", so device_id falls back to
the BLE address. Future firmware can advertise "Joseph_BLE-<ID>" and
resolve_device_id() will use that stable suffix without changing callers.
"""
import sqlite3
import time
from pathlib import Path

DB_PATH = Path(__file__).resolve().parent / "fleet.db"

SCHEMA = """
CREATE TABLE IF NOT EXISTS devices (
    device_id TEXT PRIMARY KEY,
    address TEXT NOT NULL,
    name TEXT NOT NULL,
    first_seen REAL NOT NULL,
    last_seen REAL NOT NULL,
    fw_version TEXT,
    status TEXT NOT NULL DEFAULT 'unknown'
);

CREATE TABLE IF NOT EXISTS ota_jobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    image_path TEXT NOT NULL,
    target_version TEXT,
    state TEXT NOT NULL DEFAULT 'queued',
    attempts INTEGER NOT NULL DEFAULT 0,
    created_at REAL NOT NULL,
    updated_at REAL NOT NULL,
    result TEXT,
    progress INTEGER NOT NULL DEFAULT 0,
    FOREIGN KEY (device_id) REFERENCES devices(device_id)
);
"""


NAME_ID_PREFIX = "Joseph_BLE-"  # must stay in sync with fleet.TARGET_NAME_PREFIX + "-"


def resolve_device_id(address: str, name: str) -> str:
    """Stable per-device key.

    Pi side of the device-identity plan (docs/ota-auth-and-device-id.md):
    once firmware advertises "Joseph_BLE-<ID>" (the full factory ID read via
    Zephyr's hwinfo API), that suffix is the device_id - stable across reflash,
    power cycles and address rotation. Until then, fall back to the BLE
    address (random-static: stable across reboots in practice, but NOT
    guaranteed across re-provisioning).

    Migration note: when firmware starts advertising IDs, previously seen
    devices get a fresh inventory row keyed by the new ID; the old
    address-keyed row simply goes stale (harmless - no jobs reference it
    once it stops being scheduled against).
    """
    if name.startswith(NAME_ID_PREFIX) and len(name) > len(NAME_ID_PREFIX):
        return name[len(NAME_ID_PREFIX):]
    return address


class Inventory:
    def __init__(self, db_path=DB_PATH):
        self._conn = sqlite3.connect(db_path, check_same_thread=False)
        self._conn.executescript(SCHEMA)
        columns = {row[1] for row in self._conn.execute("PRAGMA table_info(ota_jobs)")}
        if "progress" not in columns:
            self._conn.execute("ALTER TABLE ota_jobs ADD COLUMN progress INTEGER NOT NULL DEFAULT 0")
        self._conn.commit()
        self._listeners = []

    def add_listener(self, fn) -> None:
        self._listeners.append(fn)

    def _changed(self) -> None:
        for fn in list(self._listeners):
            fn()

    def upsert_device(self, address: str, name: str, status: str = "unknown") -> str:
        device_id = resolve_device_id(address, name)
        now = time.time()
        row = self._conn.execute(
            "SELECT device_id FROM devices WHERE device_id=?", (device_id,)
        ).fetchone()
        if row:
            self._conn.execute(
                "UPDATE devices SET address=?, name=?, last_seen=?, status=? WHERE device_id=?",
                (address, name, now, status, device_id),
            )
        else:
            self._conn.execute(
                "INSERT INTO devices (device_id, address, name, first_seen, last_seen, fw_version, status) "
                "VALUES (?, ?, ?, ?, ?, NULL, ?)",
                (device_id, address, name, now, now, status),
            )
        self._conn.commit()
        return device_id

    def set_status(self, device_id: str, status: str) -> None:
        self._conn.execute(
            "UPDATE devices SET status=?, last_seen=? WHERE device_id=?",
            (status, time.time(), device_id),
        )
        self._conn.commit()

    def set_fw_version(self, device_id: str, fw_version: str) -> None:
        self._conn.execute(
            "UPDATE devices SET fw_version=? WHERE device_id=?", (fw_version, device_id)
        )
        self._conn.commit()
        self._changed()

    def list_devices(self) -> list[dict]:
        cur = self._conn.execute(
            "SELECT device_id, address, name, first_seen, last_seen, fw_version, status FROM devices"
        )
        cols = [d[0] for d in cur.description]
        return [dict(zip(cols, row)) for row in cur.fetchall()]

    # ---- OTA queue: one job processed at a time (see agent_main's worker) --
    def enqueue_ota(self, device_id: str, image_path: str, target_version: str | None = None) -> int:
        now = time.time()
        cur = self._conn.execute(
            "INSERT INTO ota_jobs (device_id, image_path, target_version, state, created_at, updated_at) "
            "VALUES (?, ?, ?, 'queued', ?, ?)",
            (device_id, image_path, target_version, now, now),
        )
        self._conn.commit()
        self._changed()
        return cur.lastrowid

    def has_target_job(self, device_id: str, target_version: str) -> bool:
        row = self._conn.execute(
            "SELECT 1 FROM ota_jobs WHERE device_id=? AND target_version=? "
            "AND state NOT IN ('failed','health_check_failed','confirm_failed') LIMIT 1",
            (device_id, target_version),
        ).fetchone()
        return row is not None

    def next_queued_job(self) -> dict | None:
        cur = self._conn.execute(
            "SELECT id, device_id, image_path, target_version, attempts FROM ota_jobs "
            "WHERE state='queued' ORDER BY created_at LIMIT 1"
        )
        row = cur.fetchone()
        if not row:
            return None
        cols = [d[0] for d in cur.description]
        return dict(zip(cols, row))

    def set_job_state(self, job_id: int, state: str, result: str | None = None, bump_attempts: bool = False) -> None:
        now = time.time()
        if bump_attempts:
            self._conn.execute(
                "UPDATE ota_jobs SET state=?, result=?, updated_at=?, attempts=attempts+1 WHERE id=?",
                (state, result, now, job_id),
            )
        else:
            self._conn.execute(
                "UPDATE ota_jobs SET state=?, result=?, updated_at=? WHERE id=?",
                (state, result, now, job_id),
            )
        self._conn.commit()
        self._changed()

    def set_job_progress(self, job_id: int, progress: int, result: str | None = None) -> None:
        self._conn.execute(
            "UPDATE ota_jobs SET progress=?, result=COALESCE(?, result), updated_at=? WHERE id=?",
            (max(0, min(100, progress)), result, time.time(), job_id),
        )
        self._conn.commit()
        self._changed()

    def list_jobs(self, limit: int = 100) -> list[dict]:
        cur = self._conn.execute(
            "SELECT j.id, j.device_id, d.name AS device_name, j.image_path, "
            "j.target_version, j.state, j.progress, j.attempts, j.result, j.created_at, j.updated_at "
            "FROM ota_jobs j LEFT JOIN devices d ON d.device_id=j.device_id "
            "ORDER BY CASE WHEN j.state IN ('running','uploading','trial_pending','checking_health','confirming') "
            "THEN 0 WHEN j.state='queued' THEN 1 ELSE 2 END, j.created_at LIMIT ?",
            (limit,),
        )
        cols = [d[0] for d in cur.description]
        jobs = [dict(zip(cols, row)) for row in cur.fetchall()]
        queue_position = 0
        for job in jobs:
            if job["state"] == "queued":
                queue_position += 1
                job["queue_position"] = queue_position
            else:
                job["queue_position"] = 0
        return jobs

    def recoverable_trial_jobs(self) -> list[dict]:
        cur = self._conn.execute(
            "SELECT id, device_id, image_path, target_version FROM ota_jobs "
            "WHERE state IN ('trial_pending','checking_health','confirming') ORDER BY updated_at"
        )
        cols = [d[0] for d in cur.description]
        return [dict(zip(cols, row)) for row in cur.fetchall()]

    def requeue_interrupted_uploads(self) -> int:
        """Put jobs interrupted before trial boot back at the queue head."""
        cur = self._conn.execute(
            "UPDATE ota_jobs SET state='queued', progress=0, result=?, updated_at=? "
            "WHERE state IN ('running','uploading')",
            ("Agent restarted; upload queued to resume from the beginning", time.time()),
        )
        self._conn.commit()
        if cur.rowcount:
            self._changed()
        return cur.rowcount

    def jobs_for_device(self, device_id: str) -> list[dict]:
        cur = self._conn.execute(
            "SELECT id, image_path, target_version, state, progress, attempts, result FROM ota_jobs "
            "WHERE device_id=? ORDER BY created_at",
            (device_id,),
        )
        cols = [d[0] for d in cur.description]
        return [dict(zip(cols, row)) for row in cur.fetchall()]
