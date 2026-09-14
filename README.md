# PS4 Fan Control Payload

A small **ELF payload** for testing the PS4 `/dev/icc_fan` temperature threshold.

**Target:** PS4 Pro CUH-7106B / firmware 12.52 / Only PS4 I have
**SDK:** `ps4-payload-dev/sdk` v0.9+

## What it does

The payload:

1. Reads the current fan setting.
2. Reads `fan_control.ini`.
3. Changes only the temperature value at byte 5.
4. Writes the setting back.
5. Reads it again to verify the change.
6. Logs the results and shows a notification.
7. Exits.

It changes the **temperature threshold used by the PS4's fan controller**. It is not a direct RPM/PWM controller.

## Simple flow

```text
GoldHEN AutoRun
      ↓
GET current ICC data
      ↓
Read fan_control.ini
      ↓
Change byte 5 only
      ↓
SET ICC data
      ↓
GET again
      ↓
Verify
   ↙      ↘
 FAIL      OK
  ↓         ↓
log       log
 +        +
notify   notify
  ↓         ↓
exit      exit
```

## Files on the PS4

```text
/data/GoldHEN/autorun/fan_control.elf
/data/GoldHEN/fan_control.ini
/data/fan_control/fan_control.log
```

The INI is created automatically on first run:

```ini
threshold=65
```

Allowed range: **60–80°C**. Values outside the range fall back to 65°C.

## Logging

The payload automatically creates the log directory and writes to:

```text
/data/fan_control/fan_control.log
```

the log includes:

- raw ICC data before the change
- the current threshold
- the exact data sent to SET
- raw ICC data after the change
- verification result
- configuration source
- errors and warnings

## Build

Install `ps4-payload-dev/sdk` v0.9 or newer and set:

```sh
export PS4_PAYLOAD_SDK=/opt/ps4-payload-sdk
```

Then:

```sh
make clean
make
```

Output:

```text
fan_control.elf
```

The project uses the SDK's normal CRT/libc and `toolchain/orbis.mk`.

## GitHub Actions

A GitHub Actions workflow is included in:

```text
.github/workflows/build.yml
```

Push the project to GitHub, open **Actions**, run the build, and download the generated `fan_control.elf` from the workflow artifacts.

## Credits

This project uses ideas, research, and tooling from the PS4 homebrew community:

- **Scene-Collective** — `ps4-fan-threshold`  
  https://github.com/Scene-Collective/ps4-fan-threshold

- **Zer0xFF** — PS4/PS4 Pro ICC and fan-control research  
  https://gist.github.com/Zer0xFF/4aa38d836a5696ed1b6486bb8e782b4a

- **ps4-payload-dev contributors** — PS4 payload SDK  
  https://github.com/ps4-payload-dev/sdk

- **ps4-payload-dev contributors** — `elfldr`  
  https://github.com/ps4-payload-dev/elfldr

- **GoldHEN contributors** — GoldHEN payload environment  
  https://github.com/GoldHEN/GoldHEN

Please see the original projects for their licenses and attribution requirements.

## Important

This is a **test build**. The project does not claim that the exact `/dev/icc_fan` behavior on every CUH-7106B running 12.52 has been independently verified.

Check `fan_control.log` after the first run and review the raw ICC data before using the payload regularly.
