# OTA authentication & device identity — design / R&D

Status: DESIGN — Pi-side prep implemented (see §5); firmware work not started.
Owners: firmware items → nRF/firmware session; Pi items → Pi/app session.

## 1. Where we are today

What is already protected:

- **Sensor/LED GATT** — gated behind the certificate challenge-response
  (client cert signed by the company CA + fresh-nonce signature, per
  connection, non-bonding). Unauthenticated centrals get disconnected.
- **Image authenticity** — MCUboot only boots images signed with the MCUboot
  key. The Pi agent implements upload → trial-boot → health check → confirm,
  but current SS1 firmware still auto-confirms during startup. Rollback is
  therefore not yet controlled by the Pi health gate.

What is **open** (the gap this design closes):

- The **MCUmgr/SMP GATT service is unauthenticated**. Anyone in BLE range can
  connect and issue image-management commands: list images (fingerprinting),
  upload junk (flash wear, DoS-by-occupying-the-slot), reset the device in a
  loop, or attempt downgrades to an older signed image with known bugs.
- The **MCUboot signing key is the SDK dev key** — "signed" currently means
  "signed with a key anyone with the SDK has".
- **No downgrade protection** — any correctly-signed older version is
  accepted.
- **No stable device identity** — every unit advertises "Joseph_BLE" and the
  BLE address is random-static (stable-ish across reboots, not guaranteed
  across re-provisioning). Fleet code keys on the address as a stopgap.

## 2. OTA endpoint authentication (firmware, primary)

**Goal: reuse the existing per-connection certificate-auth flag to gate
SMP.** SS1 already tracks "this connection passed cert auth" for the sensor
gate, so OTA should require the same client certificate issued by `enroll.py`.

NCS 3.4 limitation verified against the installed source: the generic
`MGMT_EVT_OP_CMD_RECV` callback receives only group, command ID and operation.
It does **not** receive the BLE transport or `bt_conn *`, so it cannot safely
look up SS1's per-connection authentication flag by itself.

Implementation options to prototype and test:

1. **Preferred:** add a small application/upstream authorization hook in the
   MCUmgr BLE characteristic write path, where `smp_bt_chr_write()` has the
   actual `bt_conn *`. Reject writes unless
   `sentry_auth_is_authenticated(conn)` is true. Keep the patch narrow and
   version-controlled against the exact NCS release.
2. **No transport patch:** authenticated maintenance mode. After the Pi passes
   cert auth, stop advertising, disconnect other clients, and dynamically
   register the SMP service only for that single remaining connection. Remove
   it again on disconnect. This must be tested carefully for GATT service
   discovery/cache behaviour and reconnect races.

`CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS` plus
`CONFIG_MCUMGR_SMP_COMMAND_STATUS_HOOKS` can still provide a second policy
layer for allowed groups/commands, but it is not the per-connection identity
gate.

Why not the alternatives:

- `CONFIG_MCUMGR_TRANSPORT_BT_PERM_RW_AUTHEN` (require authenticated BLE
  pairing on SMP characteristics): SS1 deliberately runs Just Works LESC
  non-bonding — there is no BLE-level MITM authentication to lean on, and
  changing the pairing model breaks the current no-button UX.
- Registering/unregistering the SMP GATT service dynamically after cert auth:
  racy around reconnects, more moving parts, and service-changed indications
  confuse cached clients. The hook approach rejects cleanly at the protocol
  layer instead.

**Pi-side counterpart (blocked on the above):** `sentry_agent/ota.py`'s
`OtaClient` must perform the cert handshake on its own connection before
issuing SMP requests — same 3-step exchange as `session.py` (read nonce
char, sign, write cert‖signature), just over the OtaClient's BleakClient.
`smpclient`'s `SMPBLETransport` doesn't expose its underlying `BleakClient`
publicly, so plan A is a small subclass/patched transport that runs the
handshake post-connect; plan B is doing the handshake with a short-lived
plain BleakClient first — **won't work** (SS1 auth is per-connection), so
it must be plan A. Add `creds`-based auth to `OtaClient.__aenter__`.

## 3. Image-level hardening (firmware, independent of §2)

Do these regardless — they protect even if endpoint auth is bypassed:

