# SS1 sensor modes

SS1 firmware 1.0.16 exposes its operating mode and sampling interval through
the authenticated Smart Sentry GATT service. The Pi dashboard is the intended
configuration interface. Settings are stored in flash and survive power loss.

## Modes

| Value | Dashboard name | SERIAL30 role | Sensor path |
|---:|---|---|---|
| 0 | Debug | UARTE30 TX on P0.00 | Onboard SHT4x on GPIO I2C, SDA P0.04 / SCL P0.03; DK SHT3x fallback |
| 1 | Normal I2C | TWIM30 | Onboard SHT4x on SDA P0.04 / SCL P0.03; DK SHT3x fallback; application UART silent |
| 2 | External Auto | Unused | External SHT4x first on SDA P1.03 / SCL P1.11, then 10 kOhm B3950 NTC on P1.11/AIN4 |

SERIAL30 cannot be UARTE30 and TWIM30 simultaneously. A mode change is written
to settings, acknowledged over BLE, and followed by a cold reboot. The new
owner is initialized only during the next boot.

The external thermistor assumptions are 10 kOhm at 25 C, beta 3950 K and the
PCB's 10 kOhm pull-up to 3.3 V. A thermistor supplies temperature only;
humidity is reported as unavailable rather than as zero.

## Sampling interval

The setting is an integer from 0 through 30 seconds. Zero means the fastest
safe rate of 500 ms. Values 1 through 30 mean that many seconds. Changes apply
without a reboot.

## Authenticated GATT configuration

All UUIDs use suffix `1111-2222-3333-123456789abc`:

- `7e8f0003`: one-byte mode, read/write
- `7e8f0004`: one-byte interval seconds, read/write
- `7e8f0005`: eight-byte sensor status, read

Sensor status is little-endian `<BBBBHH>`: mode, sensor kind, flags, error,
effective interval milliseconds, reserved. Flag bit 0 means temperature is
valid, bit 1 humidity is valid, and bit 2 a mode reboot is pending.

No synthetic fallback measurements are generated. A failed or missing sensor
sets the corresponding validity flag false, excludes the failed sample from
averages, suppresses its notification, and makes a direct read return an ATT
error. The Pi polls sensor status so the dashboard shows unavailable data and
the diagnostic reason.
