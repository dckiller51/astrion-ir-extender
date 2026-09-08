# 📡 Astrion IR Extender

A network-connected, transmit-only infrared blaster for
[Astrion Custom Dashboard](https://github.com/dckiller51/astrion-custom-dashboard).
Place one inside a closed cabinet, behind a TV, anywhere your remote's own
built-in IR blaster can't reach in a straight line — the app sends the
command over WiFi instead, and the extender fires the real IR signal from
wherever you've put it.

**Fully standalone.** No Home Assistant, no cloud, no account. One generic
firmware, flashed once, works for every user — the app talks to it directly
over your local network.

> **Status:** hardware bring-up in progress. This README documents the
> firmware side; the "Devices" screen that discovers and registers
> extenders in the app itself hasn't shipped yet.

## Hardware

Any ESP8285-based WiFi IR blaster module (the generic kind commonly sold
as "Tasmota IR remote" on AliExpress/Amazon) — the same hardware family
already validated for
[astrion-ir-sniffer](https://github.com/dckiller51/astrion-ir-sniffer).

## Flashing

**Quick install (recommended — no Docker, no ESPHome install):**

1. Download the latest `.bin` from [Releases](https://github.com/dckiller51/astrion-ir-extender/releases).
2. Connect the module to your computer via USB.
3. Open <https://web.esphome.io> in Chrome or Edge (WebSerial support
   required), click **Connect**, select the serial port.
4. Choose to install from a local file, and pick the `.bin` you downloaded
   (not "Prepare for first use" — that installs ESPHome's own generic
   firmware, not this project's).

That's it — no yaml, no compiling, nothing else to install.

## Building from source (only if you want to modify the yaml)

`web.esphome.io` on its own can't compile a custom yaml like this one — it
only flashes an already-built `.bin`, which is exactly what the Quick
install section above uses. These options are only needed if you're
changing `esphome/astrion-ir-extender.yaml` yourself (a different GPIO
pinout, extra sensors, etc.) and need to produce your own `.bin`.

**Option A — ESPHome Dashboard (works well on Windows):**

Compiles inside the Docker container, but the actual USB flash step
happens through your browser's WebSerial connection straight to the
device, so Docker never needs direct access to the serial port (the usual
pain point passing USB devices into Docker Desktop on Windows).

```bash
docker run --rm -it -p 6052:6052 -v "${PWD}/esphome:/config" esphome/esphome
```

Then, in Chrome or Edge:

1. Open <http://localhost:6052>.
2. `astrion-ir-extender.yaml` shows up automatically (it's in the mounted
   `esphome/` folder).
3. Click **Install** → **Plug into this computer** → pick the serial port.

**Option B — ESPHome CLI, compile + flash in one command:**

Only works smoothly if Docker actually has access to the serial device —
reliable on Linux, often flaky through Docker Desktop on Windows (prefer
Option A there).

```bash
docker run --rm -it -v "${PWD}/esphome":/config -v /dev/ttyUSB0:/dev/ttyUSB0 --device=/dev/ttyUSB0 esphome/esphome run astrion-ir-extender.yaml
```

Adjust `/dev/ttyUSB0` to whatever serial port your module enumerates as.

**Option C — compile only, then flash via web.esphome.io:**

```bash
docker run --rm -v "${PWD}/esphome:/config" esphome/esphome compile astrion-ir-extender.yaml
```

The resulting `.bin` lands under
`esphome/.esphome/build/astrion-ir-extender/.pioenvs/astrion-ir-extender/`
(exact path can shift between ESPHome versions — if it's not there, search
for `firmware.bin` under `esphome/.esphome/build/`). Flash it the same way
as the Quick install section above.

## First boot — WiFi setup

The firmware ships with no network credentials baked in (it's one generic
binary for everyone), so on first boot — or any time it can't reach the
last network it knew — it opens its own temporary hotspot instead:

1. On your phone/computer's WiFi settings, connect to **`Astrion IR
   Extender Setup`** (password `astrion1234`).
2. A setup page should open automatically (captive portal); if not, open
   <http://192.168.4.1> yourself.
3. Enter your real WiFi network's name and password.
4. The extender reboots and joins your network. Reconnect your phone/
   computer to your normal WiFi.

> The hotspot's name and password are the same on every unit. Fine for a
> single extender, but if you're setting up more than one at the same
> time on the same network, you currently can't tell them apart during
> this step — see [Known limitations](#known-limitations).

## Finding it again afterwards

Once it's joined your real network, the extender advertises itself over
mDNS (`astrion-ir-extender.local`) like any ESPHome device, and exposes:

- `GET /text_sensor/astrion_ir_extender_mac_address` — its MAC address,
  for manually registering it in the app if network discovery doesn't
  find it automatically (blocked multicast, VLANs, etc).
- `GET /text_sensor/astrion_ir_extender_ip_address` — its current IP.
- `POST /text/pronto_trigger/set?value=<pronto hex code>` — fires that
  Pronto code out the IR LED immediately. This is the endpoint the app
  will call to actually transmit a command once the "Devices"/extender
  support lands.

You can try that last one by hand right now with `curl` to confirm the IR
transmitter itself works, before any app-side support exists:

```bash
curl -X POST "http://astrion-ir-extender.local/text/pronto_trigger/set?value=0000%20006D%200027%200000%20..."
```

(URL-encode the spaces in the Pronto code as `%20`, or quote/encode the
whole value depending on your shell.)

## Factory reset

Press and hold the onboard button for 5 seconds. This erases all stored
settings, including the saved WiFi credentials, and puts the extender back
into setup-hotspot mode — use this if you're moving it to a different
network, or if it ever gets stuck unable to reconnect.

## Known limitations

- **Multiple units, same setup hotspot name/password.** Onboarding two or
  more extenders around the same time is ambiguous right now — no way to
  tell one temporary hotspot from another. Needs a real fix (e.g. a
  MAC-derived suffix) before this is recommended for multi-unit setups.
- **GPIO pin assumptions not yet hardware-verified**: the physical
  button (GPIO0) and status LED (GPIO13) pins are carried over from
  `astrion-ir-sniffer.yaml`'s pinout for the same module family, but
  haven't been individually confirmed working on this specific firmware
  yet. The IR transmitter pin (GPIO4) *is* already confirmed, since it's
  the same one already validated by the sniffer project.
- **No app-side support yet.** Registering an extender, mDNS discovery,
  and choosing "local vs this extender" per IR device in Astrion Custom
  Dashboard are all still on the roadmap, not shipped.

## Related projects

- [Astrion Custom Dashboard (Android app / APK)](https://github.com/dckiller51/astrion-custom-dashboard)
- [Astrion IR Sniffer (capture tool / IR database)](https://github.com/dckiller51/astrion-ir-sniffer)
- [HA Astrion Custom Dashboard (Home Assistant custom component)](https://github.com/dckiller51/ha-astrion-custom-dashboard)
