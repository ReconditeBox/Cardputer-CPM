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

The Cardputer boots with **C: user 0** as its system/default drive. System utilities should therefore be placed in:

```text
C/0
```

For example:

```text
C/0/STAT.COM
```

appears to CP/M as `STAT.COM` on drive C:, user 0.

If an external command is not found on the current drive, the internal CCP also searches C: user 0. SUBMIT batch lookup and the PUN:/LST: host files likewise use C: as the system drive.

Additional user areas can be created as required:

```text
A/1
A/2
...
A/15
```

This makes it possible to manage CP/M files directly from another computer by inserting the SD card and copying normal files.

### Moving the system files to C:

Before flashing a firmware build that uses C: as the system drive, make sure the SD card contains a `C/0` directory and copy the CP/M system/utilities that were previously kept in `A/0` into `C/0`.

Keep the old `A/0` copy until the new firmware has booted successfully and the utilities have been tested from `C0>`. A: and B: remain ordinary directory-backed CP/M drives for now; removable-disk behaviour is a separate future change.


## Console modes

The machine currently supports four console-routing modes.

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
C0>USB
```

switches control to the USB console.

From the USB terminal:

```text
C0>LOCAL
```

returns control to the Cardputer.

`BOTH` enables both consoles.

Regardless of the selected mode, pressing **Fn + =** on the Cardputer forces the console back to LOCAL mode.

## Terminal model

The intended terminal model for CP/M `CON:` is **VT100**.

USB terminals can already process the VT100/ANSI output stream directly.

The local Cardputer terminal now maintains a logical **80x24 VT100 screen** with the physical **40x16 display acting as a viewport**.

The viewport normally follows the logical VT100 cursor automatically.

Cardputer cursor-key controls are:

```text
Fn + arrow       -> VT100 cursor key sent to CP/M
Aa + Fn + arrow  -> pan the local viewport
```

Manual panning moves 8 logical columns horizontally or 4 rows vertically per key press. The next CP/M output or VT100 cursor movement automatically brings the active cursor back into view.

VT100 normal cursor mode (`ESC [ A/B/C/D`) and application cursor mode (`ESC O A/B/C/D`) are both recognised for Cardputer keyboard input.

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

## WiFi configuration

WiFi is configured from a plain-text file named `WIFI.CFG` in the root of the SD card.

Example:

```text
SSID1=Home Network
PASS1=first-password

SSID2=Phone Hotspot
PASS2=second-password
```

Entries are tried in numeric order. The first network that connects is used. Up to 10 entries (`SSID1` through `SSID10`) are supported.

Blank lines and lines beginning with `#` or `;` are ignored. An empty `PASSn=` selects an open network.

Addressing defaults to DHCP. A network can instead have persistent static addressing by adding `IPn`, `MASKn` and `GWn` for the same entry. `DNSn` is optional; if it is omitted for a static entry, the gateway is used as DNS.

```text
SSID1=Home Network
PASS1=first-password
IP1=192.168.1.50
MASK1=255.255.255.0
GW1=192.168.1.1
DNS1=192.168.1.1
```

If `WIFI.CFG` is missing, contains no usable SSIDs, or none of the configured networks can be reached, the machine continues booting CP/M offline.

Passwords are stored in plain text on the SD card. The firmware never prints passwords to the console.

A template is included as `WIFI.CFG.EXAMPLE`. Copy it to the SD-card root, rename it to `WIFI.CFG`, and enter your own SSIDs and passwords.

## IFCONFIG command

`IFCONFIG.COM` displays the current WiFi configuration and can change the active `WIFI.CFG` entry between DHCP and a persistent static IPv4 address.

Display the current connection:

```text
C0>IFCONFIG

WiFi: connected
SSID:  Home Network
IP:    192.168.1.42
Mask:  255.255.255.0
GW:    192.168.1.1
DNS:   192.168.1.1
Mode:  DHCP
```

Switch the currently connected network back to DHCP:

```text
C0>IFCONFIG DHCP
```

Set a persistent static address:

```text
C0>IFCONFIG 192.168.1.50 255.255.255.0 192.168.1.1
```

Supply DNS explicitly as an optional fourth address:

```text
C0>IFCONFIG 192.168.1.50 255.255.255.0 192.168.1.1 8.8.8.8
```

Changes are written to the matching numbered entry in `/WIFI.CFG` and WiFi is reconnected immediately. If `DNSn` is omitted in static mode, the gateway is used as DNS.

`IFCONFIG DHCP` removes the active entry's `IPn`, `MASKn`, `GWn` and `DNSn` lines, restoring normal DHCP on this and subsequent boots.

Changing the IP address while Telnet owns `CON:` returns the console to LOCAL before WiFi is restarted. Reconnect the Telnet client to the new address and run `TELNETD` again if required.

If WiFi is offline, bare `IFCONFIG` reports that state. Address changes require a currently connected network so the firmware knows which numbered `WIFI.CFG` entry to update.

## Telnet console

When WiFi connects successfully, the Cardputer starts a Telnet server on TCP port 23.

A fourth CP/M console-routing command is provided:

```text
TELNETD.COM
```

This uses the same ESP32-specific BDOS hook as `LOCAL.COM`, `USB.COM`, and `BOTH.COM`.

Console modes are:

```text
LOCAL   Cardputer screen and keyboard
USB     USB CDC terminal
BOTH    Cardputer + USB CDC
TELNET  TCP/IP Telnet terminal on port 23
```

To use it:

1. Boot the Cardputer with a valid `WIFI.CFG`.
2. Note the IP address printed during startup.
3. Copy `TELNETD.COM` to a CP/M drive on the SD card, alongside the other console-switch commands.
4. From another machine on the same network, open a Telnet client to the Cardputer IP address on port 23.
5. At the Cardputer CP/M prompt, run `TELNETD`.
6. CP/M `CON:` is then routed to the Telnet client.

Only one Telnet client is accepted at a time.

If the Telnet client disconnects while it owns `CON:`, the firmware automatically returns to LOCAL mode.

The physical **Fn + =** key combination always forces LOCAL mode, including while TELNET owns the console.

## SETDEF command search path

The firmware includes a small `SETDEF.COM` utility that controls the drive search chain used by the internal CCP when loading external commands and SUB files.

The default search chain is:

```text
*,C:
```

where `*` means the current/default drive. Thus, from A: the CCP searches A: first and then the C: system drive.

Display the current search chain:

```text
C0>SETDEF
Drive Search Chain: *,C:
```

Change the order:

```text
C0>SETDEF C:,*
Drive Search Chain: C:,*
```

Use only the current drive:

```text
C0>SETDEF *
Drive Search Chain: *
```

Up to four entries are supported, using drives A: through P: and `*`. Explicitly drive-qualified commands bypass the SETDEF chain.

This first implementation covers the drive-search portion of CP/M Plus SETDEF. The CP/M Plus bracket options `TEMPORARY`, `ORDER`, `DISPLAY`, and `PAGE` are not implemented yet and are rejected rather than silently ignored.

Place `SETDEF.COM` in `C/0` with the other system utilities.

## Cold-boot PROFILE.SUB

The Cardputer uses `C/0/PROFILE.SUB` as its startup profile.

If that file exists, it is executed automatically once on a cold boot, exactly as though the user had typed:

```text
C0>C:PROFILE
```

The normal CCP `.SUB` handling then invokes `SUBMIT.COM`.

`PROFILE.SUB` is **not** run again on ordinary warm boots after programs exit. Rebooting or resetting the Cardputer starts a new cold boot and runs the profile again.

The previous `AUTOEXEC.TXT` startup mechanism has been removed.

A sample `PROFILE.SUB.EXAMPLE` is included in the repository. Copy it to:

```text
C/0/PROFILE.SUB
```

and edit it as required.

## Removable A: and B: media

CP/M drives **A:** and **B:** are removable-media slots backed by directories below `MEDIA/` on the SD card. Drives **C:** through **P:** keep their normal fixed RunCPM directory layout.

Both removable slots start empty after power-up. Example SD-card layout:

```text
MEDIA/
    WORDSTAR/
        0/
            WS.COM
            ...
    BASIC/
        0/
            ...
    GAMES/
        0/
            ...
```

Each directory directly below `MEDIA/` represents one removable disk. Inside it, RunCPM user areas use the usual hexadecimal subdirectories `0` through `F`. User area `0` is created automatically when a medium is first mounted if it does not already exist.

Media control is deliberately **physical-device-only**:

```text
Fn+A    manage drive A:
Fn+B    manage drive B:
```

If the selected drive is empty, the Cardputer LCD shows the directories available in `MEDIA/`. Use the Cardputer cursor keys to choose a directory and **Enter** to mount it; Escape or Backspace cancels.

If media is already mounted, **Fn+A** or **Fn+B** shows an eject confirmation before removing it.

USB and Telnet users can read and write whichever A:/B: media is already mounted, but there is no USB, Telnet, CP/M command, or BDOS interface for inserting or ejecting media. Only the physical Cardputer keyboard can change the media.

Mounting or ejecting invalidates RunCPM's cached login/read-only state for that drive so the next access sees the new media.

This implementation supports **directory-backed media only**. Files with a `.DSK` extension are ignored by the chooser for now.

Stock CP/M 2.2 `SUBMIT.COM` normally creates `A:$$$.SUB`. The firmware redirects only that legacy temporary file to the fixed C: system drive internally, so SUBMIT and `PROFILE.SUB` continue to work even when A: is empty or contains removable media.

## Boot splash and WiFi skip

If a file named `SPLASH.PNG` is present in the root of the SD card, the Cardputer displays it at the start of a successful boot and waits for a **physical Cardputer keypress** before continuing.

For the best result, use a PNG sized for the Cardputer display:

```text
240 x 135 pixels
```

The splash is read from:

```text
/SPLASH.PNG
```

If the file is missing, unreadable, cannot fit in available memory, or cannot be decoded as PNG, boot continues normally without stopping.

After the splash is dismissed, normal boot text is displayed and WiFi connection attempts begin. While the firmware is trying the networks listed in `WIFI.CFG`, pressing **any physical Cardputer key** immediately skips the remaining WiFi attempts and continues booting offline.

The splash-dismiss key is fully consumed before WiFi begins, so dismissing the splash does not also skip WiFi. Likewise, a WiFi-skip keypress is consumed and is not passed on to CP/M.

USB and Telnet input cannot dismiss the splash or skip WiFi; these are physical-device boot controls.

