# OGX-Mini output (original Xbox, XInput, PS3, Switch)

BlueRetro has no USB device port, so it drives the USB side of an
[OGX-Mini](https://github.com/wiredopposite/OGX-Mini): a Raspberry Pi Pico (or Pico W, RP2040-Zero,
XIAO RP2040) running OGX-Mini's **ESP32 Bluepad32 I2C** build. The Pico takes a gamepad over I2C
and shows up on its USB port as an original Xbox controller (through the usual USB to Xbox
breakaway cable), an XInput pad, a DualShock 3 or a Switch Pro controller. BlueRetro speaks the
same I2C protocol as OGX-Mini's own ESP32 firmware, so an unmodified Pico works.

Flash the `hw1/ogx_mini` firmware (`hw1_ogx_mini` in the CI's hw1 artifacts) on a plain ESP32
board. A universal build has the system too, as system id 24, but the blueretro.io web config
doesn't list it.

## Wiring

| ESP32 | Pico (OGX-Mini ESP32 Bluepad32 I2C build) |
|---|---|
| GPIO21 (SDA) | GPIO18 |
| GPIO22 (SCL) | GPIO19 |
| GND | GND |

Power both from the same supply or from the console's USB through the Pico's VSYS. The ESP32's
internal pull-ups are enough for one Pico over short wires; for longer wires, or more than one
Pico, add 2.2 to 4.7 kOhm pull-ups from SDA and SCL to 3.3 V.

The other pins of OGX-Mini's ESP32 board (UART on Pico GPIO16/17, ESP_RST, ESP_PROG, MODE_SEL) are
only for flashing the ESP32 from the Pico and are not needed.

## Settings

Output 1 is the Pico at I2C address 0x01, which is every Pico running OGX-Mini's firmware as
released. Outputs 2 to 4 are Picos at 0x02 to 0x04; that needs a Pico firmware that reads an
address strap, which OGX-Mini does not have yet.

The output's **mode** in the web config picks what the Pico shows up as on USB:

| BlueRetro mode | Pico mode |
|---|---|
| GamePad | Original Xbox (Duke / Controller S) |
| GamePad Alt | XInput (Xbox 360) |
| Keyboard | DualShock 3 (PS3) |
| Mouse | Switch Pro |

BlueRetro sends the mode each time a Pico shows up and whenever the setting changes. A Pico that
gets a new mode stores it and reboots, so it drops off the console for about a second.

**Rumble**: with the output's accessory set to **Rumble** (or **Both**), what the console sends to
the Pico's two motors goes on to the Bluetooth controller, at its strength. The `hw1/ogx_mini`
build has Rumble set by default.

## What goes through

* Every button of an Xbox pad: A, B, X, Y, the bumpers (Black and White on the original Xbox),
  Start, Back, L3, R3, Guide (PS / Home) and Share (Capture).
* 16-bit sticks and 8-bit triggers.
* The pressure of the face buttons, bumpers and D-pad, from a DualShock 3; from any other
  controller a press reads full (255). OGX-Mini's Pico firmware as released ignores the pressure
  it is given on its I2C build and reads every press full, so the pressure only reaches the
  console with a Pico firmware that uses it.
* Turbo, on the buttons, triggers and sticks, as on every other BlueRetro output.
* Stick center calibration is off on this output: a stick held while the controller connects is
  not taken as its center. The console and its games have their own dead zones.

## How it works

`main/wired/ogxmini_i2c.c` is the I2C master. It runs on the FreeRTOS core with ESP-IDF's I2C
driver: the Pico takes a pad whenever one comes, so nothing needs the bare metal core's timing.
A pad goes out as soon as it changes, and on each 60 Hz tick when turbo changed it or the last one
is 100 ms old (a Pico that rebooted gets it back). Each tick also reads each Pico's rumble back.
Picos that don't answer are looked for again every second.

`main/adapter/wired/ogxmini.c` builds the pad from BlueRetro's generic controller, and
`main/adapter/wired/ogxmini_pad.c` holds the packets, the turbo and the pressure fill, with no
ESP-IDF dependency, so `tests/pytest_ogxmini.py` checks them on the host against the layout of
OGX-Mini's own structs. The same test file checks the pad built from a DS3 and from an Xbox
controller, and the rumble gating, in QEMU.

## Protocol

From OGX-Mini's `Firmware/RP2040/src/OGXMini/Board/ESP32_Bluepad32_I2C.cpp` (all packed):

* Write, 32 bytes: `packet_len` (32), `packet_id` (1 SET_PAD, 3 SET_DRIVER), `index` (0),
  `device_type` (SET_DRIVER only: 1 Xbox OG, 4 XInput, 5 PS3, 8 Switch, ...), then the pad: `dpad`
  (up 1, down 2, left 4, right 8), `buttons` (16 bits: A, B, X, Y, L3, R3, Back, Start, LB, RB,
  Guide, Misc), `trigger_l`, `trigger_r`, four `int16` sticks (Y down positive), `analog[10]` (up,
  down, left, right, A, B, X, Y, LB, RB), then 5 reserved bytes.
* Read, 8 bytes: `packet_len` (8), `packet_id` (2 GET_PAD), `index`, `rumble_l`, `rumble_r`, 3
  reserved.
