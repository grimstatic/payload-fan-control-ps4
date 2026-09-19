# PS4 Fan Control Payload

An **ELF** alternative to **PS4 Temperature** by Lapy, LM, Zeco, AlAzif, Zer0xFF

**SDK:** `ps4-payload-dev/sdk` v0.9+

## Important
This was tested on PS4 Pro CUH-7106B / firmware 12.52 / Only PS4 I have

## How it works
1. Reads the current fan setting.
2. Reads `fan_control.ini`.
3. Changes only the temperature value at byte 5.
4. Writes the setting back.
5. Reads it again to verify the change.
6. Logs the results and shows a notification.
7. Exits.

## Config
The payload automatically creates fan_control.ini:

```text
/data/fan_control/fan_control.ini
```
Threshold can be change from 60 - 80
```text
# PS4 Fan Control
# Temperature threshold in degrees Celsius.
# Safe configuration range used by this payload: 60-80 C.
# Default: 65 C.
threshold=65
```


## Logging
The payload automatically creates the log directory and writes to:

```text
/data/fan_control/fan_control.log
```

The log records:

- payload startup
- ICC reads
- configuration results
- ICC writes
- verification
- successful completion
- warnings
- errors and failed operations
- raw ICC bytes before and after a write

## To Build
Install `ps4-payload-dev/sdk` v0.9 or newer and set:

```sh
export PS4_PAYLOAD_SDK=/opt/ps4-payload-sdk
```

Then:

```sh
make clean
make
```
The project uses the SDK's normal CRT/libc and `toolchain/orbis.mk`.

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

Please see the original projects for their licenses and attribution requirements.