1. **Replace the dev signing key**: `imgtool keygen -k ss1_mcuboot_key.pem -t
   ecdsa-p256`, point MCUboot at it (`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`),
   keep the private key with the CA key (offline, never on devices/repo —
   gitignore pattern `**/*_key.pem` already covers it). Note: first image
   signed with the new key must go on **by wire (west flash)**, not OTA —
   the running MCUboot still trusts only the old key.
2. **Downgrade prevention**: start with
   `SB_CONFIG_MCUBOOT_DOWNGRADE_PREVENTION` (semantic-version based) —
   software-only, adequate for our threat model today. Hardware security
   counters can come later if the product needs rollback protection against
   physical attackers.
3. Remove SS1's startup auto-confirm and test the existing **Pi-owned
   confirm** flow (trial + health check + revert-by-default). Until that
   firmware change lands, a failed Pi health check cannot guarantee rollback.
4. While in the area: root-cause the mid-upload disconnects (~1/3 attempt
   success; suspicion is blocking flash-erase starving the BLE link — try
   erasing the secondary slot *before* starting the transfer, or check
   `CONFIG_IMG_ERASE_PROGRESSIVELY`).

## 4. Device identity (firmware + Pi)

Goal: a per-unit ID that is stable across reflash, power cycles, address
rotation and re-provisioning, visible **before** connecting (so discovery
can tell units apart).

**Firmware plan:**

1. Read the SoC's factory unique ID through Zephyr's supported
   `hwinfo_get_device_id()` API (`CONFIG_HWINFO=y`) rather than accessing an
   nRF54 FICR security alias directly. The nRF backend returns the factory
   64-bit ID, which survives reflash and reprovisioning.
2. Advertise all 16 hexadecimal digits in the name, for example
   `Joseph_BLE-0123456789ABCDEF`. This still fits in the 31-byte scan-response
   payload and avoids introducing collisions by truncating the factory ID.
   It instantly fixes the
   "every unit looks identical in a scan" fleet blocker.
   - Privacy note: a static ID in advertising data makes units trackable.
     Fine for this product stage; revisit if that ever matters (the
     alternative — ID readable only post-auth — can't distinguish units
     pre-connect, which defeats fleet discovery).
3. Optionally also expose the same 64-bit ID via standard Device Information
   Service (0x180A / Serial Number 0x2A25), auth-gated like the sensors, for
   a verified read after connect.
4. Later (mutual auth): issue each **device** a certificate binding its ID,
   signed by the same CA, so the Pi can verify it's talking to a genuine
   unit and not something impersonating the advertised name. Today only the
   client authenticates to SS1; SS1 never proves itself to the Pi.

**Pi side (already implemented, §5):** `resolve_device_id()` now parses the
`Joseph_BLE-<ID>` suffix as the device_id when present, address fallback
otherwise; `fleet.py` discovery already prefix-matches so suffixed names are
picked up with no further change. When firmware lands, previously-seen
devices get a new inventory row keyed by the real ID and the old
address-keyed row goes stale (accepted, documented in the code).

## 5. Already done on the Pi side (this repo)

- `resolve_device_id()` suffix parsing (inventory.py) — device-ID-ready.
- Pi-native `sentry-agent` uses BlueZ/Bleak, a Unix-domain socket and a
  systemd service; it has no Windows/TCP/fake-agent fallback.
- Sequential per-adapter OTA queue, trial boot, sensor/cert-auth health check
  and explicit confirm command are implemented. The confirm gate is not yet
  authoritative because current SS1 firmware still auto-confirms at startup.
- The dashboard's pre-existing `--driver fake` mode remains available for UI
  development only; it is separate from the Pi fleet agent.

## 6. Suggested order of work

1. Firmware: advertise `Joseph_BLE-<16-HEX-ID>` (small, unblocks fleet).
2. Firmware: MGMT hook gating SMP on cert auth (§2).
3. Pi: OtaClient cert handshake (§2 counterpart — blocked on 2).
4. Firmware: new signing key + downgrade prevention (§3, needs one wired
   flash to roll the key).
5. Firmware: upload-disconnect root cause (§3.4).
6. Later: device certs / mutual auth (§4.4), MCUmgr os-group lockdown
   review, hardware security counters.
