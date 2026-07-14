# Smart Sentry

Smart Sentry is a Nordic nRF54L15 Bluetooth LE system with:

- `blinky/` — SS1 sensor/peripheral (`Joseph_BLE`), SHT3x readings, authenticated
  GATT access and Channel Sounding reflector.
- `Blonky/` — SS2 receiver/central, authenticated reconnect handling and Channel
  Sounding initiator.
- `initiator/` and `reflector/` — Nordic-derived Channel Sounding reference
  applications retained for comparison.
- `sentry_app/` — Linux/Pi dashboard work, maintained on its own feature branch.

## Branches

- `main` — hardware-tested stable SS1/SS2 Channel Sounding and power-cycle
  reconnect behavior.
- `feature/ss1-ble-ota` — SS1 MCUboot + MCUmgr/SMP firmware updates over BLE.
- `feature/sentry-app` — the separate Linux/Pi Sentry application.

The firmware targets nRF Connect SDK 3.4.0 and the
`nrf54l15dk/nrf54l15/cpuapp` board target.

## Credentials

Private CA/client keys, generated credential headers and local application
credentials are intentionally excluded from Git. Provision fresh development
identities locally with `enroll.py`; never commit production private keys.
