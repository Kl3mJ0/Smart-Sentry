"""MCUmgr/SMP OTA client for one SS1, built on smpclient's BLE transport.

Deliberately mirrors the "prove the Pi can install the existing image"
sub-goal: connect, echo, list images, upload, mark test (trial boot), reset.
`upload()` always leaves `upgrade=False` (test-only) - per smpclient's own
upload() docstring, uploading with `upgrade=True` skips the trial-boot step
and can boot-loop/brick the device. Confirming the trial image is a
*separate*, deliberate step (confirm()) that the Pi's health-check should
gate, not this client automatically - see project memory on why SS1
currently confirms too early.

This module never runs on its own; FleetManager.pause_for_ota() must park
the device's regular DeviceSession first (SS1 only accepts one GATT
connection at a time).
"""
from pathlib import Path

from smp.header import GroupId
from smp.image_management import IMG_MGMT_ERR
from smpclient import SMPClient
from smpclient.generics import error, error_v2, success
from smpclient.requests.image_management import ImageStatesRead, ImageStatesWrite
from smpclient.requests.os_management import EchoWrite, ResetWrite
from smpclient.transport.ble import SMPBLETransport

CONNECT_TIMEOUT_S = 15.0
REQUEST_TIMEOUT_S = 10.0


class OtaError(Exception):
    pass


class OtaSameImageError(OtaError):
    """The uploaded image's hash matches the currently active image, so
    MCUboot correctly refuses to schedule it for trial-boot
    (IMAGE_SETTING_TEST_TO_ACTIVE_DENIED, rc=33) - there's nothing to test.
    Distinct from OtaError so callers can skip retrying: this is a
    deterministic rejection, not a transient link failure. Observed
    2026-07-14 re-uploading a device already running the target version."""


class OtaClient:
    def __init__(self, address: str):
        self.address = address
        self._client = SMPClient(SMPBLETransport(), address, timeout_s=REQUEST_TIMEOUT_S)

    async def __aenter__(self):
        await self._client.connect(connect_timeout_s=CONNECT_TIMEOUT_S)
        return self

    async def __aexit__(self, *exc):
        await self._client.disconnect()

    async def echo(self, text: str = "sentry-agent") -> str:
        response = await self._client.request(EchoWrite(d=text))
        if error(response):
            raise OtaError(f"echo failed: {response}")
        return response.r

    async def list_images(self):
        """Return the ImageStatesReadResponse.images list (slot/version/hash/flags per image)."""
        response = await self._client.request(ImageStatesRead())
        if error(response):
            raise OtaError(f"image list failed: {response}")
        return response.images

    async def upload(self, image_path: str, progress_cb=None) -> None:
        """Upload `image_path` to the inactive slot. Does NOT mark it for
        boot - call mark_test() + reset() afterwards to trial it."""
        data = Path(image_path).read_bytes()
        async for offset in self._client.upload(data, upgrade=False):
            if progress_cb:
                progress_cb(offset, len(data))

    async def mark_test(self, image_hash: bytes) -> None:
        """Mark `image_hash` as pending (test-on-next-reset), matching
        `confirm=False` in MCUmgr's image-states-write."""
        response = await self._client.request(ImageStatesWrite(hash=image_hash, confirm=False))
        if error(response):
            if (
                error_v2(response)
                and response.err.group == GroupId.IMAGE_MANAGEMENT
                and response.err.rc == IMG_MGMT_ERR.IMAGE_SETTING_TEST_TO_ACTIVE_DENIED
            ):
                raise OtaSameImageError(
                    "uploaded image is identical to the currently active image - nothing to trial"
                )
            raise OtaError(f"mark_test failed: {response}")

    async def confirm(self, image_hash: bytes) -> None:
        """Permanently confirm `image_hash` as the running image. Call this
        only after the Pi's own health check has validated the new firmware -
        this is the step SS1 currently does too early on its own."""
        response = await self._client.request(ImageStatesWrite(hash=image_hash, confirm=True))
        if error(response):
            raise OtaError(f"confirm failed: {response}")

    async def reset(self) -> None:
        response = await self._client.request(ResetWrite())
        if error(response):
            raise OtaError(f"reset failed: {response}")
