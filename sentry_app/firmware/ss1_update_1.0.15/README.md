# SS1 firmware 1.0.15 — production and DK sensor routing

Production-key-signed fleet OTA release for both SS1 hardware variants.

- Production PCB: powers the sensor rail from P2.03, uses GPIO I2C on
  SDA P0.04 / SCL P0.03, and emits readable CRLF debug output on UART30 TX
  P0.00.
- nRF54L15 DK: retains hardware TWIM21 on SDA P1.11 / SCL P1.12.
- Automatically detects the production SHT41 at `0x44` or DK SHT3x at
  `0x44`/`0x45` and sends the same authenticated BLE packet format.

SHA-256: `cc9fc3a092751ef7fa53173c3ab034f1798b2c43da47af4ce1f087a324b50404`

MCUboot image digest: `ee7949b654ff8af1cf9dcfe94baf4046a0c2a22c509ba96739855ca57a5070c2`
