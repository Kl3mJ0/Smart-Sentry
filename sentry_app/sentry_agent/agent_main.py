"""Entry point for sentry-agent: the headless service that exclusively owns
BLE, cloud communication (future) and OTA for this Pi's SS1 fleet. Run this
under systemd (see ../systemd/sentry-agent.service) - the dashboard becomes a
thin client talking to it over the IPC socket (see ipc.py) instead of
touching BLE directly, so OTA/monitoring survive a UI restart and nothing
fights over the adapter.

    python3 agent_main.py

The local release service checks at startup and on a timer, queues every
connected older SS1, and the queue worker drives the full upload -> trial
boot -> health check -> Pi confirmation cycle one device at a time.
"""
import asyncio
import os
from pathlib import Path

try:
    import fcntl
except ImportError:  # keeps cross-platform tooling/imports readable; agent itself is Linux-only
    fcntl = None

from fleet import FleetManager
from inventory import Inventory
from ipc import IpcServer
from ota import OtaClient, OtaError, OtaSameImageError
from releases import LocalReleaseService

OTA_POLL_INTERVAL_S = 5.0
# The upload itself (not mark_test/reset) has been observed to disconnect
# mid-transfer at random offsets - 28%, 61%, 77%, 100% (success), 27%, 100%
# across 6 runs on 2026-07-14, i.e. ~1/3 success per attempt. Random failure
# points rule out a deterministic cause (e.g. a version/downgrade check,
# which would fail at the same fixed point every time); this looks like a
# transient BLE link/timing issue, so retrying the whole attempt is the
# practical fix until the root cause (candidate: blocking flash-erase vs.
# BLE supervision timeout) is found on the firmware side.
MAX_OTA_ATTEMPTS = 5
OTA_RETRY_DELAY_S = 3.0
INSTANCE_LOCK_PATH = Path("/tmp/sentry_agent.lock")


def acquire_instance_lock():
    """Hold an OS lock for this process so only one owner can touch BLE."""
    if fcntl is None:
        raise RuntimeError("sentry-agent requires Linux file locking")
    lock_file = INSTANCE_LOCK_PATH.open("a+")
    try:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lock_file.close()
        return None
    lock_file.seek(0)
    lock_file.truncate()
    lock_file.write(str(os.getpid()))
    lock_file.flush()
    return lock_file

# Production health check after a trial-boot: the device must reconnect on its own
# (proves BLE/cert-auth still work post-flash) and keep streaming fresh
# every capability it advertises for HEALTH_CHECK_STABLE_S straight, within
# HEALTH_CHECK_TIMEOUT_S of the reset. The agent confirms only after this
# passes. Failed trials remain unconfirmed and are rebooted for MCUboot
# rollback; see docs/ota-auth-and-device-id.md.
HEALTH_CHECK_TIMEOUT_S = 90.0
HEALTH_CHECK_STABLE_S = 15.0
HEALTH_CHECK_POLL_S = 1.0


async def _run_ota_job(job: dict, address: str, inventory: Inventory) -> None:
    """Upload + trial-boot, retrying the whole sequence on transient failure.
    Never confirms the image - the caller runs a health check first and
    only then calls _confirm_trial_image().

    OtaSameImageError is NOT retried: it means the upload succeeded and
    MCUboot deterministically refused mark_test because the image is
    identical to what's already active (rc=33,
    IMAGE_SETTING_TEST_TO_ACTIVE_DENIED) - retrying would just hit the same
    rejection again every time."""
    last_error: Exception | None = None
    for attempt in range(1, MAX_OTA_ATTEMPTS + 1):
        try:
            last_progress = -1

            def report_progress(offset, total):
                nonlocal last_progress
                progress = int(offset * 100 / max(total, 1))
                if progress != last_progress:
                    last_progress = progress
                    inventory.set_job_progress(
                        job["id"], progress, f"Uploading {offset}/{total} bytes"
                    )

            async with OtaClient(address) as ota:
                inventory.set_job_state(job["id"], "uploading", result="Uploading signed image")
                await ota.upload(job["image_path"], report_progress)
                inventory.set_job_progress(job["id"], 100, "Upload complete; scheduling trial boot")
                images = await ota.list_images()
                pending = next((img for img in images if not img.active), None)
                if pending is None:
                    raise OtaError("no inactive image slot to trial after upload")
                await ota.mark_test(pending.hash)
                await ota.reset()
            return
        except OtaSameImageError:
            raise
        except Exception as e:
            last_error = e
            print(f"[ota] upload attempt {attempt}/{MAX_OTA_ATTEMPTS} failed: {e!r}")
            if attempt < MAX_OTA_ATTEMPTS:
                await asyncio.sleep(OTA_RETRY_DELAY_S)
    raise last_error


