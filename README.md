# Cardputer-CPM

CP/M on the **M5Stack Cardputer Adv**, using **RunCPM** as the emulation core and the Cardputer's built-in display, keyboard, microSD slot, USB CDC and WiFi as the machine interfaces.

The aim is a practical standalone CP/M computer rather than a serial-console demonstration: boot from microSD, run real CP/M software locally, use removable logical media, and move files over the network without removing the SD card.

## Features

Current functionality includes:

- Z80-compatible CP/M environment
- CP/M boots directly on the Cardputer Adv
- built-in keyboard and 240 x 135 LCD as the default console
- logical 80 x 24 VT100 terminal with a 40 x 16 local viewport
- microSD-backed CP/M drives
- fixed **C:** system/default drive
- removable directory-backed **A:** and **B:** media slots
- fixed **C:** through **P:** drives
- CP/M user areas 0 through 15
- real CP/M `.COM` programs
- `PROFILE.SUB` cold-boot startup
- configurable CCP command search path with `SETDEF`
- USB CDC console
- WiFi configuration from `WIFI.CFG`
- persistent DHCP/static IPv4 configuration with `IFCONFIG`
- foreground Telnet server with `TELNETD`
- foreground anonymous FTP server with `FTPD`
- outbound DNS lookup with `DNS`
- ICMP echo testing with `PING`
- outbound Telnet client with `TELNET`
- interactive outbound passive-mode FTP client with `FTP`
- Lynx-style text web browser with `BROWSE`
- HTTP/HTTPS file download with `WGET`
- NTP UTC clock synchronisation with `NTP`
- current UTC date/time display with `TIME`
- live ESP32 memory reporting with `MEM`
- CP/M character devices `LST:`, `RDR:` and `PUN:`
- EXT-header auxiliary UART at 115200 8N1 *(implemented; physical hardware validation pending)*
- built-in `AUX` routing command
- built-in `BATTERY` status command
- persistent CP/M R/O, SYS and archive file attributes
- FTP enforcement of CP/M read-only files
- optional PNG boot splash
- physical key to skip WiFi connection attempts
- physical **G0** mode cycle: LOCAL -> TELNETD -> FTPD -> LOCAL
- physical **Fn + =** emergency return to LOCAL mode

Standard CP/M utilities such as `DIR`, `STAT` and `PIP` have been tested successfully.

## Tested software

The following software and toolchains have been exercised successfully on Cardputer-CPM:

- **WordStar**
- **Microsoft MBASIC / BASIC-80**
- **Hi-Tech C**
- **Microsoft M80** macro assembler
- **Microsoft L80 / LINK-80** linker
- **Microsoft CREF80** cross-reference generator
- **Microsoft LIB80** library manager
- standard CP/M utilities including **DIR**, **STAT** and **PIP**
- CP/M character devices **LST:**, **RDR:** and **PUN:**

The Microsoft development-tool test included assembling relocatable modules with M80, linking multiple modules with L80, generating a CREF80 `.PRN` cross-reference listing, building a searchable library with LIB80, and having L80 successfully extract a required module from that library with `/S`.

The auxiliary UART path is implemented in the firmware and exposed through the `AUX` command, but **physical AUX/RS232 hardware validation is still pending**.

This list records software that has actually been tested; it is not intended to imply that unlisted CP/M software is incompatible.

---

## Hardware

- **M5Stack Cardputer Adv**
- microSD card
- USB connection for flashing and optional USB CDC console

The SD interface uses:

| Signal | GPIO |
| --- | ---: |
| SCK | 40 |
| MISO | 39 |
| MOSI | 14 |
| CS | 12 |

The Cardputer Adv EXT header provides the auxiliary serial interface used by Cardputer-CPM. Firmware support is implemented; physical RS232/AUX hardware validation is pending:

| Signal | GPIO |
| --- | ---: |
| AUX TX | 13 |
| AUX RX | 15 |

The AUX UART runs at **115200 baud, 8 data bits, no parity, 1 stop bit**.

