# amaran-bridge guide

Everything about building, flashing and running the bridge. For the overview, see the [README](../README.md).

## What it does

- On/off, brightness, colour temperature, hue/saturation, green/magenta tint
- Reads state back from the fixtures, so Home Assistant survives a reboot and
  picks up changes made from the amaran app or a control panel
- Availability that means something: a fixture is available only if it
  answered us, not merely if it is powered on
- Home Assistant auto-discovery, plus live diagnostic sensors
- Control and diagnostics over MQTT (`amaran/bridge/cmd` → `amaran/bridge/reply`)

## Requirements

- ESP32-C5 devkit
- ESP-IDF **v5.5.2 or newer** — earlier versions cannot build for the
  production C5 silicon
- A patch applied to your ESP-IDF checkout (below) — not optional
- An MQTT broker that Home Assistant is already using
- The amaran desktop app installed and paired with your lights, once, to
  create the mesh and the database this reads

## The ESP-IDF patch

```bash
python3 patches/apply-net-hook.py ~/esp/esp-idf
```

Idempotent, keeps a `net.c.orig` backup, and must be re-run after an ESP-IDF
upgrade. Without it the firmware still builds and runs — it just never learns
anything about the fixtures, which shows up as every light reporting
`reply_age -1` and staying permanently unavailable.

**Why it is unavoidable.** Fixtures address their status replies to the
provisioner address (`0x0001`) and to nobody else. ESP-IDF's mesh stack drops
any PDU whose destination is not one of its own element addresses *before the
access layer* — `trans_unseg()` returns early on `!rx->local_match`, and
`bt_mesh_rpl_check()` does the same — so no amount of model registration can
catch them. No public API exposes "a packet from node X was decrypted".

The patch adds two weak hooks to the network receive path and changes nothing
else; routing and relaying are untouched.

| Hook | Fires on | Used for |
|---|---|---|
| `amaran_mesh_net_rx(src, dst, ctl)` | every network-decrypted PDU | liveness |
| `amaran_mesh_access_rx(src, dst, data, len)` | app-decrypted Telink payloads | real state |

This node also takes unicast address **`0x0001`**, so those replies are
addressed to it. **Do not run the amaran desktop or iPad app at the same
time** — it uses that address too, and two nodes sharing one will interfere.

## Setup

```bash
cp main/config.h.example main/config.h   # wifi + mqtt credentials, then edit
python3 tools/gen_mesh_config.py         # mesh keys + fixtures, from the app
python3 patches/apply-net-hook.py ~/esp/esp-idf
./flash.sh
```

`gen_mesh_config.py` reads the amaran desktop app's SQLite database
(`~/Library/Application Support/amaran Desktop/*/amaran.db`) for the mesh keys
and each fixture's unicast address. Re-run it whenever fixtures are added,
removed or renamed — a stale address means commands go to nobody, silently.

Per-fixture colour and CCT limits are not in that database. They live in
`tools/capabilities.json`; copy `tools/capabilities.example.json` to start one.
Fixtures missing from it get a permissive default, which at worst shows a
colour wheel for a fixture that has no RGB emitters.

`main/config.h` and `main/mesh_config.h` are gitignored. Between them they hold
your Wi-Fi PSK, MQTT password, and the mesh network and application keys —
treat a leaked `mesh_config.h` as handing over control of every light.

Wi-Fi is pinned to 2.4 GHz; a 5 GHz SSID will not associate.

## Home Assistant

Fixtures appear automatically via MQTT discovery within a few seconds of boot.
Each becomes a light entity, and fixtures that support tint also get a
green/magenta `number` entity.

Topics, for reference or manual use:

```
amaran/<fixture>/set          JSON command   {"state":"ON","brightness":128}
amaran/<fixture>/state        JSON state
amaran/<fixture>/gm/set       green/magenta, -10..10
amaran/<fixture>/available    online | offline
amaran/bridge/availability    the bridge itself
amaran/bridge/diagnostics     JSON, every 30s
```

## Control and diagnostics

There is no serial prompt. Publish to `amaran/bridge/cmd` and read
`amaran/bridge/reply`:

| Command | Does |
|---|---|
| `status` | node address, sequence number, mesh/wifi/mqtt state |
| `seen` | per fixture: BLE advert age and mesh reply age, seconds (`-1` = never) |
| `probe` | ask every fixture to report itself |
| `raw <addr> <on\|off>` | send to any address, bypassing the fixture table |
| `seqbump [n]` | jump the outbound sequence number forward |

