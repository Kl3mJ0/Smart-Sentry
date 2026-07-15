# SS1 firmware 1.0.5 — production-key bootstrap

- `ss1_1.0.5_bootstrap.hex` contains MCUboot plus the signed application.
  Flash this once by wire to move an existing board from the Nordic
  development signing key to the Smart Sentry production public key.
- `blinky.signed.bin` is the BLE OTA payload for all later devices that
  already run the production-key MCUboot.

Recommended first install from the NCS 3.4.0 environment:

```powershell
west flash -d build
```

Or load `ss1_1.0.5_bootstrap.hex` in Nordic Programmer. This wired bootstrap
also replaces MCUboot; do not try to send the bootstrap HEX through MCUmgr.

SHA-256:

- OTA BIN: `e7df27d2b80dad36fbc20e32c073d7caad0796f39cd60f2972d0e5af43c6168d`
- bootstrap HEX: `917c86a6ac1641a80879ee03897718eb8466a5f782ec96ca28d218d00ff8fc80`