---



## Quick start

### 1. Prepare the SD card

At minimum, create the system drive and user 0:

```text
C/
    0/
```

Place the CP/M utilities you want available globally in:

```text
C/0/
```

For example:

```text
C/0/STAT.COM
C/0/PIP.COM
C/0/SUBMIT.COM
```

The Cardputer boots as:

```text
C0>
```

### 2. Add the Cardputer utilities

The repository includes these small CP/M commands:

```text
LOCAL.COM
USB.COM
BOTH.COM
TELNETD.COM
TELNET.COM
FTP.COM
BROWSE.COM
DNS.COM
PING.COM
WGET.COM
NTP.COM
TIME.COM
IFCONFIG.COM
FTPD.COM
MEM.COM
SETDEF.COM
```

For normal use, copy them into:

```text
C/0/
```

### 3. Build and upload

Build:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e m5stack-cardputer
```

Upload:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e m5stack-cardputer -t upload
```

The upload procedure is described in [Building and flashing](#building-and-flashing).

---

## CP/M storage layout

Cardputer-CPM uses ordinary directories on the microSD card rather than CP/M disk-image files for its normal fixed drives.

### System drive

**C:** is the fixed system/default drive.

User 0 is stored as:

```text
C/0/
```

For example:

```text
C/0/STAT.COM
```

appears in CP/M as `STAT.COM` on C: user 0.

The internal CCP's default command search chain is:

```text
*,C:
```

so an unqualified external command is searched for on the current drive first and then on C:.

`PROFILE.SUB`, SUBMIT support, and the RDR:/PUN:/LST: host files also use C: as the system drive.

### Fixed drives

Drives **C:** through **P:** use the ordinary RunCPM directory layout:

```text
C/0
C/1
...
D/0
...
P/0
```

User areas 10 through 15 are stored as hexadecimal directories:

```text
A
B
C
D
E
F
```

so, for example, CP/M user 15 on drive F: is backed by:

```text
F/F/
```

### Removable A: and B:

Drives **A:** and **B:** are removable logical media slots. Their media live below:

```text
MEDIA/
```

See [Removable A: and B: media](#removable-a-and-b-media).

### F: as the network transfer drive

The FTP server is intentionally restricted to **F:**.

That makes F: a convenient transfer area for adding or retrieving CP/M programs over WiFi without removing the SD card. FTP user selection maps directly to F: user areas 0 through 15.

### CP/M file attributes

Cardputer-CPM persists the standard CP/M file attributes:

- **R/O** — read-only
- **SYS** — system
- **Archive** — unchanged since backup

The attributes are stored in the underlying FAT directory metadata, so they survive Cardputer restarts and removing/reinserting the SD card.

Standard CP/M 2.2 `STAT.COM` can set the user-visible attributes in the normal way:

```text
STAT FILENAME.TYP $R/O
STAT FILENAME.TYP $R/W
STAT FILENAME.TYP $SYS
STAT FILENAME.TYP $DIR
```

Ambiguous filenames such as `*.COM` are also supported.

R/O files cannot be written, deleted or renamed until changed back to R/W.

SYS files are omitted by the built-in `DIR` command while remaining visible to attribute-aware software such as `STAT`.

The CP/M archive flag is mapped to the FAT archive flag with the required inverse meaning: a successful write marks the FAT file as changed and therefore clears CP/M's archived state.

---

## Boot sequence

### Optional splash screen

If the SD-card root contains:

```text
/SPLASH.PNG
```

it is displayed during boot.

For the best result use:

```text
240 x 135 pixels
```

The splash waits for a **physical Cardputer keypress** before boot continues.

If the file is missing, unreadable, too large for available memory, or cannot be decoded, boot continues normally.

### Skipping WiFi

After the splash, the firmware tries the networks in `WIFI.CFG`.

During these connection attempts, pressing **any physical Cardputer key** skips the remaining WiFi attempts and continues booting offline.

The splash-dismiss key and WiFi-skip key are both consumed and are not passed through to CP/M.

USB or network input cannot dismiss the splash or skip WiFi.

### Cold-boot PROFILE.SUB

If this file exists:

```text
C/0/PROFILE.SUB
```

it is executed automatically once during a cold boot, as though the user had entered:

```text
C0>C:PROFILE
```

The normal CCP `.SUB` handling then invokes `SUBMIT.COM`.

`PROFILE.SUB` is not run again on ordinary warm boots after programs exit.

A sample `PROFILE.SUB.EXAMPLE` is included in the repository.

---

## Console and terminal

Cardputer-CPM supports four console-routing modes:

| Mode | Console |
| --- | --- |
| LOCAL | Cardputer LCD + keyboard |
| USB | USB CDC terminal |
| BOTH | Cardputer + USB |
| TELNET | Telnet client |

LOCAL is the default at boot.

### Console commands

```text
LOCAL.COM
USB.COM
BOTH.COM
```

Examples:

```text
C0>USB
```

routes CP/M console I/O to USB.

From the USB terminal:

```text
C0>LOCAL
```

returns the console to the Cardputer.

`BOTH` enables Cardputer and USB output together for diagnostics.

Physical **Fn + =** always forces a return to LOCAL where applicable.

### G0 system-mode button

While Cardputer-CPM is running, each press of the physical **G0** button advances the machine through:

```text
LOCAL -> TELNETD -> FTPD -> LOCAL
```

The transitions are machine-level rather than simulated CP/M keystrokes:

- **LOCAL -> TELNETD** starts TELNETD on the default TCP port 23 and routes `CON:` to the Telnet console.
- **TELNETD -> FTPD** cleanly closes any Telnet client/listener, returns the console locally, then starts FTPD.
- **FTPD -> LOCAL** closes FTP control/data connections and the listener, then restores the local CP/M console.

G0 also works while an FTP transfer is active.

Holding **G0 during power-on/USB connection** still has its normal ESP32-S3 download-mode purpose; the mode cycle applies only after Cardputer-CPM is running.

### VT100 model

CP/M `CON:` uses a VT100-style terminal model.

The local terminal maintains a logical:

```text
80 x 24
```

screen, with the Cardputer LCD displaying a:

```text
40 x 16
```

viewport.

The viewport normally follows the logical cursor automatically.

Keyboard controls:

```text
Fn + arrow       VT100 cursor key sent to CP/M
Aa + Fn + arrow  pan the local viewport
```

Manual panning moves 8 logical columns horizontally or 4 rows vertically.

Both normal VT100 cursor mode:

```text
ESC [ A/B/C/D
```

and application cursor mode:

```text
ESC O A/B/C/D
```

are supported for Cardputer cursor-key input.

### USB terminal settings

The USB CDC console has been tested with ordinary serial terminal programs such as PuTTY using:

```text
115200 baud
8 data bits
no parity
1 stop bit
no flow control
```

Close any other application that already has the Cardputer COM port open before starting a terminal emulator.

---

## CP/M character devices

Cardputer-CPM provides the classic CP/M character devices:

| Device | Cardputer-CPM backing |
| --- | --- |
| `CON:` | Cardputer/USB/Telnet console router |
| `LST:` | `C/0/LST.TXT` |
| `RDR:` | `C/0/RDR.TXT` by default |
| `PUN:` | `C/0/PUN.TXT` by default |

The file-backed `RDR:` and `PUN:` devices remain the default after boot.

### AUX UART

The Cardputer Adv EXT header exposes a bidirectional UART. The firmware path and CP/M routing are implemented, but physical operation with the external RS232/AUX hardware has not yet been validated:

```text
TX  GPIO13
RX  GPIO15
115200 8N1
```

Cardputer-CPM maps the CP/M `TTY:` reader/punch selection to this UART.

The built-in CCP command:

```text
C0>AUX
```

shows the current routing.

Route both CP/M reader and punch traffic to the UART:

```text
C0>AUX ON
```

Return them to the file-backed devices:

```text
C0>AUX OFF
```

`AUX ON` changes only the RDR/PUN fields of the CP/M IOBYTE; the `CON:` and `LST:` assignments are preserved.

Software that changes the CP/M IOBYTE directly can also select `TTY:` for RDR/PUN to reach the UART.

### BATTERY

`BATTERY` is a built-in CCP command:

```text
C0>BATTERY
```

It reports the Cardputer battery level and measured battery voltage, for example:

```text
Battery
-------
Level:   83%
Voltage: 4012 mV (4.012 V)
```

Charging state/current are not reported because the Cardputer hardware does not provide reliable charging-status/current readings through the M5Stack power API.

---

## Networking

WiFi is optional. If no configured network connects, CP/M continues booting offline.

### WIFI.CFG

WiFi configuration is read from:

```text
/WIFI.CFG
```

Example:

```text
SSID1=Home Network
PASS1=first-password

SSID2=Phone Hotspot
PASS2=second-password
```

Up to 10 entries are supported:

```text
SSID1 ... SSID10
```

Entries are tried in numeric order and the first successful connection is used.

Blank lines and lines beginning with `#` or `;` are ignored.

An empty password selects an open network:

```text
PASS1=
```

Passwords are stored in plain text on the SD card. The firmware does not print them to the console.

A template is provided as:

```text
WIFI.CFG.EXAMPLE
```

Copy it to the SD-card root as `WIFI.CFG` and edit it for your networks.

### DHCP and static IPv4

DHCP is the default.

A network may instead contain persistent static addressing:

```text
SSID1=Home Network
PASS1=first-password
IP1=192.168.1.50
MASK1=255.255.255.0
GW1=192.168.1.1
DNS1=192.168.1.1
```

`IPn`, `MASKn` and `GWn` are required together for static mode.

`DNSn` is optional. If omitted, the gateway is used as DNS.

---

## IFCONFIG

`IFCONFIG.COM` displays the current WiFi configuration and can update the active `WIFI.CFG` entry.

Display status:

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

Return the current network to DHCP:

```text
C0>IFCONFIG DHCP
```

Set a persistent static address:

```text
C0>IFCONFIG 192.168.1.50 255.255.255.0 192.168.1.1
```

Optionally specify DNS:

```text
C0>IFCONFIG 192.168.1.50 255.255.255.0 192.168.1.1 8.8.8.8
```

Changes are written back to the matching numbered entry in `/WIFI.CFG` and WiFi reconnects immediately.

`IFCONFIG DHCP` removes that entry's `IPn`, `MASKn`, `GWn` and `DNSn` lines.

If WiFi is offline, bare `IFCONFIG` reports the offline state. Address changes require a currently connected network so the firmware knows which numbered `WIFI.CFG` entry to modify.

Network configuration changes are blocked while a network service is using the interface.

---

## Outbound networking

Cardputer-CPM also provides CP/M commands that initiate network connections from the Cardputer itself.

### DNS

Show the currently active DNS server:

```text
C0>DNS
DNS server: 192.168.1.1
```

Resolve a host name to IPv4:

```text
C0>DNS example.com
example.com = 93.184.216.34
```

Usage:

```text
DNS
DNS host
```

### PING

Send ICMP echo requests to a host name or IPv4 address:

```text
C0>PING example.com
```

Four requests are sent by default. An optional count from 1 through 20 may be supplied:

```text
C0>PING 192.168.1.1 10
```

Press **Ctrl-C** to cancel a running ping.

### TELNET

Connect from CP/M to a remote Telnet service:

```text
C0>TELNET host
```

Port 23 is the default. A different TCP port may be specified:

```text
C0>TELNET host 2323
```

The command is a foreground Telnet client. Remote terminal output passes through the Cardputer's VT100 console path, and Cardputer/USB console input is sent to the remote host.

Disconnect with either:

```text
Ctrl-]
```

or physical **Fn + =**.

Outbound `TELNET` may run while `TELNETD` is active, including from an inbound Telnet console session. It remains unavailable while `FTPD` is active.

### FTP

Connect to a remote FTP server:

```text
C0>FTP ftp.example.com
```

Port 21 is the default. A different control port may be supplied:

```text
C0>FTP ftp.example.com 2121
```

The client attempts anonymous login first and then enters an interactive prompt. Transfers default to binary mode.

Common commands:

```text
ftp> ls
ftp> pwd
ftp> cd pub
ftp> get PROGRAM.COM
ftp> get archive.bin F:ARCHIVE.BIN
ftp> put README.TXT
ftp> put F:PROGRAM.COM PROGRAM.COM
ftp> user accountname
ftp> pass
ftp> binary
ftp> ascii
ftp> quote SYST
ftp> quit
```

`PASS` prompts for the password without echoing it.

The outbound client uses **passive FTP only**. `GET` downloads through a temporary file and installs it only after the transfer completes, and existing CP/M read-only destinations are protected. Local filenames remain CP/M 8.3 names in the current user area, with an optional drive prefix.

Outbound `FTP` may run while TELNETD is active. It is unavailable while the local FTPD server is active.

### BROWSE

`BROWSE.COM` is a small interactive text-mode web browser with a Lynx-style terminal interface:

```text
C0>BROWSE https://example.com/
```

With no URL, it prompts for one:

```text
C0>BROWSE
URL:
```

Pages are fetched completely to a temporary SD file, parsed into a local text-page model, then displayed through the existing VT100 console. The Cardputer view uses a **38-column by 14-line content viewport**, leaving the bottom two rows for browser status and controls.

Links are displayed as ordinary text. The currently selected link is shown in **reverse video** rather than with link numbers.

Navigation follows the familiar Lynx model:

```text
Up / Down       previous / next link
Right / Enter   follow selected link
Left / Backspace
                go back
Space / PgDn    page down
- / PgUp        page up
Home / End      top / bottom

G               enter a URL
R               reload
D               download selected link
L               show selected link URL
H or ?          help
Q               quit
```

On the Cardputer itself, cursor navigation uses the normal **Fn + arrow** keys. **Space** and **-** provide page down/up locally. Home/End and Page Up/Page Down are additionally recognised when an external USB or Telnet terminal sends their standard VT100-style escape sequences.

`D` passes the selected HTTP/HTTPS link to the existing WGET download path and prompts for an optional CP/M 8.3 destination filename.

Relative HTTP/HTTPS links are resolved against the current page. Up to 64 selectable links are retained per page, up to 384 rendered text lines are kept, and eight pages of back history are available.

The browser handles ordinary server-rendered HTML and plain text, including headings, paragraphs, lists, links, basic preformatted text, image ALT text and common HTML entities. It ignores CSS and JavaScript and does **not** execute client-side applications. Modern sites that require JavaScript will therefore be incomplete or unusable, as expected for a CP/M-style text browser.

HTTP and HTTPS are supported. As with WGET, HTTPS traffic is encrypted but certificate identity is not yet validated because the firmware does not currently carry a CA bundle.

### WGET

Download a file directly into CP/M storage:

```text
C0>WGET http://example.com/README.TXT
```

If the final URL component is already a valid CP/M 8.3 filename, that name is used on the current drive and current user area.

An explicit destination may be supplied:

```text
C0>WGET http://example.com/file.bin PROGRAM.COM
C0>WGET https://example.com/file.bin F:PROGRAM.COM
```

The destination is always restricted to a CP/M drive, the current user area, and a valid 8.3 filename. Existing CP/M read-only files are protected.

WGET downloads through a temporary file and replaces the destination only after the transfer completes successfully, so a failed transfer does not leave a truncated target.

Both `http://` and `https://` are supported. HTTPS traffic is encrypted, but this first implementation does **not** validate the remote certificate because the firmware does not yet carry a CA bundle.

`WGET` is unavailable while FTPD owns the transfer service.

### NTP and TIME

After WiFi connects, Cardputer-CPM starts UTC time synchronisation with:

```text
pool.ntp.org
```

Boot is not delayed waiting for NTP.

Force a synchronisation and wait for a result:

```text
C0>NTP
```

Use another server for the current session:

```text
C0>NTP time.cloudflare.com
```

A successful sync reports the server and UTC date/time.

Display the current clock without contacting an NTP server:

```text
C0>TIME
Time: 2026-10-03 20:15:42 UTC
```

The host clock is deliberately kept in UTC. Local timezone handling can be added separately without changing the underlying absolute time.

---

## TELNETD

`TELNETD.COM` starts a foreground Telnet console service.

WiFi connection by itself does **not** open a Telnet listener.

Default port:

```text
C0>TELNETD
```

Custom port:

```text
C0>TELNETD 2323
```

Valid ports are 1 through 65535.

Running `TELNETD`:

1. creates the listener;
2. routes CP/M `CON:` to TELNET;
3. displays the listening address and port on the Cardputer;
4. accepts one client.

Connection events include the remote IPv4 address:

```text
[TELNET client connected from 192.168.1.23]
[TELNET client disconnected from 192.168.1.23]
```

If the Telnet client disconnects, TELNETD ends completely, the listener closes, and `CON:` returns to LOCAL.

A new Telnet client cannot connect until `TELNETD` is run again.

Physical **Fn + =**, `LOCAL.COM`, or switching to USB/BOTH also ends TELNETD and closes the client/listener.

---

## FTPD

`FTPD.COM` starts a foreground anonymous FTP server on TCP port 21.

Run:

```text
C0>FTPD
```

While active, the normal CP/M prompt is replaced by a dedicated FTP status screen showing the listening address, current user area and client state.

Physical **Fn + =**:

- closes the FTP client if connected;
- closes any data connection;
- shuts down the port-21 listener;
- exits FTPD;
- returns to the LOCAL CP/M console.

Physical **G0** performs the FTPD -> LOCAL step of the normal G0 mode cycle and also works during an active transfer.

### Authentication

FTPD is anonymous.

`USER` is accepted and the supplied `PASS` value is ignored.

Only one FTP control client is accepted at a time.

Connection and disconnection events show the remote IPv4 address:

```text
[FTP client connected from 192.168.1.23]
[FTP client disconnected from 192.168.1.23]
```

Unlike TELNETD, an FTP client disconnect does **not** terminate FTPD. The server returns to waiting for another client until **Fn + =** is pressed or **G0** advances the machine from FTPD back to LOCAL.

### FTP filesystem view

FTPD exposes **only F:**.

No other CP/M drive and no arbitrary SD-card path is accessible.

There are no FTP directories. `CWD` is repurposed to select the CP/M user area:

```text
CWD 0
CWD 1
...
CWD 15
```

`PWD` reports:

```text
/0
...
/15
```

Every new FTP session starts in F: user 0.

Internally these map to:

```text
F/0
...
F/9
F/A
...
F/F
```

but those host directories are not exposed as an FTP directory tree.

The following are rejected:

- `MKD`
- `RMD`
- `CDUP`
- path traversal
- embedded drive names
- access outside the selected F: user area

FTP filenames are restricted to CP/M-compatible 8.3 names and uploads are canonicalised to uppercase.

Supported operations include:

```text
LIST
NLST
MLSD
RETR
STOR
APPE
DELE
RNFR / RNTO
SIZE
PASV
EPSV
```

FTPD honours CP/M read-only protection. A file marked R/O cannot be overwritten with `STOR`, appended with `APPE`, deleted with `DELE`, or renamed with `RNFR/RNTO`. `LIST` and `MLSD` also report the file as read-only.

Active-mode FTP (`PORT`/`EPRT`) is not supported.

FTPD and TELNETD are mutually exclusive.

A bare `IFCONFIG` may display network status while FTPD is active, but address changes are refused until FTPD is stopped.

---

## Removable A: and B: media

A: and B: are physical-style logical media slots backed by directories below:

```text
MEDIA/
```

Both slots start empty after power-up.

Example:

```text
MEDIA/
    WORDSTAR/
        0/
            WS.COM
    BASIC/
        0/
    GAMES/
        0/
```

Each directory immediately below `MEDIA/` represents one removable medium.

Its CP/M user areas use:

```text
0 ... 9, A ... F
```

User 0 is created automatically when a medium is first mounted if necessary.

### Physical controls

```text
Fn+A    manage A:
Fn+B    manage B:
```

If the slot is empty, a local chooser displays the available directories below `MEDIA/`.

Use the Cardputer cursor keys to choose a medium and **Enter** to mount it.

Escape or Backspace cancels.

If media is already mounted, **Fn+A** or **Fn+B** displays an eject confirmation.

Mount/eject is deliberately **physical-device-only**. USB, Telnet and CP/M programs may access currently mounted media, but they cannot insert or eject it.

Changing media invalidates RunCPM's cached login/read-only state so the next access sees the new medium.

Only directory-backed removable media are supported at present. `.DSK` files are ignored by the chooser.

Stock CP/M 2.2 `SUBMIT.COM` normally creates `A:$$$.SUB`. Cardputer-CPM redirects only that legacy temporary file to fixed C: internally so SUBMIT and `PROFILE.SUB` continue to work even when A: is empty or has removable media inserted.

---

## SETDEF

`SETDEF.COM` controls the internal CCP drive search chain for external commands and SUB files.

Default:

```text
*,C:
```

Display the current chain:

```text
C0>SETDEF
Drive Search Chain: *,C:
```

Change it:

```text
C0>SETDEF C:,*
Drive Search Chain: C:,*
```

Use only the current drive:

```text
C0>SETDEF *
Drive Search Chain: *
```

Up to four entries are supported using drives A: through P: and `*`.

Explicitly drive-qualified commands bypass the SETDEF chain.

The CP/M Plus bracket options `TEMPORARY`, `ORDER`, `DISPLAY` and `PAGE` are not implemented and are rejected rather than silently ignored.

Place `SETDEF.COM` in `C/0` with the other system utilities.

---

## MEM

`MEM.COM` reports live ESP32 runtime memory information from CP/M.

Run:

```text
C0>MEM
```

The report includes:

```text
Heap total
Heap free
Heap minimum
Largest block
Firmware size
Firmware free
PSRAM total/free, or none
```

`Heap free` is the currently available heap.

`Heap minimum` is the lowest free-heap value observed since boot and is useful for identifying peak memory pressure.

`Largest block` is the largest single allocation that can currently be satisfied.

The command is useful for comparing memory usage with WiFi and network services active.

---

## Building and flashing

### Development environment

The project uses:

- Visual Studio Code
- PlatformIO
- Arduino framework for ESP32-S3

The PlatformIO environment is:

```text
m5stack-cardputer
```

Dependencies are defined in `platformio.ini`.

### Build

From the VS Code terminal:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e m5stack-cardputer
```

### Put the Cardputer Adv into upload mode

1. Turn the Cardputer off.
2. Hold **G0**.
3. Connect USB while still holding **G0**.
4. Release **G0**.
5. Upload from PlatformIO.

### Upload

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e m5stack-cardputer -t upload
```

---

## RunCPM

This project incorporates source derived from **RunCPM** by Mockba the Borg:

https://github.com/MockbaTheBorg/RunCPM

The original RunCPM copyright and MIT licence text are preserved in:

```text
src/runcpm/LICENSE
```

Cardputer-specific integration, console, storage and networking work are maintained in this repository.

---

## License

Cardputer-CPM is distributed under the MIT License.

See:

```text
LICENSE
```

Portions derived from RunCPM remain subject to the RunCPM MIT copyright and licence notice in:

```text
src/runcpm/LICENSE
```
