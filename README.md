# BlueRetro (fork)

**This is a fork of [darthcloud/BlueRetro](https://github.com/darthcloud/BlueRetro),
which was archived on 2025-12-14.** It tracks upstream `e1a9831` ("Farewell") and
carries changes that are not in mainline and never will be, since mainline no
longer accepts contributions. Upstream's own closing notice is reproduced below.

## What this fork changes

**Toolchain**

* **Ported from ESP-IDF v5.5.0 to v6.1.** Touches interrupt setup, startup,
  GPIO and every wired-protocol driver that talks to RMT/SPI/UART, plus the
  `CONFIG_NEWLIB_*` → `CONFIG_LIBC_*` rename and a CMake minimum of 3.22. The
  tree no longer builds against 5.5.0, so the inherited upstream workflows still
  pinned to the `idf-blueretro:v5.5.0_2024-12-02` image will not run.

**Hardware**

* **HW2 SNES iBlueControl board overlay**: new `configs/hw2/ibluecontrol_snes`
  target, Kconfig entry, LED and power-management handling, built by CI
  alongside every other target.

**Switch 2 controllers.** Upstream shipped SW2 support in `v25.10-beta` and was
archived weeks later, leaving several defects in that path unfixed. This fork
consolidates the independent community fix lines and adds work none of them had.
Ported with attribution from [RyanCopley](https://github.com/RyanCopley/BlueRetro),
[Last-Colossi](https://github.com/Last-Colossi/BlueRetro),
[bjerreman](https://github.com/bjerreman/BlueRetro-Switch2Fix) and
[LaserBear Industries](https://github.com/LaserBearIndustries/BlueRetro_LBI):

* **SPI read responses are correlated to their request.** A duplicated or
  out-of-order ack used to shift the init state machine by one, storing the
  controller's ASCII serial number as its long-term key: the "only one NSO
  GameCube pad ever works" failure. *(RyanCopley)*
* **Stale keys recover instead of bricking a pad.** The LE LTK is cleared when a
  BLE link drops before HID init, which produces no `ENCRYPT_CHANGE` and so
  never tripped the old recovery path. *(RyanCopley)*
* **Unpaired BLE controllers are visible again** once anything is connected; the
  passive scan no longer filters on the accept list. *(RyanCopley)*
* **Per-device ACL fragment reassembly**, a 20 s connect-time LE supervision
  timeout, and cleanup of the BLE config interface on disconnect. *(RyanCopley)*
* **Phantom stick and calibration integrity.** Non-`RSP` acks are skipped, user
  calibration is validated against its `0xA1B2` magic and rejected when
  implausible, input is gated until calibration loads, and the per-device
  calibration cache is cleared so one controller cannot inherit another's
  centres. *(Last-Colossi)*
* **A static deadzone on the NSO GameCube analog triggers**, which idle a few
  counts above neutral. *(Last-Colossi, trimmed from 9 to 8)*
* **Up to four simultaneous controllers.** Inbound reconnects get a real device
  slot, `LE_LTK_REQUEST` is answered, and advertising stops at two BLE links and
  resumes when a slot frees. *(bjerreman)*
* **A failed pairing no longer steals a controller's port.** Slots are handed out
  lowest-first and the slot index *is* the console port, so anything that
  advertised and failed pushed real controllers to port 2. Addresses are now
  parked after three strikes, a 10 s watchdog reclaims a slot that stalls before
  HID init, and an SMP `Pairing Failed` is deliberately *not* treated as fatal,
  because the NSO pad sends one and then completes pairing anyway. *(LaserBear)*
* **A controller switched off frees its port in ~5 s, not ~20 s**, by tightening
  the LE supervision timeout once init is done. *(LaserBear)*
* **Melee-safe combo defaults on GameCube builds**: `L+R+A+Start` resets a match
  in Super Smash Bros. Melee and was bit-for-bit the stock power-off combo.
  *(RyanCopley; GameCube builds only)*

Not carried by any of those forks:

* **The HD-rumble keepalive is always sent**, not only when rumble is enabled.
  A Pro Controller 2 or Joy-Con 2 drops its link without a steady stream of
  output frames, so turning rumble off used to take the controller down with it.
* **Joy-Con 2 left/right product IDs corrected.** `0x2067` is the left half and
  `0x2066` the right; upstream and darthcloud issue #1249 have them swapped.
* **Joy-Con 2 output reports have the right shape.** They carry one LRA, not the
  Pro Controller 2's two, so the command header belongs 16 bytes earlier.
  Upstream used the Pro template and then never sent the frame, which hid it.
* **A solo Joy-Con 2 half works as its own controller**, held sideways, the way
  a Switch 1 Joy-Con already does here. Upstream left the mapping stubbed out.
* **An unrecognised Switch 2 controller falls back to the Pro Controller 2
  layout** instead of delivering no input at all.
* **The connection watchdog is disarmed when a device slot is freed**, closing a
  window where it could fire against whichever controller next took that slot.
* **Key-derivation instrumentation** for the pairing exchange, and a cross-check
  of the advertised product ID against the one read from SPI flash.

**Wiimote as a pointing device.** The Wiimote's IR camera now drives
pointer outputs. See [Wiimote pointer](#wiimote-pointer) below.

**Changes absorbed from other forks.** See
[Absorbed from other forks](#absorbed-from-other-forks) below.

## About BlueRetro

<p align="center"><img src="/static/PNGs/BRE_Logo_Color_Outline.png" width="600"/></p>
<br>
<p align="justify">BlueRetro is a multiplayer Bluetooth controllers adapter for various retro game consoles & computers. Lost or broken controllers? Reproduction too expensive? Need those rare and obscure accessories? Just use the Bluetooth devices you already got! The project is open source hardware & software under the CERN-OHL-P-2.0 & Apache-2.0 licenses respectively. It's built for the popular ESP32 chip. Wii, Switch, PS3, PS4, PS5, Xbox One, Xbox Series X|S & generic HID Bluetooth (BR/EDR & LE) devices are supported. Parallel 1P (Computers, NeoGeo, Supergun, JAMMA, Handheld, etc), Parallel 2P (Atari 2600/7800, Master System, Computers, etc), NES, PCE / TG16, Mega Drive / Genesis, SNES, CD-i, 3DO, Jaguar, Saturn, PSX, PC-FX, JVS (Arcade), Virtual Boy, N64, Dreamcast, PS2, GameCube & Wii extension are supported with simultaneous 4+ players using a single adapter.</p>

## Wiimote pointer

A bare Wiimote (no extension) aimed at a Wii sensor bar can act as a
pointing device. The IR camera is only switched on when the output the
Wiimote is mapped to uses a pointer, so it costs no battery otherwise.

| Output (web config mode) | What the Wiimote emulates | Buttons |
|---|---|---|
| **Mouse** on any system with mouse support (SNES, PS1/PS2, N64, Saturn, Genesis, Dreamcast, PCE, PC-FX, 3DO, CD-i, ...) | Mouse: aiming moves the cursor | B = left click, A = right click |
| **GamePad Alt** on CD-i | Absolute coordinate device (Peacekeeper light gun) | B / 1 = button 1, A / 2 = button 2 |
| **Mouse** + **Rumble** accessory on PS1/PS2 (**Both** accessories keeps the memory card) | Namco GunCon (Time Crisis, Point Blank, Die Hard Trilogy 2, ...) | B = trigger, A / + = A, 1 / 2 / - = B; D-pad held or no sensor bar in view = aiming off the screen (the reload shot) |
| **GamePad Alt** on the SNES's **2nd port** (experimental) | Super Scope (Super Scope 6, Yoshi's Safari, Battle Clash, T2: The Arcade Game, ...) | B = Fire, A = Cursor, + / 2 = Pause, - / 1 = flip the turbo switch; D-pad held or no sensor bar in view = off the screen |

Tips:

* For absolute outputs (CD-i GamePad Alt, GunCon, Super Scope), set the deadzone of the left stick
  X/Y mappings to 0 % in the advanced mapping config, otherwise the pointer
  sticks around screen center. Then run the game's own gun calibration.
* Mouse speed follows the mapping's scale (max %) of the right stick X/Y.
* If vertical aim is inverted on your setup, swap the Y up/down mapping.
* The GunCon reports the NTSC picture's range. On a PAL console the game's
  calibration has less room to work with: calibrate, and if the shots land
  off vertically, say so in an issue.
* The GunCon also works from a gamepad: the left stick aims across the
  whole screen.
* With a Nunchuk or other extension plugged in, the Wiimote behaves as
  before and the camera stays off.

The Super Scope is experimental: it has not been tried on a console yet. A
real scope pulls the port's pin 6 (IOBit) low when its sensor sees the beam,
and the SNES latches where the beam was. The adapter can't see the picture,
so it works out when the beam will be under the aim from the polls: they
come once a frame, at a point that moves around within a known window, and
the adapter tracks the frame from the earliest of them
(`main/wired/beam_timing.c`). That takes about 3 seconds after the game
starts reading the port. Things to know:

* It only works on the 2nd port: the 1st port's pin 6 doesn't reach the
  latch. GamePad Alt on the 1st port is a plain pad.
* The light pulse only goes out while Fire or Cursor is held on the screen,
  as with the real scope. Run the game's sight calibration first.
* In a model of the SNES's timing (`tests/pytest_beam_timing.py`) the aim
  stays within about 2 dots of the beam on NTSC and PAL. A real console may
  differ.
* A game that reads the port several times a frame at varying points, or
  doesn't read it every frame, may never be tracked; the scope then reports
  its buttons but never hits.
* Pin 6 has to reach the console both ways. If your adapter's port wiring
  only lets it in (as the SNES rumble protocol needs), the pulse can't go
  out.

Adding another absolute pointing device (e.g. a tablet or touch screen
protocol) only needs the system to advertise `WIRED_PTR_ABS` for its mode
in `wired_ptr_caps()` (`main/adapter/wired/wired.c`) and to read the
absolute position from the LX/LY axes, as the CD-i and the GunCon do. An output
that can aim off the screen reads `CTRL_PTR_OFFSCREEN` in the controller's
flags, set while the Wiimote sees no sensor bar. Guns whose sensor the
console times against the beam (Zapper, Menacer, Justifier, ...) need the
timing on the console's side, as the Super Scope does; `beam_timing.c` is a
start for that.

## Original Xbox through an OGX-Mini

An [OGX-Mini](https://github.com/wiredopposite/OGX-Mini) Pico wired to the ESP32's I2C pins
(GPIO21/22) gives BlueRetro a USB output: original Xbox, XInput, PS3 or Switch, picked by the
output's mode. The `hw1/ogx_mini` build talks to an unmodified Pico running OGX-Mini's ESP32
Bluepad32 I2C firmware; rumble goes back to the controller with the Rumble accessory set, and a
DualShock 3's pressure buttons keep their pressure. See [docs/ogx_mini.md](docs/ogx_mini.md) for
the wiring and settings.

## N64 keyboard and commands

From bherdm/BlueRetroForever. Set an N64 output to **Keyboard** mode and a
Bluetooth keyboard on it is a RANDnet keyboard: games and homebrew get the
RND-001 read (0x13), and libdragon gets an extended read (0x2A) carrying up
to ten held keys as HID usages, media and system keys, and Apple's Fn. The
console's lock LEDs are sent back to the keyboard. Design notes are in
[`docs/keyboard-design.md`](docs/keyboard-design.md) and
[`docs/classic-keyboard-design.md`](docs/classic-keyboard-design.md).

Each port has keyboard settings, saved with the config (config version 4;
flashing an older firmware back resets the config to defaults):

* **Mode:** full layout (every key by position, both reads) or classic
  (an RND-001 for retail software: 0x13 only, characters translated from
  the layout).
* **Layout:** US, UK, DE, FR or JP. **Arrangement:** Mac or PC.

They are set over the USB serial port (`kb <1-4> <full|classic>
<us|uk|de|fr|jp> <mac|pc>`), from the BLE output config (5 bytes instead of
2; an app writing 2 bytes leaves them alone), or by an N64 program. The
serial port also takes `inq start`, `inq stop`, `disconnect`, `status` and
`help`.

N64 programs can send BlueRetroForever's command **0x7B** on any port in any
mode to read the status block (pairing, what is connected on each port),
open or close pairing, disconnect a port, and read or set the keyboard
settings. See [`docs/n64-commands-design.md`](docs/n64-commands-design.md)
and the test ROM in `tests/n64_console_cmd/`.

Differences from BlueRetroForever:

* **Auto device identity switching is off by default**
  (`CONFIG_BLUERETRO_N64_AUTO_ID_SWITCHING`). With it on, each port reports
  the type of the device actually connected, so a keyboard on a port left in
  the default gamepad mode turns into a RANDnet keyboard, which retail games
  ignore. Turn it on in menuconfig if you want that.
* **No LED level.** The Bluetooth LED keeps upstream's pulse; the 0x7B LED
  level verb answers "unknown verb" and the status block's LED level is 0.
* **The BLE config command for the console system is 0x09** (0x08 in
  BlueRetroForever), since 0x08 reads the port LED config here.

## Board builds

Builds for specific adapters and internal boards, each a config in `configs/`
built by CI next to the regular ones. They only change anything on their own
builds; the regular hw1 / hw2 firmware is unaffected.

| Board | Config | Notes |
|---|---|---|
| AliExpress **iBlueControlMod** GameCube internal board (V1.1) | `hw2/gamecube_ibluecontrolmod`, `hw2/gamecube_ibluecontrolmod_safe` | Flashable over Bluetooth from the stock firmware at blueretro.io/ota.html. Rumble on by default, reset button and sleep mode. The `_safe` build lowers the Bluetooth TX power and keeps the radio quiet for 20 s after boot, for boards whose 3.3 V supply browns out. Full guide: [`tools/ibluecontrolmod/README.md`](tools/ibluecontrolmod/README.md) ([Deutsch](tools/ibluecontrolmod/README.de.md)). |
| **RetroScaler** 4-port adapters (N64, Dreamcast, GameCube) | `hw1/n64_retroscaler`, `hw1/dreamcast_retroscaler`, `hw1/gamecube_retroscaler` | Drives the four BT status LEDs. Rumble is the default accessory (Dreamcast: Jump Pack, plus the VMU on port 1), written once into an existing config so no reset from the web config is needed; other settings and mappings are kept. |
| **RetroScaler** single-port adapters | `hw1/{3do,genesis,nes,parallel_1p,parallel_2p,playstation,saturn,snes}_retroscaler` | Reconstructed from the vendor's binaries. |
| **MethyleneX** PS2 internal board | `hw2/playstation_methylenex` | 128x64 OLED menus navigated with a DualShock 4 touchpad, screensaver, buzzer, and the board's settings over the BLE config interface. Its BLE commands 0x15 to 0x17 exist only on this build (other builds use those IDs for the port LEDs). |

ESP32-MINI based adapters need no config of their own: the Bluetooth LED pin
is picked from the chip package.

## Absorbed from other forks

Fixes and features picked from other forks of BlueRetro, with their original
authors kept on each commit. Each commit message names its source fork and
any change made while porting it to ESP-IDF 6.1.

| From | What |
|---|---|
| [bherdm/BlueRetroForever](https://github.com/bherdm/BlueRetroForever) | Cancel an LE connect the peer never answers, retry a failed SW2 SPI read, ignore unrequested SET_LED / pairing acks, disconnect logging; OTA: writes without response (~3x faster updates), errors for a rejected image, full firmware name/version reads; disconnect every peer (config interface included) before restart / sleep; QEMU heap fix; N64: RANDnet keyboard with both reads (0x13 and libdragon's extended 0x2A), media/system keys, lock LEDs back to the Bluetooth keyboard, per-port layout (US/UK/DE/FR/JP, Mac/PC) and classic RND-001 mode, ISO/JIS keys, commands from N64 programs (0x7B: pairing, status, disconnect, keyboard settings), per-port device identity (optional auto switching), N64 mouse acceleration; pairing / status / disconnect / keyboard settings over the USB serial port |
| [tassiolucas/BlueRetro](https://github.com/tassiolucas/BlueRetro) | DS4 clones that refuse HID until the link is encrypted (QRD Spark N5, Maestro S3, Fantech Nova II...) |
| [azediawan/BlueRetro](https://github.com/azediawan/BlueRetro) | Log refused L2CAP connections |
| [rpf16rj/BlueRetro](https://github.com/rpf16rj/BlueRetro) | DualSense accurate haptics (vibration v2); PS1/PS2 rumble queued on every poll again |
| [pummra/BlueRetro](https://github.com/pummra/BlueRetro) | BLE reconnect when several devices share one advertising event |
| [jjtcm/BlueRetro](https://github.com/jjtcm/BlueRetro) | LE Secure Connections pairing (ported to PSA Crypto, used only when a peer refuses legacy pairing); 2026 Steam Controller (Triton) over BLE; `tools/br_trace.py` HCI trace decoder; sturdier memory card debug reads |
| [arcticlula/BlueRetro-PS2-Multitap](https://github.com/arcticlula/BlueRetro-PS2-Multitap) | PS2 multitap on port 1 with Multitap = Alt (slot lines on GPIO 18/23 from an sd2psx helper, or inferred from poll order) |
| [Scyllinice/BlueRetro](https://github.com/Scyllinice/BlueRetro) | Genesis: level-driven 3/6-button pad emulation (fixes dropped inputs and C/Start flicker with fast readers) |
| [DoomyDoomer/BlueRetro](https://github.com/DoomyDoomer/BlueRetro) | SNES: count clocked bits with PCNT and only apply commands of a sane length (darthcloud's post-archive work); Bluetooth LED pin on ESP32-MINI modules (Humble Bazooka's change) |
| [Modac/BlueRetro](https://github.com/Modac/BlueRetro) | WS2812 per-port status LEDs on N64/GC (off by default, configured over the BLE config interface) |
| [andrew171717/BlueRetro-PokeBall-Plus](https://github.com/andrew171717/BlueRetro-PokeBall-Plus) | Poké Ball Plus over BLE |
| [loneshxdow/BlueRetro](https://github.com/loneshxdow/BlueRetro) | GameSir Nova Lite 2 (descriptor-less Xbox layout) with rumble |
| [adamhleslie/BlueRetro](https://github.com/adamhleslie/BlueRetro) | SInput gamepads over Bluetooth (handled as generic HID, SDP first) |
| [eaudunord/BlueRetro](https://github.com/eaudunord/BlueRetro) | Saturn: Mission Stick and Arcade Racer emulation, keyboard lock keys and scancode fixes, mouse/keyboard behind the multitap; Genesis: XBAND keyboard |
| [RobertDaleSmith/BlueRetro](https://github.com/RobertDaleSmith/BlueRetro) | 3DO: the arcade SILLY_CONTROL_PAD (coin/start/service, used by Orbatak) on the Keyboard mode |
| [konsolenschatz/BlueRetro-iBlueControlMod](https://github.com/konsolenschatz/BlueRetro-iBlueControlMod) | AliExpress iBlueControlMod GameCube internal board (V1.1), with an English / German guide |
| [opssemnik/Blueretro-for-retroscaler-4port-1port](https://github.com/opssemnik/Blueretro-for-retroscaler-4port-1port) | RetroScaler adapters: 4-port N64 / Dreamcast / GameCube with their BT status LEDs, single-port configs, per-port accessory defaults |
| [akbkarimi/BlueRetro](https://github.com/akbkarimi/BlueRetro) | MethyleneX PS2 internal board: OLED menus driven from a DualShock 4 touchpad, screensaver, buzzer, settings over BLE |

### Looked at and not taken yet

These changes were reviewed while absorbing the forks above and deliberately
left out for now. Each one is listed with where to find it, so it can be
picked up later once the reason below is resolved.

#### 1. bherdm/BlueRetroForever: shorter LE supervision timeout

* Repo: [bherdm/BlueRetroForever](https://github.com/bherdm/BlueRetroForever), branch `main`
* Commit: [`64f852c`](https://github.com/bherdm/BlueRetroForever/commit/64f852cdb06a859fbb50e0cd525c4f9e5f8d7bca)
  "Keep upstream's LE supervision timeout and radio policy after a connect"
* What it does: goes back from a 20 s to upstream's 3 s LE supervision
  timeout, and drops the advertising/scan policy applied right after an LE
  connection.
* Why not now: it fixes a first-time NSO GameCube controller pairing on
  bherdm's bench, but it undoes the 20 s timeout and radio policy that this
  fork's Switch 2 multi-controller support was built on (a second SW2
  controller's bring-up starved the first link at 3 s). The two results
  contradict each other and neither has been reproduced here. Take it only
  after testing pairing and reconnecting with two or more SW2 controllers.
  The one part of it another commit needs, starting the LE connect timer,
  is already included with "Cancel an LE connect the peer never answers".

#### 2. DoomyDoomer/BlueRetro: rest of darthcloud's SNES / NES series

* Repo: [DoomyDoomer/BlueRetro](https://github.com/DoomyDoomer/BlueRetro), branch `snes-isr-preload`
  (the first two commits are also on `master`)
* Commits, in order (all by darthcloud except the last):
  * [`ffc724a`](https://github.com/DoomyDoomer/BlueRetro/commit/ffc724ab24eed72b7e0a07ddb829e58f919dc6e7)
    "FC/NES variant of the SNES analog/rumble mode"
  * [`c434001`](https://github.com/DoomyDoomer/BlueRetro/commit/c4340011d8fb9042883b4be81e6d0ed916aecc6a)
    "Use D1 for P2 for consistency"
  * [`b1c861f`](https://github.com/DoomyDoomer/BlueRetro/commit/b1c861f21f826dee4efa21c31d520af22657a607)
    "[SNES] Resolve random press"
  * [`a8c230c`](https://github.com/DoomyDoomer/BlueRetro/commit/a8c230c6e1ee331f1594b719328652894b6f3d20)
    "[NPISO] Inverse data polarity, handle analog trigger in separate field"
  * [`a75e253`](https://github.com/DoomyDoomer/BlueRetro/commit/a75e253074f4df549368f5380137578bc9f95fa7)
    "fix: pre-load SPI buffer to eliminate LATCH ISR latency for single adapter running on P2" (DoomyDoomer)
* What it does: fixes random button presses on SNES by inverting the SPI data
  lines, cuts latch latency, and moves NES from the npiso driver to the SNES
  SPI driver to give it the SNES analog/rumble mode.
* Why not now: the commits only work as a set, and the set breaks other
  systems. `a8c230c` inserts a field at the start of the npiso output and
  inverts its polarity, but the Virtual Boy still reads that output through
  the old npiso driver, so it would read wrong data. Moving NES to the SNES
  SPI driver (`ffc724a`) drops the Four Score / Hori multitap, the Famicom
  keyboard and the trackball, which only the npiso driver supports.
  DoomyDoomer runs these on an SNES-only build, where none of that shows.
  To take them, port the Virtual Boy path (and NES, if it stays on npiso) to
  the new output format, or keep npiso for NES whenever one of those
  accessories is selected. The first commit of the series, PCNT bit
  counting, is self-contained and is already included.

#### 3. eaudunord/BlueRetro: Saturn default remap and global trigger change

* Repo: [eaudunord/BlueRetro](https://github.com/eaudunord/BlueRetro), branch `saturn-keyboard-standard-map`
* Commits: [`5592b2a`](https://github.com/eaudunord/BlueRetro/commit/5592b2a40192c52b8a7a6cfed3b911471a877504)
  and [`520d5b8`](https://github.com/eaudunord/BlueRetro/commit/520d5b88ae92fc90eb3139021a82ca5f8cfb7036)
  (the rest of both is included)
* What was left out: moving Saturn L/R to other source buttons and widening
  every Saturn pad's mask, a default Xbox-to-Saturn button remap in
  `config.c`, a config patch that forced keyboard + mouse on Saturn multitap
  pairs and rewrote their mouse mapping on every load, and a change to
  `adapter_map_from_axis()` that treats trigger axes differently on every
  system.
* Why not now: each changes defaults or mappings for existing users (the
  last for every console), not only for the new devices. They can come back
  as opt-in settings.

## Switch 2 controller support

Supported over Bluetooth LE: **Pro Controller 2** (`0x2069`), **NSO GameCube**
(`0x2073`), and **Joy-Con 2** left (`0x2067`) and right (`0x2066`). A controller
type this build does not recognise falls back to the Pro Controller 2 layout
rather than delivering no input.

There is no Switch 2 NSO SNES, N64, NES or Mega Drive controller. Those pads are
Switch 1 devices on Bluetooth BR/EDR, supported by a different code path
entirely and unaffected by any of this.

Known limitations:

* **Pairing a Switch 2 controller to BlueRetro unpairs it from a Switch 2
  console.** The controller stores one host pairing, so the console's entry is
  overwritten. Re-sync on the console (cable or the sync button) to use it there
  again. This is a property of the controller, not of the adapter.
* **Joy-Con 2 halves work one per player, held sideways**, like Switch 1
  Joy-Cons. Merging a left and right half into a single controller is not
  implemented.
* **Two simultaneous BLE controllers is the practical radio limit.** The ESP32
  cannot advertise, scan and service links at once, so advertising stops once
  two are connected and resumes when a slot frees. Pairing several controllers
  at the same moment can still collide; pair them one at a time.
* **Motion, mouse and headset-audio features are not implemented.**

## GameCube builds: combo buttons differ from upstream

Super Smash Bros. Melee resets a match on **L + R + A + Start**, which is exactly
upstream's `SYS_POWER_OFF` combo, so every match reset also tells the adapter to cut
power. GameCube builds therefore base the combos on `PAD_MQ` rather than `PAD_MM`
(Start), and swap reset and power off onto A and B:

| Combo | Action |
| --- | --- |
| L + R + Capture + A | System reset |
| L + R + Capture + B | System power off |
| L + R + Capture + X | Bluetooth pairing toggle |
| L + R + Capture + Y + D-pad Up | Factory reset |
| L + R + Capture + Y + D-pad Down | Deep sleep |

`PAD_MQ` is the Capture button on a Switch 2 GameCube pad, the Capture button on a
Switch Pro pad, and the touchpad click on a DS4/DualSense. **Controllers with no
`PAD_MQ` — notably the Wii U Pro and PS3 pads — cannot satisfy the combo base and
lose every combo on a GameCube build.** Remap `COMBO BASE 3` to another button in the
web config to restore them. Non-GameCube builds keep the upstream defaults.

## READ THIS FIRST
* [Project documentation](https://github.com/barrenechea/BlueRetro/wiki)

## Need help?
* [Open a GitHub discussion](https://github.com/barrenechea/BlueRetro/discussions)

## Makers sponsoring BlueRetro
Buying BlueRetro adapters from these makers helps support the continued development of the BlueRetro firmware!\
Thanks to all sponsors!

* [Laser Bear Industries](https://www.laserbear.net)
* [Humble Bazooka](https://www.humblebazooka.com)
* [RetroOnyx](https://www.retroonyx.com/)
* [RetroTime](https://8bitmods.com/retrotime)

## Community Contribution
* BlueRetro PS1/2 Receiver by [mi213](https://twitter.com/mi213ger): 3D printed case & PCB for building DIY PS1/2 dongle.\
  https://github.com/Micha213/BlueRetro-PS1-2-Receiver
* N64 BlueRetro Mount by [reventlow64](https://twitter.com/reventlow): 3d printed mount for ESP32-DevkitC for N64.\
  https://www.prusaprinters.org/prints/90275-nintendo-64-blueretro-bluetooth-receiver-mount
* BlueRetro Adapter Case by [Sigismond0](https://twitter.com/Sigismond0): 3d printed case for ESP32-DevkitC.\
  https://www.prusaprinters.org/prints/116729-blueretro-bluetooth-controller-adapter-case
* BlueRetro AIO by [pmgducati](https://github.com/pmgducati): BlueRetro Through-hole base and cable PCBs.\
  https://github.com/pmgducati/Blue-Retro-AIO-Units
* BlueRetro HW2 internal guides by [Nostalgic Indulgences](https://twitter.com/nosIndulgences): Internal install guides\
  https://github.com/nostalgic-indulgences/BlueRetro_Internal_Installation
* BlueRetro latency test by [GamingNJncos](https://twitter.com/GamingNJncos): Documentation on how to run BlueRetro latency test\
  https://github.com/GamingNJncos/BLE-3D-Saturn-Public/tree/main/BlueRetro_Latency_Testing
* BR4N64 by [TharathielCB](https://github.com/TharathielCB): Internal BlueRetro Flex-PCB for Nintendo 64\
  https://github.com/TharathielCB/BR4N64
* BlueMemCard by [ChrispyNugget](https://github.com/ChrispyNugget): Replacement PCB for PSX memory card that incorporates PicoMemcard and BlueRetro support\
  https://github.com/ChrispyNugget/BlueMemCard
* BlueRetro HW2 QSB for GameCube by [Arthrimus](https://github.com/Arthrimus): Internal Blueretro PCB with CurrentTrigger for GameCube\
  https://github.com/Arthrimus/BlueRetro-HW2-GameCube

<br><p align="center"><img src="https://cdn.hackaday.io/images/4560691598833898038.png" height="200"/></p>

## Upstream's closing notice

*darthcloud, 2025-12-14:*

```
After 6 years working on BlueRetro, the time has come for me to move on
and focus on my family and my real job.

The code remains available as-is for reference, learning, and community use,
but no new features, bug fixes, or pull requests will be accepted.

Thank you to everyone who contributed, tested, reported issues,
or supported the project over the years.
Your involvement made BlueRetro what it is today.

If you are looking to build upon this work, please feel free to
fork the repository in accordance with the license.

Thank you,
Jacques Gagnon
```

