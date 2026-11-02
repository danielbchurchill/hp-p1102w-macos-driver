# HP LaserJet P1102w driver for macOS

A small, native CUPS driver for the **HP LaserJet Professional P1102 / P1102w**
on current macOS, including Apple Silicon. It needs no Ghostscript, no
Homebrew and no HP software at runtime.

HP never shipped an Apple Silicon driver for this printer, and recent macOS
releases don't include one. The printer is *host-based*: it has no PostScript
or PCL interpreter, so the Mac must render every page to a bitmap and send it
in HP/Zenographics **ZjStream** format. Generic drivers can't do that.

**Status:** tested on macOS 27.0.1 (Apple Silicon) with a P1102w over USB. The
test page prints at the correct scale (2.00 × 2.00 in reference square), and
every step of the gray ramp is distinct.

## How it works

```
app ─► PDF ─► cgpdftoraster ─► rastertozjs ─► foo2zjs ─► usb backend ─► printer
              (macOS built-in)  (this repo)    (-z2 -P -L0)
```

1. **`cgpdftoraster`** ships with macOS. Using Apple's own graphics engine, it
   renders each page to 8-bit grayscale at 1200 × 600 dpi, the engine's native
   resolution.
2. **`rastertozjs`** (`src/rastertozjs.c`) halftones that to 1 bit with a
   clustered-dot screen (about 106 lpi, 45°), places it on the full sheet, maps
   print-dialog options to printer codes, and streams PBM pages to the encoder.
3. **`foo2zjs`** (vendored, unmodified) JBIG-compresses each page and wraps it
   in ZjStream with the P1102's model quirks: `-z2`, no `START_PLANE` records
   (`-P`), and JBIG `MX = 0` to work around a firmware bug.

### Why no Ghostscript?

Other macOS ports of foo2zjs pipe PostScript through Ghostscript. The macOS
CUPS sandbox won't let filters run binaries from `/opt/homebrew`, so those
ports copy about 75 MB of Ghostscript and its libraries into `/Library/Printers`.
Using the built-in rasterizer avoids all of that. The installed driver is two
small universal binaries (arm64 + x86_64) and one PPD, in
`/Library/Printers/HP_P1102w_ZJS/`.

### No half-sent jobs

If a job is interrupted after its header but before the page data, the P1102
waits for data forever with a blinking Ready light, and ignores later jobs
until you power-cycle it. `rastertozjs` therefore buffers the whole encoded job
and sends nothing to the printer unless every page rendered successfully.

## Install

Requires the Xcode Command Line Tools (`xcode-select --install`).

```sh
git clone https://github.com/danielbchurchill/hp-p1102w-macos-driver.git
cd hp-p1102w-macos-driver
make            # build (as yourself, not root)
make test       # offline: renders a test page and validates the ZjStream
sudo ./install.sh
lp -d HP_LaserJet_P1102w build/testpage.pdf
```

`install.sh` installs the driver, runs a self-test in an empty environment
(the way CUPS runs filters), finds the printer on USB and creates the
`HP_LaserJet_P1102w` queue.

| Option | Effect |
|---|---|
| `--paper A4` | Default paper size (default `Letter`) |
| `--queue NAME` | Queue name (default `HP_LaserJet_P1102w`) |
| `--no-queue` | Install the driver only. Add the printer yourself in System Settings › Printers & Scanners. |

If `sudo` can't prompt for a password (for example, inside a tool that has no
terminal), use the macOS admin dialog instead:

```sh
osascript -e 'do shell script "'"$PWD"'/install.sh 2>&1" with administrator privileges'
```

Uninstall with `sudo ./uninstall.sh`, which removes the driver and any queues
that use it.

On the test page, the square must measure exactly **2.00 × 2.00 in**. If it
comes out half or double width, the resolution is wrong.

## Print dialog options

- **Paper size:** Letter, Legal, Executive, A4, A5, A6, JIS B5, Envelope #10 / DL / C5 / Monarch, Postcard
- **Paper type:** Plain, Light, Recycled, Cardstock, Rough, Labels, Envelope, Transparency, Letterhead, Preprinted, Prepunched
- **Print density:** 1 (lightest) to 5 (darkest)
- **Toner saver:** on or off

The printer has no duplexer.

## Troubleshooting

