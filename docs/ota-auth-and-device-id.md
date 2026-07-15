# SS1 OTA security, identity and rollback

Status: implemented on `feature/ss1-ble-ota`; hardware acceptance testing is
required before merging to the stable branch.

## Security chain

1. SS1 advertises `Joseph_BLE-<16 hex factory ID>`, sourced from Zephyr's
   `hwinfo_get_device_id()`. It survives power cycling and reflashing and lets
   the Pi inventory distinguish a fleet without relying on rotating BLE
   addresses.
2. Sensor, LED, firmware-version and DFU access require the provisioned Pi
   certificate challenge-response. A global Zephyr GATT authorization callback
   rejects writes to the stock MCUmgr SMP characteristic unless that exact
   connection is certificate-authenticated.
3. The Pi's `AuthenticatedSMPBLETransport` performs the certificate handshake
   on smpclient's own connection. Authenticating a separate connection would
   not pass the firmware's per-connection gate.
4. MCUboot verifies ECDSA-P256 images using the locally provisioned production
   key and has downgrade prevention enabled. The private key is gitignored;
   see `blinky/keys/README.md`.
5. SS1 does not confirm itself. Every update is a trial boot. The Pi waits for
   15 seconds of fresh temperature and humidity over an authenticated link,
   then explicitly confirms the active hash. On failure it requests a reboot;
   because the image remains unconfirmed, MCUboot restores the previous image.
   Trial jobs survive/recover across agent restarts through SQLite.

The first firmware using the production key requires a wired pristine flash:
the MCUboot already deployed on existing boards trusts the old SDK development
key. All following releases can use BLE OTA.

## Local automated release flow

`sentry-agent` checks `sentry_app/firmware/**/manifest.json` on startup and
periodically. It validates that each referenced signed binary exists, chooses
the highest semantic version, never queues a device already at or above that
version, and avoids duplicate jobs. Jobs are persistent and processed one at a
time because a single Pi Bluetooth adapter is the constrained transport.

The dashboard shows the discovered SS1 fleet, permanent ID/current firmware,
local release state, active upload percentage and the sequential device queue.
The Check Now button invokes the same update check used on startup.

## Cloud release step (deliberately last)

Add a cloud provider that downloads an authenticated manifest and signed binary
into the local firmware cache, ideally with HTTPS certificate pinning or a
separately signed release manifest. The existing version rules, queue, BLE
certificate gate, MCUboot signature verification, trial health check, confirm
and rollback remain unchanged.

Future hardening: device certificates for mutual authentication, an HSM/CI
signing service instead of a filesystem private key, and hardware-backed
security counters if physical rollback attacks enter the threat model.
