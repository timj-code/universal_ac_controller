# BC7215 Matter ESP32-C3 — HDC2010 + LED PWM update

Based on the five source files supplied on 2026-11-20. **Not yet compiled with your local ESP-IDF / ESP-Matter checkout; hardware test required.** No change to BC7215 library, existing NVS pairing record, or existing Matter endpoint layout except the optional humidity cluster.

## Installation

Copy `app_main.cpp`, `app_driver.cpp`, `app_priv.h`, `CMakeLists.txt`, and the unchanged `idf_component.yml` into the project's **main/** directory (make a backup first). `CHANGES.patch` is included for reviewing the differences. Keep your existing files in `components/`, `deps/`, root `CMakeLists.txt`, `sdkconfig`, etc. Do not delete them. `CMakeLists.txt` adds ESP-IDF 5.5 driver component dependencies for the I2C and LEDC APIs.

Build in the configured ESP-IDF / ESP-Matter shell:

```sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Hardware (aligned with ESPHome 1.7.2)

- BC7215 UART1: TX GPIO4, RX GPIO3, BUSY GPIO5, MOD GPIO6.
- HDC2010: I2C0, 100kHz, SDA GPIO7, SCL GPIO2, address 0x40. Requires 3.3V supply and appropriate I2C pull-ups.
- LED: GPIO8, active-low, LEDC at 1kHz PWM. Use GPIO9 for existing `iot_button` via board configuration.

## Matter attributes

- Room Air Conditioner endpoint retains Thermostat (`0x0201`) and the separate Fan endpoint remains unchanged.
- **Thermostat LocalTemperature (0x0000)** receives the measured HDC2010 temperature in units of 0.01 C, instead of mirroring target temperature.
- **Relative Humidity Measurement (0x0405) / MeasuredValue (0x0000)** is added to the existing Room AC endpoint in 0.01% RH units (0–10000). Do not expect all phone apps to display this optional attribute.
- Sensor is sampled about every 2 seconds. Read/write operations run in a sensor FreeRTOS task, and the Matter attributes are updated from the CHIP task using `PlatformMgr().ScheduleWork`.
- The measured local temperature starts null until a valid reading. On a sensor error, the last valid reading is retained, and a warning is logged. A failed initial probe does not prevent climate control but does prevent future sensor readings until reboot.
- In ESP-Matter versions with a code-driven Relative Humidity Measurement server, use its `SetMeasuredValue` integration API, not legacy `attribute::report`; headers are detected at build time. See SDK compatibility note below.

## LED behavior

- No Matter fabric AND BC7215 unpaired: steady on.
- Matter commissioning or Wi-Fi offline/reconnecting: 1Hz (500ms on/off).
- Wi-Fi connected, BC7215 not paired: one 100ms flash every 2 seconds.
- No Matter fabric, BC7215 paired: two 100ms flashes every 2 seconds.
- Wi-Fi connected and BC7215 paired: **smooth three-second breathing cycle** with LEDC PWM (30ms updates).
- IR pairing: ~3Hz flash; Alt protocol traversal: existing count feedback; factory reset hold: 3Hz warning. GPIO9 short/double/5s long press retains original meaning. **No User Function support added.**

Note: a Matter fabric is the "network configured" marker for the no-configuration pattern. A device can have a Wi-Fi credential but no fabric; it still displays the uncommissioned pattern. Unlike the previous implementation, an established Wi-Fi connection does not require an active subscription for the breathing LED.

## First-run tests

1. Boot and check: `HDC2010 ready: address=0x40 SDA=7 SCL=2 ID=0x07D0`.
2. Inspect repeating: `HDC2010: 24.31 C, 53.22 %RH` (values will vary).
3. Query Thermostat LocalTemperature and RelativeHumidityMeasurement MeasuredValue with a Matter controller. Confirm ambient temperature stays constant when changing AC setpoint.
4. Verify LED smooth 3s breathing; test unpaired, offline, IR pairing, protocol traversal and long-press reset.
5. If humidity appears null while logs show readings, check your ESP-Matter release and whether the `relative-humidity-measurement-server` code-driven `SetMeasuredValue` path is present. The fallback attribute path is meant for legacy releases only.

## Limitations and dependencies

- No local ESP-IDF, ESP-Matter, BC7215, or device build environment was supplied, so the code has **not** been compiled against the exact SDK or tested on hardware.
- Code-driven cluster APIs vary across ESP-Matter releases. If you use a release whose humidity integration header or config differs, the corresponding adapter may need adjustment. The new files intentionally retain both legacy and modern update paths to reduce this risk.
- On GPIO8, ESP32-C3 strapping behavior matters: avoid external circuitry that forces an unsuitable level during boot.
- A platform such as Apple Home may or may not show the optional humidity attribute on the AC tile even when the Matter cluster correctly reports values; verify with a Matter attribute reader.
