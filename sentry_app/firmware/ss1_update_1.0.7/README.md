# SS1 firmware 1.0.7 — SHT3x address auto-detection

This signed fleet OTA release probes the SHT3x at startup and selects either
address `0x44` (production SS1) or `0x45` (DK SS1). One firmware image can
therefore run on both hardware variants.

Expected serial output includes one of:

```text
Detected SHT3x sensor at address 0x44
Detected SHT3x sensor at address 0x45
```

SHA-256: `bbff71a704d67f411241168de5b42225c35f275daf9493bc7b90dbb9545dae94`
