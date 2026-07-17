# Proto0 User Guide

This guide will introduce you to the RGB Sunglasses Proto0 hardware.

Please read this entire page before working with Proto0! There are only 5 boards
in existence and I cannot currently make more. This guide will help you work with
the hardware safely!

## Hardware Overview

![Annotated Proto0 Hardware](https://github.com/user-attachments/assets/0122a8e3-596f-4ef4-ab50-09779bb1ce73)

1. (Red) — Glasses LED Panel Connector: a Molex 2.54 mm SL 5-pin connector to attach to the glasses LED panel.
2. (Purple) — Battery Connectors (2×): JST PH connectors for 2× lithium-polymer 3.7 V batteries. Samples are included with each devkit.
3. (Blue) — Battery Charger Status LED: a simple LED controlled by the battery charger. See the BQ25752 documentation for blink-pattern descriptions.
4. (Teal) — System Status LEDs: two independently controlled status LEDs. While charging, one will indicate charge status.
5. (Green) — D-Pad Buttons: Up, Down, Left and Right. Intended to control animations. Currently, only the GLIM player uses the buttons. The "Left" button can be used to enter DFU mode (see [Firmware Recovery](/recovery)).
6. (Yellow) — Recessed Reset Button: press this button to reset the MCU if it's stuck.

There is a USB-C port on the side of the case, near the System Status LEDs,
accessible via a cutout.

## Important Proto0 Hardware Notes

- There is a third white connector accessible on the Proto0 hardware. It is the same connector type as the battery connectors. **Do not attach a battery to this connector. You will damage the battery charger.** This connector will be plugged on all shipped units to prevent accidental battery connection. **Do not remove the plug.**
- Do not connect or disconnect the Glasses LED Panel while the system is powered on. Disconnect the USB-C cables and both batteries before connecting or disconnecting the LED panel.
- If you disassemble the case, the buttons will fall out. When re-assembling the case, **please note that the reset button pin is shorter than all other button pins to recess it below the case lid**.
- Your hardware may have a different jumper configuration than what is shown in this example image. Do not remove any jumpers installed on the Proto0. They are critical for operation of the hardware. Removing a jumper may result in hardware damage. If you need guidance on re-configuring the hardware, consult @skalldri.

## Unboxing and Setup

### Unboxing and Hardware Setup

When you receive your Proto0 hardware it will include:

- 1× Proto0 compute pack, installed in a case
- 1× Glasses LED Panel
- 3× Glasses LED Panel cables, of varying lengths
- 2× 3.7 V 2200 mAh lithium-polymer batteries

For day-to-day development, the compute pack can be powered from the USB-C port.
Battery charging and thermal management is not yet supported: a future firmware
update will enable battery charging.

Connect the short Glasses LED cable between the Proto0 compute-pack connector and
the matching connector on the Glasses LED panel.

Connect a USB-C cable between your PC and the Proto0 compute pack. The glasses will
power on within 10 s. The Glasses LED panel should pulse blue to indicate the
glasses are waiting for a Bluetooth connection.

### Bluetooth Pairing (first time)

> Note: currently, Android is the only supported app platform.
> [iOS support is planned](https://github.com/skalldri/rgb-sunglasses/issues/65)
> and help is wanted with testing!

#### Android

Install the latest app release from
[the Releases page](https://github.com/skalldri/rgb-sunglasses/releases).

Open the app and accept the required permissions to begin Bluetooth scanning.

The Proto0 board should be detected in the companion app. Tap "Connect" to begin
the connection process.

Pin-code pairing is required with the Proto0 board. Android will prompt you to pair
with the new device. This prompt can appear and disappear quickly. Check the
swipe-down system notification tray to see if there is a notification prompting you
to pair.

You may receive two prompts to pair. The first will not require a PIN code. After
this prompt, a PIN code will be required. When this stage is reached, the Glasses
LED panel will begin scrolling a 6-digit PIN code. Enter this code into the dialog
and accept to complete pairing.

After this initial pairing with your phone, you will no longer need to enter a PIN
code to connect to the Proto0 in future.

Once pairing is complete, the device state will be synced with the companion app.
You will be able to start interacting with the app to select animations to play.

## Using Batteries

_Battery charging and thermal management are not yet enabled in firmware. This
section will be expanded once battery support ships._

## USB Interface

You can connect the Proto0 hardware to your PC via the USB-C cable. The Proto0
hardware is a composite USB device that exposes several USB endpoints to your PC.

The Proto0 hardware uses Zephyr's default VID:PID `2fe3:0001`.

### Virtual Serial Ports

Proto0 exposes two serial ports to your PC. The exact naming of these serial ports
depends on how your PC enumerates the device. Typically, on Unix-based systems, they
are exposed as `/dev/ttyACM0` and `/dev/ttyACM1`.

- **ACM0**: Zephyr console. Exposes a shell for development / debugging, as well as application logs.
- **ACM1**: MCUmgr port. A non-human-readable binary protocol that speaks MCUmgr. Used for firmware updates.

Both ports are configured for 115200 baud, but given they are virtual ports this
has no effect.

### USB Mass Storage

The Proto0 hardware exposes a small 3.9 MB USB mass-storage device. This is
pre-formatted as a FAT16 filesystem. **Do not attempt to reformat this disk.**

The Proto0 device ships with a `glim` folder as the only contents of the filesystem.
This folder contains GLIM files which are dynamically read by the GLIM animation. You
can create your own GLIM files and place them in this folder. On the next reboot, the
GLIM animation re-scans this folder and exposes any animations to the companion app.

## Firmware Update

The glasses' firmware can be updated a few ways:

- **Over Bluetooth (OTA)** — the easiest for end users. The companion app checks GitHub
  for new firmware releases and offers to install them: connect to a device and open the
  **Firmware Update** page. (BLE OTA is slow — see
  [Issue 59](https://github.com/skalldri/rgb-sunglasses/issues/59).)
- **Over USB (MCUmgr)** — the fast path for developers. See
  [Developer Setup → Flash the firmware](/developer-setup).
- **Bricked / won't boot?** Recover it over USB with MCUboot's DFU mode — see
  [Firmware Recovery](/recovery).