async def _wait_for_healthy(fleet: FleetManager, device_id: str) -> bool:
    """Block until the device proves itself healthy after a reset, or time out.

    Uses a temporary state-listener rather than reading fleet.state directly,
    so a stale pre-reboot temp/humidity value already sitting in fleet.state
    can't be mistaken for a fresh post-reboot reading: only events observed
    *during* this call count. Any conn_state other than "secure" clears the
    freshness flags, so the device must stay connected and streaming for the
    full HEALTH_CHECK_STABLE_S window, not just briefly touch it.
    """
    loop = asyncio.get_event_loop()
    deadline = loop.time() + HEALTH_CHECK_TIMEOUT_S
    fresh_temp = asyncio.Event()
    fresh_humidity = asyncio.Event()

    def _listener(dev_id, key, value):
        if dev_id != device_id:
            return
        if key == "temp":
            fresh_temp.set()
        elif key == "humidity":
            fresh_humidity.set()
        elif key == "conn_state" and value != "secure":
            fresh_temp.clear()
            fresh_humidity.clear()

    fleet.add_state_listener(_listener)
    try:
        stable_since = None
        while loop.time() < deadline:
            state = fleet.state.get(device_id, {})
            humidity_required = state.get("humidity_available", True)
            healthy_now = (
                state.get("conn_state") == "secure"
                and not state.get("reconnecting")
                and fresh_temp.is_set()
                and (fresh_humidity.is_set() if humidity_required else True)
            )
            if healthy_now:
                stable_since = stable_since or loop.time()
                if loop.time() - stable_since >= HEALTH_CHECK_STABLE_S:
                    return True
            else:
                stable_since = None
            await asyncio.sleep(HEALTH_CHECK_POLL_S)
        return False
    finally:
        fleet.remove_state_listener(_listener)


class HealthCheckFailed(Exception):
    pass


async def _confirm_active_image(fleet: FleetManager, address: str, device_id: str) -> str:
    """Reconnect via OTA (pausing the sensor session again) and confirm
    whatever image is currently active. Returns a human-readable result
    string; raises on failure."""
    paused = await fleet.pause_for_ota(device_id)
    try:
        async with fleet.discovery_lock:
            async with OtaClient(address) as ota:
                images = await ota.list_images()
                active = next((img for img in images if img.active), None)
                if active is None:
                    raise OtaError("no active image found")
                await ota.confirm(active.hash)
        return f"health check passed, confirmed version {active.version}"
    finally:
        if paused:
            fleet.resume_after_ota(device_id)


async def confirm_device_now(fleet: FleetManager, device_id: str) -> str:
    """Run the health check + confirm flow for `device_id` right now,
    independent of the OTA queue - e.g. to finish confirming a trial image
    that was left pending (the agent doesn't persist "job in flight" across
    its own restarts, so a trial image from before a restart needs this)."""
    session = fleet.sessions.get(device_id)
    address = session.address if session else device_id
    healthy = await _wait_for_healthy(fleet, device_id)
    if not healthy:
        raise HealthCheckFailed(f"device did not prove healthy within {HEALTH_CHECK_TIMEOUT_S:.0f}s")
    return await _confirm_active_image(fleet, address, device_id)


async def _force_trial_rollback(fleet: FleetManager, device_id: str) -> str:
    """Reboot an unconfirmed trial image. MCUboot then restores the previous
    confirmed slot. If the broken image cannot authenticate, rollback remains
    guaranteed at the next physical/watchdog reboot because it was never
    confirmed; report that distinction accurately."""
    session = fleet.sessions.get(device_id)
    if not session:
        return "rollback armed; device is offline and will revert on next reboot"
    paused = await fleet.pause_for_ota(device_id)
    try:
        async with fleet.discovery_lock:
            async with OtaClient(session.address) as ota:
                await ota.reset()
        return "rollback reboot requested; MCUboot will restore previous confirmed image"
    except Exception as exc:
        return f"rollback armed for next reboot (remote reset unavailable: {exc})"
    finally:
        if paused:
            fleet.resume_after_ota(device_id)


