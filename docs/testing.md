# Testing this fork

## What CI checks

**build** (every push and PR): all 63 board configs build with `-Werror`, then
`tools/mem_report.py` writes each board's free IRAM, static DRAM and flash code size to the job
summary. A board with under 2 KB of IRAM left fails. The universal builds, which carry every wired
driver, have about 6 KB left; most single-system builds have about 9 KB.

**pytest** (every push and PR): the firmware runs in QEMU and the tests drive it through the
WebSocket test interface. That covers everything from the Bluetooth HID report to the bytes a
wired driver would send, but not the radio or the wired drivers themselves. It runs on two builds:

| Build | Config | What it adds |
|---|---|---|
| qemu | `configs/dbg/qemu` | Coverage report |
| qemu_variants | `configs/dbg/qemu_variants` | N64 device identity following what is connected, default accessories |

Besides upstream's tests, the suite checks the following:

| Area | Test |
|---|---|
| Wiimote IR camera setup sent on pointer outputs only | `pytest_wiimote_ir_init.py` |
| Wiimote IR as a pointer and as a mouse | `pytest_wiimote_ir_controller.py` |
| CD-i pointer and mouse | `pytest_cdi_pointer.py` |
| PS1/PS2 GunCon from a Wiimote or a gamepad | `pytest_psx_guncon.py` |
| N64 mouse, and a pad driven by a mouse | `pytest_n64_mouse.py` |
| Commands from a console's program (0x7B) | `pytest_console_cmd.py` |
| Saturn Mission Stick and Arcade Racer | `pytest_saturn_mission_racer.py` |
| 3DO arcade pad | `pytest_3do_silly_pad.py` |
| Steam Controller (BLE), Poke Ball Plus, SInput | `pytest_steam_controller.py`, `pytest_pokeball_plus.py`, `pytest_sinput.py` |
| Xbox without a descriptor: rumble, and reuse of a slot | `pytest_xbox_classic_no_descriptor.py` |
| Keyboards: ISO/JIS keys, NKRO; N64 keyboard keys, LEDs, classic mode, Apple keyboards | `pytest_hid_keyboard.py` |
| **Everything else against upstream** | `pytest_upstream_golden.py` |

`pytest_upstream_golden.py` feeds the 11 HID descriptors the tests carry through all 23 wired
systems and 4 output modes: 1012 combinations with 8 reports each. Every stage (decoded, mapped,
wired) must equal what upstream 7c77f06 produced (`tests/golden/`), except for the changes the
fork made on purpose. Those are listed in the test, each with its reason. To re-record the golden
file, see the header of `tests/upstream_diff.py`.

## What only hardware can check

QEMU has no radio and does not run the wired drivers. The `BT_DISABLE` builds have no
Bluetooth controller, and the second core, which bit-bangs the console protocols, is never
started. Check these on real hardware first:

**Bluetooth**
- Pair and reconnect a BLE controller that uses LE Secure Connections.
- Reconnect a BLE controller that sends several reports per advertisement.
- A DualSense: rumble, then the accurate haptics (vibration v2).
- A Wiimote with the IR bar: the camera turns on only on pointer outputs (CD-i pointer, CD-i,
  PSX or N64 mouse).
- Restart, sleep and factory reset with controllers connected: each one is disconnected first.

**Wired output timing**
- CD-i: pointer and light gun position, mouse.
- N64: RANDnet keyboard, mouse, and with `N64_AUTO_ID_SWITCHING` a port that changes identity on
  the console's reset. Also the 0x7B commands from a ROM.
- Saturn: Mission Stick, Arcade Racer, keyboard lock keys, multitap mouse and keyboard.
- Genesis: XBAND keyboard, 6-button pad.
- 3DO: arcade pad.
- PS1 GunCon (Mouse + Rumble accessory) with a Wiimote: the console sees a GunCon (ID 0x63),
  the game's calibration lands on the sight, and a shot with the sensor bar out of view reloads.
  Time Crisis, Point Blank, on NTSC and PAL.
- PS2: multitap (Multitap = Alt), and rumble on every 0x42 poll.
- PCNT bit counting: on the systems that use it, no missed or doubled polls.

**Boards**
- iBlueControlMod GameCube (normal and `_safe`): power off, reset button, LEDs.
- MethyleneX PS2: OLED, touchpad menus, buzzer, BLE settings.
- RetroScaler single and 4-port adapters: the 4 status LEDs.
- WS2812 port LEDs.
