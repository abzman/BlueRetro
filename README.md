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

Tips:

* For absolute outputs (CD-i GamePad Alt), set the deadzone of the left stick
  X/Y mappings to 0 % in the advanced mapping config, otherwise the pointer
  sticks around screen center. Then run the game's own gun calibration.
* Mouse speed follows the mapping's scale (max %) of the right stick X/Y.
* If vertical aim is inverted on your setup, swap the Y up/down mapping.
* With a Nunchuk or other extension plugged in, the Wiimote behaves as
  before and the camera stays off.

Adding another absolute pointing device (e.g. a tablet or touch screen
protocol) only needs the system to advertise `WIRED_PTR_ABS` for its mode
in `wired_ptr_caps()` (`main/adapter/wired/wired.c`) and to read the
absolute position from the LX/LY axes. CRT-timed light guns (Zapper,
Menacer, GunCon, ...) are a different problem and are not covered.

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

