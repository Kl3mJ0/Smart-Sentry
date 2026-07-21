import asyncio
import sys
import unittest
from pathlib import Path

AGENT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AGENT_DIR))

from inventory import Inventory  # noqa: E402
from releases import LocalReleaseService, version_tuple  # noqa: E402


class FakeFleet:
    def __init__(self, devices=None):
        self.devices = devices or []

    def add_state_listener(self, _fn):
        pass

    def list_devices(self):
        return self.devices


class ReleaseTests(unittest.TestCase):
    def test_numeric_version_order(self):
        self.assertGreater(version_tuple("1.10.0"), version_tuple("1.9.9"))

    def test_queues_upgrade_once_and_never_downgrade(self):
        inv = Inventory(":memory:")
        old = inv.upsert_device("AA:BB:CC:DD:EE:01", "Joseph_BLE-0000000000000001")
        new = inv.upsert_device("AA:BB:CC:DD:EE:02", "Joseph_BLE-0000000000000002")
        inv.set_fw_version(old, "0.0.1")
        inv.set_fw_version(new, "99.0.0")
        devices = [
            {"device_id": old, "fw_version": "0.0.1"},
            {"device_id": new, "fw_version": "99.0.0"},
        ]
        service = LocalReleaseService(FakeFleet(devices), inv)
        self.assertEqual(asyncio.run(service.check_now()), 1)
        self.assertEqual(asyncio.run(service.check_now()), 0)
        jobs = inv.list_jobs()
        self.assertEqual(len(jobs), 1)
        self.assertEqual(jobs[0]["device_id"], "0000000000000001")

    def test_queue_positions_only_number_waiting_jobs(self):
        inv = Inventory(":memory:")
        first = inv.upsert_device("AA:BB:CC:DD:EE:01", "Joseph_BLE-0000000000000001")
        second = inv.upsert_device("AA:BB:CC:DD:EE:02", "Joseph_BLE-0000000000000002")
        first_job = inv.enqueue_ota(first, "/tmp/fw.bin", "1.2.3")
        inv.enqueue_ota(second, "/tmp/fw.bin", "1.2.3")
        inv.set_job_state(first_job, "uploading")

        jobs = inv.list_jobs()
        self.assertEqual(jobs[0]["queue_position"], 0)
        self.assertEqual(jobs[1]["queue_position"], 1)

    def test_interrupted_upload_is_requeued_after_agent_restart(self):
        inv = Inventory(":memory:")
        device = inv.upsert_device("AA:BB:CC:DD:EE:01", "Joseph_BLE-0000000000000001")
        job_id = inv.enqueue_ota(device, "/tmp/fw.bin", "1.2.3")
        inv.set_job_state(job_id, "uploading")
        inv.set_job_progress(job_id, 47)

        self.assertEqual(inv.requeue_interrupted_uploads(), 1)
        job = inv.next_queued_job()
        self.assertEqual(job["id"], job_id)
        self.assertEqual(inv.list_jobs()[0]["progress"], 0)


if __name__ == "__main__":
    unittest.main()