async def ota_worker(fleet: FleetManager, inventory: Inventory):
    """Drains the OTA queue sequentially (this Pi has one adapter, so
    sequential-per-adapter and sequential-globally are the same thing for
    now)."""
    startup_recovery_done = False
    while True:
        if not startup_recovery_done:
            startup_recovery_done = True
            recovered_uploads = inventory.requeue_interrupted_uploads()
            if recovered_uploads:
                print(f"[ota] requeued {recovered_uploads} interrupted upload(s)")
            for trial in inventory.recoverable_trial_jobs():
                inventory.set_job_state(trial["id"], "checking_health", result="Agent restarted; recovering trial")
                try:
                    result = await confirm_device_now(fleet, trial["device_id"])
                    inventory.set_job_state(trial["id"], "confirmed", result=result)
                except Exception as exc:
                    rollback = await _force_trial_rollback(fleet, trial["device_id"])
                    inventory.set_job_state(trial["id"], "health_check_failed", result=f"{exc}; {rollback}")
        job = inventory.next_queued_job()
        if job is None:
            await asyncio.sleep(OTA_POLL_INTERVAL_S)
            continue

        device_id = job["device_id"]
        session = fleet.sessions.get(device_id)
        address = session.address if session else device_id  # device_id IS the address today
        inventory.set_job_state(job["id"], "running", bump_attempts=True)
        paused = await fleet.pause_for_ota(device_id)
        try:
            # Hold the same lock fleet.run() takes around its scan: BlueZ
            # allows only one discovery session adapter-wide, and OtaClient's
            # connect-by-address does its own scan internally.
            async with fleet.discovery_lock:
                await _run_ota_job(job, address, inventory)
        except OtaSameImageError as e:
            inventory.set_job_state(job["id"], "same_image", result=str(e))
            print(f"[ota] job {job['id']} skipped: {e}")
            if paused:
                fleet.resume_after_ota(device_id)
            continue
        except Exception as e:
            inventory.set_job_state(job["id"], "failed", result=str(e))
            print(f"[ota] job {job['id']} failed after {MAX_OTA_ATTEMPTS} attempts: {e!r}")
            if paused:
                fleet.resume_after_ota(device_id)
            continue

        # Upload + trial-boot succeeded and the device is rebooting. Let its
        # normal sensor session reconnect on its own - that's the health
        # check itself (proves BLE/cert-auth work post-flash) - before
        # deciding whether to confirm.
        inventory.set_job_state(job["id"], "checking_health", result="Trial boot started; Pi health check in progress")
        if paused:
            fleet.resume_after_ota(device_id)

        try:
            result = await confirm_device_now(fleet, device_id)
            inventory.set_job_state(job["id"], "confirmed", result=result)
            print(f"[ota] job {job['id']} confirmed: {result}")
        except HealthCheckFailed as e:
            rollback = await _force_trial_rollback(fleet, device_id)
            inventory.set_job_state(job["id"], "health_check_failed", result=f"{e}; {rollback}")
            print(f"[ota] job {job['id']} health check failed: {rollback}")
        except Exception as e:
            inventory.set_job_state(job["id"], "confirm_failed", result=str(e))
            print(f"[ota] job {job['id']} confirm failed: {e!r}")


async def main():
    instance_lock = acquire_instance_lock()
    if instance_lock is None:
        print("[agent] another sentry-agent already owns BLE; exiting")
        return
    inventory = Inventory()
    fleet = FleetManager(inventory)
    releases = LocalReleaseService(fleet, inventory)
    ipc = IpcServer(
        fleet, inventory,
        confirm_fn=lambda device_id: confirm_device_now(fleet, device_id),
        releases=releases,
    )

    try:
        server = await ipc.start()
        async with server:
            await asyncio.gather(
                fleet.run(),
                ota_worker(fleet, inventory),
                releases.run(),
                server.serve_forever(),
            )
    finally:
        instance_lock.close()


if __name__ == "__main__":
    asyncio.run(main())