`seen` is the diagnostic that matters. `adv_age` means the fixture is powered
on and in radio range; `reply_age` means it heard us and answered. A fresh
advert with no reply means present but not listening.

## Gotchas

**Sequence numbers.** Fixtures keep a replay-protection list and silently
discard any message whose sequence number is not strictly greater than the last
one they recorded from that sender (Mesh spec 3.8.8). The counter lives in NVS,
so `erase-flash` restarts it at zero and every fixture that remembers a higher
value ignores the bridge — the send returns success and the light never moves.
`AMARAN_SEQ_START` is the floor; `seqbump` is the live escape hatch.

**IV index.** The stack ignores any Secure Network Beacon whose IV index is
below its own or more than 42 above it, so a node provisioned at 0 against a
network that has advanced past 42 never syncs — silently, in both directions.
`AMARAN_IV_INDEX_INITIAL` must be close to the network's real value; pass
`--iv` to the generator to set it.

**The vendor opcode is one byte.** Telink uses `0x26` as a single-byte opcode,
not the standard three-byte vendor encoding. The model registers both forms,
but only the one-byte form matches what fixtures actually send.

**Software AES is mandatory.** `CONFIG_MBEDTLS_HARDWARE_AES=n`. With hardware
AES the mesh stack performs a DMA-capable heap allocation for every 16-byte
block, hundreds per second; it eventually fails with `Encrypt failed (err -22)`
and the mesh stops dead.

**Advertising buffers are the transmit budget.** Each transmission holds a
buffer for its whole retransmit span. Queue faster than the pool drains and the
stack logs `Out of network buffers` and discards commands *before they reach
the air*, which looks exactly like a fixture being out of range. Hence
`ADV_BUF_COUNT=120`, three network copies, two application repeats, and the
60–70 ms gaps in `mesh.c`. Those gaps are flow control, not politeness.

**Wi-Fi power save is required.** `WIFI_PS_MIN_MODEM` — software coexistence
hands the BLE controller the idle slots Wi-Fi gives up there. With
`WIFI_PS_NONE` the radio never yields and fixtures stop hearing the bridge.

**`esp_wifi_set_band_mode()` must be called after `esp_wifi_start()`**, or it
returns `ESP_ERR_WIFI_NOT_STARTED` and the surrounding `ESP_ERROR_CHECK`
reboots the board in a loop.

**Console input on USB-Serial-JTAG wedges the peripheral.** Making USB the
primary console gives an interactive prompt and then, after a few keystrokes,
kills log output *and* esptool until the BOOT button forces download mode.
UART0 stays primary and USB is an output-only mirror, which is reliable. That
is why control lives on MQTT.

**Mesh identity survives a reflash.** It lives in NVS, so a board that
previously ran different firmware returns with that firmware's unicast address.
`mesh.c` compares against `AMARAN_NODE_ADDR` and re-provisions on a mismatch.

**Retained MQTT messages outlive the firmware that published them.** Stale
discovery configs keep re-creating entities that nothing feeds — ghost lights,
and diagnostic sensors frozen at whatever they last reported. Retained
*command* topics are worse: the broker replays them on every subscribe, so
lights switch themselves on. This firmware clears its own command topics at
connect; to clear a stale discovery config, publish an empty retained payload
to its `.../config` topic. Give each board a unique `DEVICE_ID` as well — two
MQTT clients sharing an id evict each other in a loop, and every eviction fires
the last will, so entities flap available/unavailable every few seconds.

## Development loop

`./devloop.sh` turns the repository folder into a command bus: it tails serial
output into `.devloop/serial.log`, runs command files dropped into
`.devloop/in/`, and writes results to `.devloop/out/`. Verbs: `build`, `flash`,
`reset`, `logclear`, `sh`.

The `sh` verb runs arbitrary shell commands as the current user. Stop the
script when you are done with it.

## Layout

```
main/telink.c    the 10-byte vendor payloads and their decode
main/mesh.c      self-provisioning mesh node, transmit queue, receive hooks
main/ha.c        wifi, mqtt, discovery, state, availability, admin commands
patches/         the required ESP-IDF patch
tools/           mesh_config.h generator and fixture capabilities
```

`telink.c` is byte-exact against hardware and reverse-engineered; changes there
should be tested against real fixtures rather than reasoned about.