**Green light blinks and nothing prints.** The printer is waiting for the rest
of an interrupted job. Turn it off and on. To ask the printer for its status
(`CODE=10023` means busy or waiting; `40xxx` and `41xxx` codes are paper, jam,
door or toner problems):

```sh
printf '\033%%-12345X@PJL\n@PJL INFO STATUS\n\033%%-12345X' > /tmp/probe.pjl
URI=$(/usr/libexec/cups/backend/usb | grep -i 1102 | awk '{print $2}')
DEVICE_URI="$URI" /usr/libexec/cups/backend/usb 1 me probe 1 "" /tmp/probe.pjl 3>&1
```

**A job fails.** Turn on CUPS debug logging, print, then read the filter's log:

```sh
cupsctl --debug-logging
lp -d HP_LaserJet_P1102w build/testpage.pdf
grep -a rastertozjs /var/log/cups/error_log | tail
cupsctl --no-debug-logging
```

A healthy one-page job reports roughly 50–100 KB of ZjStream.

**Printer not found on USB.** Check `lpinfo -v | grep usb`. Some units ship
with HP *Smart Install* enabled, which makes them appear as a virtual CD-ROM.
Disable it from the printer's setup utility.

**"Printer drivers are deprecated" when installing.** This is a standard CUPS
notice about PPD-based drivers. They still work on macOS 27.

**Other models.** foo2zjs also drives the P1566 and P1606dn with the same
`-z2` flags, so this driver will probably work for them. They are untested,
and their PPD `*Product` and `1284DeviceID` lines would need adding.

## Repository layout

| Path | Contents |
|---|---|
| `src/rastertozjs.c` | CUPS raster filter; depends only on the system `libcups` |
| `ppd/HP-LaserJet-P1102w-ZJS.ppd` | Printer description: page sizes, margins, options |
| `vendor/foo2zjs/` | Unmodified subset of upstream foo2zjs (see [`UPSTREAM.md`](vendor/foo2zjs/UPSTREAM.md)) |
| `tools/make_testpage.py` | Test page generator (Python standard library only) |
| `install.sh`, `uninstall.sh` | System install and removal |

## Sources and credits

- **foo2zjs** by Rick Richardson. The ZjStream encoder and decoder vendored
  here. The P1102 model flags (`-z2 -P -L0`), paper and media codes, 1200 × 600
  resolution and 0.16 in engine clip margins all come from its `foo2zjs.c`
  and `foo2zjs-wrapper`.
  Mirror: <https://github.com/koenkooi/foo2zjs> (commit `e04290d`)
- **JBIG-KIT** by Markus Kuhn, bundled with foo2zjs.
  <https://www.cl.cam.ac.uk/~mgk25/jbigkit/>
- **foo2zjs ChangeLog, 2011-11-12.** Documents the P1102 / CP1025 firmware
  bug that needs JBIG `MX = 0`.
- **[cragkhit/hp-p1102-macos](https://github.com/cragkhit/hp-p1102-macos).**
  The first working foo2zjs port for modern macOS. Its notes on the CUPS
  sandbox blocking `/opt/homebrew`, and on the silent "header only, blinking
  light" failure, shaped this driver's Ghostscript-free design and whole-job
  buffering.
- **[grosa787/hp-p1102-macos](https://github.com/grosa787/hp-p1102-macos).**
  A fork of the above, verified on macOS 27. Source of the 1200 × 600 raster
  note and the PJL status probe in Troubleshooting.
- **[faradayfury/printer-all PR #3](https://github.com/faradayfury/printer-all/pull/3).**
  "Fixes for HP LaserJet Pro P1102", explaining why the P1102 needs `-P` and
  `MX = 0`.
- **[HP Support Community: P1102w driver for macOS Sequoia](https://h30434.www3.hp.com/t5/Printer-Setup-Software-Drivers/HP-LaserJet-Pro-P1102w-printer-driver-for-macOS-Sequoia-15-1/td-p/9248220)**
  and
  **[P1102 not working with new macOS](https://h30434.www3.hp.com/t5/Printer-Setup-Software-Drivers/HP-LaserJet-P1102-not-working-with-new-MacOS/td-p/9237883).**
  Confirm that no current HP driver exists for this printer on Apple Silicon.

Written with help from Claude Code (Anthropic).

## License

GPL-2.0-or-later. See [`LICENSE`](LICENSE). The vendored foo2zjs and JBIG-KIT
sources keep their original copyright notices.
