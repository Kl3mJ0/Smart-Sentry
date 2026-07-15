"""Entry point for sentry-agent: the headless service that exclusively owns
BLE, cloud communication (future) and OTA for this Pi's SS1 fleet. Run this
under systemd (see ../systemd/sentry-agent.service) - the dashboard becomes a
thin client talking to it over the IPC socket (see ipc.py) instead of
touching BLE directly, so OTA/monitoring survive a UI restart and nothing
fights over the adapter.

    python3 agent_main.py

Nothing here auto-executes an OTA: enqueue_ota jobs sit in the queue until
something (an operator via the IPC socket, or the cloud poller once it
exists) puts one there. The queue worker below only *drains* jobs that are
already queued, one at a time - but once it does, it drives the full
upload -> trial-boot -> health-check -> confirm/revert cycle itself; SS1
no longer decides on its own when a trial image is good (see
_wait_for_healthy/_confirm_trial_image).
"""
import asyncio

from fleet import FleetManager
from inventory import Inventory
from ipc import IpcServer
from ota import OtaClient, OtaError, OtaSameImageError

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

# Health check after a trial-boot: the device must reconnect on its own
# (proves BLE/cert-auth still work post-flash) and keep streaming fresh
# temp+humidity for HEALTH_CHECK_STABLE_S straight, within
# HEALTH_CHECK_TIMEOUT_S of the reset. This - not SS1 itself - is what
# decides whether the image gets confirmed. If it doesn't pass, we
# deliberately leave the image unconfirmed: MCUboot reverts to the previous
# image on the next power cycle on its own, which is the safety net we want
# to keep, not route around.
HEALTH_CHECK_TIMEOUT_S = 90.0
HEALTH_CHECK_STABLE_S = 15.0
HEALTH_CHECK_POLL_S = 1.0


async def _run_ota_job(job: dict, address: str) -> None:
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
            async with OtaClient(address) as ota:
                await ota.upload(job["image_path"])
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
            healthy_now = (
                state.get("conn_state") == "secure"
                and not state.get("reconnecting")
                and fresh_temp.is_set()
                and fresh_humidity.is_set()
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


async def ota_worker(fleet: FleetManager, inventory: Inventory):
    """Drains the OTA queue sequentially (this Pi has one adapter, so
    sequential-per-adapter and sequential-globally are the same thing for
    now)."""
    while True:
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
                await _run_ota_job(job, address)
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
        inventory.set_job_state(job["id"], "trial_pending", result="uploaded + reset, running health check")
        if paused:
            fleet.resume_after_ota(device_id)

        try:
            result = await confirm_device_now(fleet, device_id)
            inventory.set_job_state(job["id"], "confirmed", result=result)
            print(f"[ota] job {job['id']} confirmed: {result}")
        except HealthCheckFailed as e:
            inventory.set_job_state(
                job["id"], "health_check_failed",
                result=f"{e} - left unconfirmed, MCUboot will revert on next power cycle",
            )
            print(f"[ota] job {job['id']} health check failed - leaving unconfirmed")
        except Exception as e:
            inventory.set_job_state(job["id"], "confirm_failed", result=str(e))
            print(f"[ota] job {job['id']} confirm failed: {e!r}")


async def main():
    inventory = Inventory()
    fleet = FleetManager(inventory)
    ipc = IpcServer(fleet, inventory, confirm_fn=lambda device_id: confirm_device_now(fleet, device_id))

    server = await ipc.start()
    async with server:
        await asyncio.gather(
            fleet.run(),
            ota_worker(fleet, inventory),
            server.serve_forever(),
        )


if __name__ == "__main__":
    asyncio.run(main())
