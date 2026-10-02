# Cardputer-CPM

CP/M on the **M5Stack Cardputer Adv**, using RunCPM as the emulation core and the Cardputer's built-in screen, keyboard and microSD card as the primary machine interface.

## Current status

The current build is a working proof of concept:

- Z80-compatible CP/M environment
- CP/M boots directly on the Cardputer Adv
- Built-in Cardputer keyboard works as the local console
- Built-in 240x135 display works as the local console
- microSD-backed CP/M drives
- Drives A: through P: map to ordinary directories on the SD card
- CP/M user areas are represented as subdirectories
- Real CP/M `.COM` programs execute successfully
- USB CDC console works with ordinary terminal emulators such as PuTTY
- Console can be switched between LOCAL, USB and BOTH modes
- Physical **Fn + =** always forces the console back to LOCAL mode

Programs already tested include standard CP/M utilities such as `DIR`, `STAT` and `PIP`.

## Hardware

- M5Stack Cardputer Adv
- microSD card
- USB connection for flashing and optional USB CDC console

The SD interface currently uses:

| Signal | GPIO |
| --- | ---: |
| SCK | 40 |
| MISO | 39 |
| MOSI | 14 |
| CS | 12 |

## Development environment

The project is built with **PlatformIO** using the Arduino framework for ESP32-S3.

The main PlatformIO environment is:

```text
m5stack-cardputer
```

Dependencies are defined in `platformio.ini`.

## SD card layout

CP/M drives are ordinary directories on the microSD card rather than disk-image files.

For example:

```text
A/0
B/0
C/0
...
P/0
```

The first directory level is the CP/M drive and the second is the CP/M user area.

So:

```text
A/0/STAT.COM
```

appears to CP/M as `STAT.COM` on drive A:, user 0.

Additional user areas can be created as required:

```text
A/1
A/2
...
A/15
```

This makes it possible to manage CP/M files directly from another computer by inserting the SD card and copying normal files.

## Console modes

The machine currently supports three console-routing modes.

### LOCAL

The Cardputer screen and keyboard are the CP/M console.

This is the default at boot.

### USB

CP/M console I/O is routed through USB CDC. A normal serial terminal emulator can be used on the host computer.

The development setup has been tested at:

```text
115200 baud
8 data bits
no parity
1 stop bit
no flow control
```

### BOTH

Output is sent to both the Cardputer and USB consoles for diagnostic use.

## Console switching commands

Three tiny CP/M commands are included in the repository:

```text
LOCAL.COM
USB.COM
BOTH.COM
```

From CP/M:

```text
A>USB
```

switches control to the USB console.

From the USB terminal:

```text
A>LOCAL
```

returns control to the Cardputer.

`BOTH` enables both consoles.

Regardless of the selected mode, pressing **Fn + =** on the Cardputer forces the console back to LOCAL mode.

## Terminal model

The intended terminal model for CP/M `CON:` is **VT100**.

USB terminals can already process the VT100/ANSI output stream directly.

The current local Cardputer implementation is an early 40-column x 16-row terminal implementation. The next terminal milestone is a logical **80x24 VT100 screen** with the Cardputer's physical **40x16 display acting as a viewport** that follows the logical cursor.

That work is not yet complete.

## Building

Open the project folder in Visual Studio Code with the PlatformIO extension installed.

Build from the PlatformIO controls, or from a VS Code terminal with:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e m5stack-cardputer
```

## Uploading

The Cardputer Adv can be placed into its upload mode with the following procedure:

1. Turn the Cardputer off.
2. Hold **G0**.
3. Connect USB while still holding **G0**.
4. Release **G0**.
5. Upload from PlatformIO.

From a VS Code terminal:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e m5stack-cardputer -t upload
```

## USB terminal testing

Close any other program that has the Cardputer COM port open before starting another terminal emulator.

For PuTTY, select **Serial** and use the Cardputer's current COM port with:

```text
Speed:       115200
Data bits:   8
Stop bits:   1
Parity:      None
Flow control: None
```

Boot normally in LOCAL mode, then enter:

```text
USB
```

on the Cardputer to hand the CP/M console to the USB terminal.

## RunCPM

This project incorporates source derived from **RunCPM** by Mockba the Borg:

https://github.com/MockbaTheBorg/RunCPM

RunCPM is distributed under the MIT License.

The original RunCPM copyright and licence text are preserved in:

```text
src/runcpm/LICENSE
```

Cardputer-specific integration and console work are maintained in this repository.

## License

Cardputer-CPM is distributed under the MIT License. See the root `LICENSE` file.

Portions derived from RunCPM remain subject to the RunCPM MIT copyright and licence notice in `src/runcpm/LICENSE`.
