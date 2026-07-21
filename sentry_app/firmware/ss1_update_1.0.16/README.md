# SS1 firmware 1.0.16 — selectable sensor modes

Production-key-signed fleet OTA release for production SS1 and the nRF54L15 DK.

- **Debug:** UART30 debug output on P0.00 and GPIO-driven onboard I2C.
- **Normal I2C:** hardware TWIM30 drives the production SHT41 with no application
  serial output.
- **External Auto:** detects an external SHT4x or reads a common 10 kΩ B3950
  thermistor on the external sensor connector.
- The Pi app can select the mode and a 0–30 second reading interval. Interval
  zero means the firmware's safe 500 ms minimum.
- Invalid or unavailable sensor readings are reported as unavailable; fabricated
  fallback measurements are not generated.

SHA-256: `fc0e707ed98848fb931e4b4acf9dd8fec751b9ae35591d3cd764706d0dfe7af3`

MCUboot image digest: `a772d0bfcba2da8fe2e5af30ec63e2cc068f835ec8fb448b67ff4216740fdd6e`
