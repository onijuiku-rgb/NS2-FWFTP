# ns2-fwftp

A kaiHEN homebrew app (NRO) that serves your Switch 2's installed system firmware
(every system program and system data title) over read-only FTP on port 1337, so
you can copy it to a PC. Nothing is written to the SD card except
`sdmc:/switch/ns2-fwftp/log.txt`.

## Use

1. Copy `ns2-fwftp.nro` to `sdmc:/switch/` and launch it from hbmenu.
2. Connect any FTP client to the address on screen (port 1337, any login, passive
   mode). FileZilla, lftp, curl and wget work.
3. Download what you want. Press + on the console to quit.

```
/index.txt                       every title: id, type, version, content id, NCA size
/log.txt                         this session's log
/probe.txt                       every filesystem open attempt and its result
/<id>/exefs/...                  a system program's code (main, main.npdm, ...)
/<id>/romfs/...                  RomFS of a program or system data title
/raw/<id>_<content id>.nca       NCA files as stored (encrypted)
```

The home menu is `0400000000001000`. Most system modules have no RomFS, so their
folders contain only `exefs`.

Your dump is your console's copyrighted firmware and includes console-specific
files such as an eShop client certificate. Don't post it publicly.

## Firmware support

| Firmware | Status |
| --- | --- |
| 23.0.1 | Tested: everything dumps, no errors |
| 23.0.0 | Expected to work |
| 22.x - 19.x | Untested. System data and ExeFS should work. Program RomFS (home menu and applets) uses a 23.0.0+ command and will likely fail |

## Reporting problems

If something fails, open an issue with your firmware version and these two files
from the FTP root:

- `probe.txt`: every method tried to open each title and its error code
- `log.txt`: the session log

Errors are shown as system error codes, such as 2002-1001.

## Build

```sh
make            # needs ounce-devkit (libounce, libkaihen) installed in devkitPro
```
