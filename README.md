# PS4 Fan Control — GoldHEN AutoRun ELF

Target: **PS4 Pro CUH-7106B / firmware 12.52**

SDK: **ps4-payload-dev/sdk v0.9+**

## Purpose

This is a small test-oriented ELF payload for the PS4 `/dev/icc_fan` threshold interface.

It changes the **fan temperature threshold** used by the ICC autoservo. It is **not** a direct RPM/PWM controller.

## Test flow

```text
GoldHEN AutoRun
      |
      v
GET /dev/icc_fan (10-byte PS4 request)
      |
      v
Log the raw 10 bytes
      |
      v
Read /data/GoldHEN/fan_control.ini
      |
      v
Copy the GET result and change ONLY byte 5
      |
      v
SET the 10-byte request
      |
      v
GET again
      |
      v
Log the raw 10 bytes again
      |
      +---- mismatch/error ----> log + notification + exit
      |
      +---- verified ----------> log + notification + exit
```

This intentionally uses the **10-byte PS4 request** documented by established PS4 fan-threshold payloads. The temperature in Celsius is stored at **byte 5**.

## Files on the PS4

```text
/data/GoldHEN/autorun/fan_control.elf
/data/GoldHEN/fan_control.ini
/data/fan_control/fan_control.log
```

On first run, the INI is created automatically:

```ini
threshold=65
```

The payload accepts **60–80 °C**. Values outside that range fall back to 65 °C.

## Logging

Yes — logging is implemented.

The payload appends to:

```text
/data/fan_control/fan_control.log
```

The test build logs:

- the initial raw ICC GET bytes
- the threshold read from byte 5
- the exact 10 bytes that will be sent by SET
- the raw ICC GET bytes after the write
- verification success/mismatch
- configuration source and old/new thresholds
- failures and warnings

Example:

```text
[2026-09-14 04:30:12] [READ] ICC GET raw (10 bytes): 00 00 00 00 00 4F 00 00 00 08 | threshold=79C
[2026-09-14 04:30:12] [WRITE] ICC SET raw (10 bytes): 00 00 00 00 00 41 00 00 00 08 | threshold=65C
[2026-09-14 04:30:12] [VERIFY] ICC VERIFY raw (10 bytes): 00 00 00 00 00 41 00 00 00 08 | threshold=65C
[2026-09-14 04:30:12] [OK] Applied 65C (was 79C) | source=ini | ini=65C
```

The raw-byte logging is intentional for the first physical 12.52 test. It lets us compare what the CUH-7106B actually reports before and after the change.

## Build

Use the current `ps4-payload-dev/sdk` release (v0.9 or newer) and set:

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

The project uses the SDK's normal CRT/libc and `toolchain/orbis.mk` rather than a handwritten `_start()`.

## Important test note

This project deliberately does **not** claim that the exact `/dev/icc_fan` behavior on **CUH-7106B / Orbis 12.52** has been independently verified.

The first physical run should be treated as a diagnostic test. Check `fan_control.log` after the run and inspect the raw 10-byte GET result.
