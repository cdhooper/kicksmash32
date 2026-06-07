# Getting Started on Windows

This is the start-here guide for using **KickSmash** from a **Windows** host. It
takes you end-to-end, from a USB cable in your hand to a board that is
**flashed, configured, and ready to install** in your Amiga, with the exact
command lines to type at every step.

## What this guide is (and isn't)

**This guide covers:** a Windows 10/11 host, a KickSmash board that already has
its STM32 firmware loaded, flashing a Kickstart ROM image over USB, and
configuring the board's banks. It stops at "board configured and ready to
install" and hands off to the hardware-install documents for the physical
installation in your specific Amiga.

**This guide does not cover:** loading the STM32 firmware (see
[`hw_programming.txt`](hw_programming.txt)), building the software from source
(see [`DEVENV.md`](DEVENV.md) / [`sw_build.txt`](sw_build.txt)), the AmigaOS-side
`smash` workflow (see [`AMIGACLI.md`](AMIGACLI.md)), or Linux/macOS hosts. Those
paths are pointed to from the [Extended Reading](#extended-reading) index at the
end, not walked here.

**The host-side flow below is the same for every supported Amiga model.** Only
two things vary by machine: the byte-swap mode (the A3000/A4000 examples here
need no swap flag; some models do) and the physical install. Where a model
difference matters, this guide points you to [`WHICH.md`](WHICH.md). All worked
examples target the **Amiga 3000**.

## The spine: what you'll do

You'll do **eight things** on the host, then hand off to the hardware-install
docs. Everything below is one of these steps.

1. Confirm KickSmash enumerates over USB.
2. Fix the Windows COM-port driver (the Code 10 / Code 28 stumbling block).
3. Download the `hostsmash` release binary and open a terminal in its folder.
4. Make first contact and run a health check.
5. See what's already on the board's eight banks.
6. Prepare (and sanity-check) your Kickstart ROM image.
7. Write and verify the ROM into a bank.
8. Name the bank and set the power-on default.

Then, with the host-side work done, you hand off to the hardware-install docs
for your specific Amiga (the boundary of this guide, covered in step 9 below).

A [Troubleshooting](#troubleshooting) section and the
[Extended Reading](#extended-reading) index follow the steps.

## Prerequisites

Before step 1, gather all of the following so you don't discover a gap
mid-flow.

| Need | Detail |
| --- | --- |
| KickSmash board | Firmware already loaded. It must enumerate as VID `0x1209` / PID `0x1610`, product **"KickSmash Prg"** (see step 1). |
| An Amiga | A3000 or A4000 for the examples here. Other models work; see [`WHICH.md`](WHICH.md). |
| USB-C cable | Clearance near the ROM sockets is tight; see [`usb_cables.txt`](usb_cables.txt) for fit notes. |
| Windows 10 or 11 | Newer Windows 11 builds (24H2/25H2) have driver nuances; see [`windows_notes.txt`](windows_notes.txt). |
| `hostsmash` release binary | **The git checkout has no prebuilt binary.** You download it from a GitHub release (step 3). |
| A plaintext Kickstart ROM | Not Cloanto-encrypted. You must be licensed to use it (e.g. an Amiga Forever `A3000.47.115.rom`). Step 6 shows how to check. |
| `wdi-simple.exe` | The Windows driver installer. Obtained during step 2 (download link there) -- nothing to do up front. |

### Substitution conventions (read once)

Two things in every command below are placeholders you must replace with your
own values:

- **`com5`** -- your actual COM port number. Find it in step 2. It is bound to
  the `-d` (device) option, so it always appears as `-d com5`.
- **`A3000.47.115.rom`** -- your own ROM filename. Unlike the COM port, the ROM
  filename is a **positional argument**: it is *not* bound to `-w`. The order of
  `-b`, `-w`, `-v`, `-y`, and the filename is flexible, which is why the other
  docs show forms like `-wv A3000.rom -b 0`. Wherever you see this filename,
  substitute yours.

### Single-owner port

Only **one** `hostsmash` session can hold the KickSmash serial port at a time.
If you have a terminal session open (a `-t` interactive session), **close it**
(`Ctrl-X`) before running the next command, or the next command will fail to
open the port.

### Not loaded yet?

If your device shows up as a DFU device `0483:df11` instead of `0x1209` /
`0x1610` "KickSmash Prg", the firmware is **not** loaded. That is out of scope
here; see [`hw_programming.txt`](hw_programming.txt) and the DFU section of
[`windows_notes.txt`](windows_notes.txt).

## Step 1: Connect over USB and confirm it

Plug the KickSmash into your PC with the USB-C cable. Open **Device Manager**.

You are looking for a **"KickSmash Prg"** device (VID `0x1209` / PID `0x1610`).
Before the driver is fixed it usually appears under **Other devices**, and a
related serial interface under **Ports (COM & LPT)** shows an error status. That
error is exactly what step 2 fixes.

## Step 2: Fix the Windows driver (Code 10 / Code 28)

This is the single biggest Windows-only stumbling block. Windows ships a
generic serial driver, but it often does not bind to KickSmash automatically.

**Symptom.** In Device Manager, the COM interface shows:

```text
This device cannot start. (Code 10)
A device which does not exist was specified.
```

and a second interface may show **Code 28** ("The drivers for this device are
not installed.").

**Fix.** Download `wdi-simple.exe`:

```text
https://github.com/rogerclarkmelbourne/Arduino_STM32/blob/master/drivers/win/wdi-simple.exe?raw=true
```

Open an **elevated** (Run as administrator) command prompt and run:

```text
wdi-simple.exe --vid 0x1209 --pid 0x1610 --type 3 --name "KickSmash Prg" --dest "maple-serial"
```

Then **unplug and replug** the board.

**Result.** KickSmash now appears under **Ports (COM & LPT)** as
**"KickSmash Prg (COM5)"**. Note the `COMx` number shown -- that is your port. It
replaces `com5` in every command below. Windows displays the port uppercase
(`COM5`); the commands type it lowercase (`com5`) -- they are the same port. If
Windows shows COM7, type `-d com7` everywhere this guide shows `-d com5`.

> If you have `wget`, the `wdikicksmash.bat` script in the release `sw\` folder
> runs the download and install in one shot. See
> [`windows_notes.txt`](windows_notes.txt), which also documents the
> STSW-STM32102 fallback for older Windows and a COM-port-arbiter trick to keep
> the same COM number across multiple boards.

## Step 3: Get hostsmash and open a terminal in its folder

The host utility is **not** in the git checkout. Download the official GitHub
release:

```text
https://github.com/cdhooper/kicksmash32/releases
```

Download the release zip (we used `release_2.0`, whose asset is
`kicksmash_2.0.zip`) and extract it. Inside, the Windows binary is at:

```text
sw\hostsmash_win64.exe
```

The zip also carries `linux`, `mac`, `pi`, and `win32` builds -- ignore those on
Windows 64-bit.

**Open your terminal in the extracted `sw\` folder** so the commands below work
exactly as typed. In File Explorer, navigate into the extracted `sw\` folder,
then **Shift + right-click** an empty area and choose **"Open PowerShell window
here"** (or "Open command window here"). Alternatively, add the `sw\` folder to
your `PATH`.

**Sanity check** (works with no device attached -- help prints before the serial
port is opened):

```text
hostsmash_win64.exe --help
```

You should see the usage text listing options like `-d`, `-w`, `-v`, `-b`,
`-t`, and `-y`.

## Step 4: First contact and health check

Confirm the host and board can talk, and that both flash chips are healthy,
*before* writing anything.

Ask for the firmware version:

```text
hostsmash_win64.exe -d com5 -t ver
```

Expected output:

```text
Kicksmash 32 Version 2.0+ built 2026-05-30 00:15:38
    CPUID=411fc231 Dev=0418 Rev=1001 Flash=256 KB
    STM32F1    Connectivity revision Z
    HCLK=72 MHz  APB1=36 MHz  APB2=72 MHz
    Power-on reset
    Valid config at 3e300
Connected: !A17 !A18 !A19 !D31 !KBRST Flash0 Flash1
Prom 000122d2 000122d2 M29F160FT M29F160FT
Standalone
CMD> ver
Version 2.0+ built 2026-05-30 00:15:38
```

**Bench note on the `Connected:` line.** The `!` prefix on each signal
(`!A17 !A18 !A19 !D31 !KBRST`) means those Amiga bus signals are **not
asserted**. That is normal and expected when the board is on the bench (not
installed in an Amiga). All-`!` here is healthy.

Check the flash chips directly:

```text
hostsmash_win64.exe -d com5 -t "prom id"
```

Expected output:

```text
000122d2 000122d2 M29F160FT M29F160FT
```

Two **matching** IDs means both flash halves are present and healthy. (`-t`
takes a quoted firmware command; multi-word commands like `"prom id"` must be
quoted.)

> Remember the single-owner rule: if you opened an interactive `-t` session,
> exit it (`Ctrl-X`) before the next command.

## Step 5: See what's already on the board

The flash is **4 MB total**, divided into **eight 512 KB banks** (banks 0-7).
`-b N` selects bank N. Vendors often pre-flash DiagROM and a ROM Switcher into
banks 6 and 7.

```text
hostsmash_win64.exe -d com5 -t "prom bank show"
```

Representative output:

```text
Bank  Name            Merge LongReset  PowerOn  Current  NextReset
0     Kick3.2.3                        *
1     Kick3.X
...
6     DiagROM
7     ROM Switcher          0          *        *
```

The columns are **Bank, Name, Merge, LongReset, PowerOn, Current, NextReset**.
This is the layout you are about to modify.

## Step 6: Prepare your ROM image

KickSmash flashes a **plaintext** Kickstart image. It cannot use a
Cloanto-encrypted image directly.

- A **valid native Kickstart** begins with bytes `11 14 4E F9` (older
  256 KB-class images may begin `11 11 4E F9`).
- A **Cloanto-encrypted** image instead begins with the ASCII string
  `AMIROMTYPE1`. These must be decrypted first (via Amiga Forever or
  [amitools](https://amitools.readthedocs.io/en/latest/tools/romtool.html))
  before flashing.

Check your file's first 16 bytes in PowerShell:

```powershell
Format-Hex -Path .\A3000.47.115.rom -Count 16
```

Read the output's first row:

- Starts with `11 14 4E F9` (or `11 11 4E F9`) -- plaintext, good to flash.
- Starts with `41 4D 49 52 4F 4D 54 59 50 45 31` (`AMIROMTYPE1`) -- encrypted;
  decrypt it first.

**Version, for the curious.** Header bytes 12-15 encode the Kickstart version
as major.minor. For example, `A3000.47.115.rom` has bytes 12-15 =
`00 2F 00 73` = 47.115 = AmigaOS 3.2.3.

## Step 7: Write and verify a bank

This is the core action: one command to write, one to verify.

Write the image into bank 0:

```text
hostsmash_win64.exe -d com5 -b 0 -w A3000.47.115.rom -y
```

Expected output (the percentage line is abbreviated here):

```text
Auto swap mode: 32, Swapping 3210, Length 0x80000
Erase area before write?: yes
Erase sector(s) from 0x0 to 0x80000: yes
...
Writing 0x080000 bytes to EEPROM starting at address 0x0
0% ... 100%
Wrote 0x80000 bytes to device
```

**About auto-swap.** For a recognized A3000/A4000 ROM, hostsmash auto-detects
the byte order and applies swap `3210` for you, so you do **not** need a `-s`
flag. The banner reads `Auto swap mode: 32, Swapping 3210` -- note it says
**`32`** (the 32-bit mode), not `A3000`. (Some older docs show `A3000` here;
that text is stale.) This banner appears on **both** the write and the verify
step. The `-y` answers the erase/write prompts automatically.

> **Model note:** the A3000T uses a swapped layout and needs `-s 1032`. See
> [`WHICH.md`](WHICH.md) for which models need a swap flag. The A3000/A4000
> examples here need none.

Verify the write:

```text
hostsmash_win64.exe -d com5 -b 0 -v A3000.47.115.rom
```

It re-reads the bank, compares against your file, and ends with:

```text
Auto swap mode: 32, Swapping 3210, Length 0x80000
...
Verify success
```

## Step 8: Name the bank and set the power-on default

Give the bank a human-readable label (used by the ROM Switcher and
`prom bank show`):

```text
hostsmash_win64.exe -d com5 -t "prom bank name 0 Kick3.2.3"
```

Choose what the Amiga boots at cold power-on. To boot bank 0 directly:

```text
hostsmash_win64.exe -d com5 -t "prom bank poweron 0"
```

To instead get the ROM Switcher menu at power-on, set the power-on bank to the
switcher's bank (commonly 7):

```text
hostsmash_win64.exe -d com5 -t "prom bank poweron 7"
```

The setting persists to the board's config (you'll see a `config write at ...`
line). Re-run `prom bank show` (step 5) to confirm the `PowerOn` column moved.

The board is now **flashed, named, and configured**.

## Step 9: Install in the Amiga (hand-off)

Physical installation varies a lot by model and motherboard revision -- too many
variations to cover exhaustively here. **Find your specific board and Amiga in
[`WHICH.md`](WHICH.md), then follow [`hw_install.txt`](hw_install.txt) for the
full procedure.** The short A3000 checklist below is a heads-up, not a
substitute for those documents.

**A3000 quick checklist (verify against your board's revision):**

- **ROM speed jumpers:** set **J151 and J152 to 1-2** (25 MHz operation) so the
  CPU can talk to the STM32 reliably.
- **KBRST wire:** connect the Amiga's KBRST signal to the KickSmash KBRST pin.
  On the A3000, KBRST is at **RP701 pin 6** or **U713 pin 1**.
- **ROM Tower:** older A3000 boards (**rev -- 7**) require a ROM Tower adapter;
  see [`WHICH.md`](WHICH.md) and the
  [A3000 ROM tower replacement](https://github.com/cdhooper/amiga_rombankswitcher_a3000_romtower).
- **Orientation and other models:** the USB connector orientation and KBRST pin
  differ per model. **Defer to [`WHICH.md`](WHICH.md) and
  [`hw_install.txt`](hw_install.txt) for your exact board.**

That is the end of the host-side workflow. Hand off to the hardware docs above
to put the board in your Amiga.

## Troubleshooting

| Symptom | Fix | More |
| --- | --- | --- |
| Driver **Code 10 / Code 28** | Run the `wdi-simple.exe` command in step 2, then unplug/replug. | [`windows_notes.txt`](windows_notes.txt) |
| Command can't open the port / "port in use" | Only one session owns the port. Close any open `-t` session (`Ctrl-X`) and retry. | This guide, step 4 |
| Wrong COM port | hostsmash can't open the device. Re-check Device Manager -- Ports for the current `COMx`. | [`windows_notes.txt`](windows_notes.txt) |
| hostsmash rejects the image / reports an **unrecognized ROM format** | The image is Cloanto-encrypted (`AMIROMTYPE1` header) or otherwise not a recognized layout. Decrypt or obtain a plaintext image and re-check the header is `11 14 4E F9` (step 6). | This guide, step 6 |
| `0%Remote sent error 78` / CRC errors | Communication failure. Capture a log with `setx TERM_DEBUG out.txt`, then re-run; try a pacing delay with `-D <msec>`. | [`windows_notes.txt`](windows_notes.txt), [`sw_hostsmash.txt`](sw_hostsmash.txt) |
| Erase error during write | Usually the Amiga is powered off / KBRST not connected when programming an installed board, or a locked flash block. On the bench this should not happen. | [`sw_hostsmash.txt`](sw_hostsmash.txt) |

> On the exact unrecognized-ROM wording: hostsmash prints a message of the form
> `Unrecognized Amiga ROM format:` followed by the first four bytes it read.
> Treat any such rejection as "wrong/encrypted image" and return to step 6.

## Extended Reading

The documents below are grouped so the read order is obvious: **start here**,
then **host programming**, then **on the Amiga**, then **hardware**, then
**reference / off-path** for builders and firmware developers. Image files
(`*.jpg`, `*.png`) in `doc/` are illustrative assets referenced by these
documents, not standalone reading.

### Start here

| Document | What it covers | Read this when |
| --- | --- | --- |
| [`getting_started_windows.md`](getting_started_windows.md) | This end-to-end Windows flashing + configuration walkthrough. | You are on Windows and want the literal commands to flash a ROM. |
| [`FAQ.txt`](FAQ.txt) | Why KickSmash exists, supported models, and a worked "program a new ROM and switch to it" recipe. | Background on why KickSmash exists -- optional before you start; useful for the program-and-switch pattern. |
| [`WHICH.md`](WHICH.md) | Which KickSmash board fits which Amiga, socket spacing, install orientation, KBRST pin per board, ROM-tower note, and per-model swap flags. | Before buying or installing -- confirm your board and model. |

### Host programming (this guide's path)

| Document | What it covers | Read this when |
| --- | --- | --- |
| [`sw_hostsmash.txt`](sw_hostsmash.txt) | The authoritative host-utility reference: every flag, byte-swap modes, read/write/verify/erase, file serving. | You need a flag this guide didn't use, or deeper detail on swap modes. |
| [`windows_notes.txt`](windows_notes.txt) | Windows COM-port driver fix, DFU on Windows, hostsmash debugging, COM-port arbiter trick. | On Windows when the device won't talk or you're scripting multiple boards. |
| [`sw_install.txt`](sw_install.txt) | Installing firmware updates (`reset dfu`) and copying the Amiga-side utilities. | After hardware programming, before Amiga-side use. |
| [`sw_kicksmash.txt`](sw_kicksmash.txt) | The KickSmash firmware CLI command set (what `-t "..."` commands exist). | You want the full set of `prom ...` and other firmware commands. |

### On the Amiga

| Document | What it covers | Read this when |
| --- | --- | --- |
| [`AMIGACLI.md`](AMIGACLI.md) | Screenshot walkthrough of the AmigaOS `smash` bank show/write/name/verify flow. | You want the in-system (Amiga-side) equivalent of this guide. |
| [`SWITCHER.md`](SWITCHER.md) | What the ROM Switcher is (1.3 linked vs. 1.5+ stand-alone) and how to set it for power-on / long-reset. | You want a boot menu to pick ROMs. |
| [`sw_smash.txt`](sw_smash.txt) | The AmigaOS `smash` utility reference for in-system programming and switching. | Programming from the Amiga itself. |
| [`sw_smashfs.txt`](sw_smashfs.txt) | The AmigaOS `smashfs` auto-mount filesystem for host-exported volumes. | Sharing files between host and Amiga. |
| [`sw_smashfsrom.txt`](sw_smashfsrom.txt) | The ROMable variant of `smashfs`. | Embedding the filesystem in a ROM. |
| [`sw_smashftp.txt`](sw_smashftp.txt) | The AmigaOS `smashftp` file-transfer utility. | Moving files between host and Amiga. |

### Hardware

| Document | What it covers | Read this when |
| --- | --- | --- |
| [`hw_install.txt`](hw_install.txt) | Physical install: KBRST wiring per model, ROM-speed jumpers, board orientation, post-install debugging. | Putting the board into the Amiga (step 9 hands off here). |
| [`hw_programming.txt`](hw_programming.txt) | Loading STM32 firmware via ST-Link or USB DFU, plus the stand-alone pin / `prom id` health check. | Firmware is not yet loaded, or you're building a board. |
| [`usb_cables.txt`](usb_cables.txt) | USB-C cable fit recommendations for the tight ROM-socket clearance. | Sourcing a cable. |

### Reference / off-path (builders and firmware developers)

| Document | What it covers | Read this when |
| --- | --- | --- |
| [`hw_build.txt`](hw_build.txt) | PCB fabrication and build tips for manufacturing the board. | You are building the board, not using a finished one. |
| [`DEVENV.md`](DEVENV.md) | Building all the software via the VSCode devcontainer (Docker/Fedora). | Compiling firmware/utilities/host tools from source. |
| [`sw_build.txt`](sw_build.txt) | Build prerequisites and packages (including Raspberry Pi) for compiling hostsmash. | Building the host tools from source. |
| [`release_notes.txt`](release_notes.txt) | Per-release changelog (firmware / Amiga / host). | Checking what changed in a given release. |
