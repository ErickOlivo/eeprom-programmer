# Two images in one EEPROM: an AT28C256 programmer for a breadboard VGA card

This repository contains the firmware and the PC tool we used to store **two 100 × 75 pixel, 64‑color images** in a single **AT28C256** EEPROM and display either of them on a VGA monitor with a Ben Eater–style "world's worst video card" built from 74‑series logic.

- **Firmware** for an Arduino Nano that programs the EEPROM through two 74HC595 shift registers.
- **A Python tool** that converts any picture to the card's format, sends it to the Arduino, and verifies what was written.
- **Bank switching**: the 32 KB chip is split into two 16 KB halves selected by address line **A14**, so one switch on the video card chooses which image is displayed.

Everything here was used on real hardware as part of the Computer Architecture course (ECMC1016) at Yachay Tech University, Ecuador.

---

## Table of contents

1. [Results](#1-results)
2. [How it works](#2-how-it-works)
3. [Hardware](#3-hardware)
4. [Software setup](#4-software-setup)
5. [Step-by-step: writing two images](#5-step-by-step-writing-two-images)
6. [Reference: firmware commands](#6-reference-firmware-commands)
7. [Reference: Python tool](#7-reference-python-tool)
8. [Serial protocol](#8-serial-protocol)
9. [Troubleshooting](#9-troubleshooting)
10. [Known limitations](#10-known-limitations)
11. [Repository layout](#11-repository-layout)
12. [Credits](#12-credits)

---

## 1. Results

Two images were converted, written to the two banks of the same AT28C256 and read back by the programmer:

| Step | Bank | Bytes verified | Write time |
|---|---|---|---|
| Write image 1 with `--verify` | 0 (A14 = 0) | 7500 / 7500 | 84 s |
| Write image 2 with `--verify` | 1 (A14 = 1) | 7500 / 7500 | 84 s |
| Re-verify image 1 **after** writing image 2 | 0 | 7500 / 7500 | — |

The third row is the key evidence: bank 0 was unchanged after writing bank 1, which proves that A14 really reaches the chip and that both images coexist. On the video card, moving pin 1 (A14) between GND and +5 V switches between the two pictures.

The measured 84 s per image is slightly above the theoretical 75 s (7500 bytes × 10 ms) because of the per-byte acknowledgment over the serial link.

---

## 2. How it works

### 2.1 VGA timing (inherited from Ben Eater's design)

The card generates 800 × 600 @ 60 Hz timing with a **10 MHz** clock instead of the standard 40 MHz. A monitor measures absolute times, not pixels, so dividing every horizontal count by 4 keeps the same microsecond timings and gives an effective resolution of 200 × 600.

| Horizontal (100 ns per count) | Counts | Duration |
|---|---|---|
| Visible area | 0–199 | 20.0 µs |
| Front porch | 200–209 | 1.0 µs |
| Horizontal sync | 210–241 | 3.2 µs |
| Back porch | 242–263 | 2.2 µs |
| **Whole line** | 264 | **26.4 µs (≈ 37.88 kHz)** |

| Vertical (one count per line) | Lines | Duration |
|---|---|---|
| Visible area | 0–599 | 15.84 ms |
| Front porch | 600 | 26.4 µs |
| Vertical sync | 601–604 | 105.6 µs |
| Back porch | 605–627 | 607.2 µs |
| **Whole frame** | 628 | **16.579 ms (≈ 60.3 Hz)** |

### 2.2 From pixel counters to EEPROM addresses

The card drops the least significant bit of the horizontal counter (200 / 2 = 100 columns) and the three least significant bits of the vertical counter (600 / 8 = 75 rows). Both remaining values fit in 7 bits, so the EEPROM address is:

```
address = (bank << 14) | (Y << 7) | X        X = 0..99   Y = 0..74
          A14            A13..A7    A6..A0
```

Consequences:

- Each row occupies a **128-byte stride**, but only 100 bytes are visible. Columns 100–127 fall in horizontal blanking, are never read, and are **not written** by this firmware (saving 2100 writes ≈ 21 s).
- One image spans addresses 0x0000–0x257F, using only 7500 of the 16 384 bytes in a bank.
- **A13 is used** (it is the top bit of Y). With A13 grounded only rows 0–63 can be addressed.
- **A14 is not driven by the counters.** That is what makes it free to act as an external bank-select bit.

### 2.3 Two banks with A14

| Bank | A14 | Address range | Content |
|---|---|---|---|
| 0 | 0 | 0x0000–0x257F | Image 1 |
| 1 | 1 | 0x4000–0x657F | Image 2 |

The counters generate exactly the same A0–A13 sequence in both cases; A14 only decides which half of the chip answers. On the programmer, A14 must be driven by the shift register so both halves can be written. On the video card, A14 goes to a switch.

### 2.4 Color format and DAC

Each byte holds one pixel:

```
bit:  7 6 5 4 3 2 1 0
      0 0 R R G G B B      (each channel 0..3, 4 × 4 × 4 = 64 colors)
```

Each channel is converted to a voltage with two resistors forming a divider against the monitor's 75 Ω input termination. The monitor expects 0–0.7 V. Our build uses **680 Ω** on the most significant bit of each channel and **1510 Ω** (1 kΩ + 510 Ω in series) on the least significant bit:

| Bits | Active resistance | Calculated level | Ideal (equally spaced) |
|---|---|---|---|
| 00 | none | 0.000 V | 0.000 V |
| 01 | 1510 Ω | 0.237 V | 0.233 V |
| 10 | 680 Ω | 0.497 V | 0.467 V |
| 11 | 1510 Ω ∥ 680 Ω ≈ 469 Ω | 0.689 V | 0.700 V |

Formula: `V = 5 V × 75 / (R + 75)`. The calculation assumes ideal 5 V outputs; real CMOS outputs under load give slightly lower values, so measure them on your build. Ben Eater's original uses 1.5 kΩ instead of 1510 Ω.

### 2.5 Blanking

Outside the visible window the EEPROM must not drive the color lines, or the monitor's black reference is disturbed (a blank EEPROM reads 0xFF, which is white). A NAND gate combines the horizontal and vertical "visible" signals and drives **/OE** of the EEPROM, putting its outputs in high impedance during blanking.

---

## 3. Hardware

### 3.1 Programmer bill of materials

- 1 × Arduino Nano (ATmega328P)
- 2 × 74HC595 shift registers
- 1 × AT28C256 EEPROM (32 KB, parallel, 28 pins)
- Breadboard, jumper wires, USB cable

The same programmer can be built for a 28C64: the 28C64B and the 28C256 share the JEDEC 28-pin pinout except pins 1 and 26 (not connected on the 28C64B, A14 and A13 on the 28C256).

### 3.2 Programmer wiring

| Arduino Nano | Goes to |
|---|---|
| D2 | SER (pin 14) of 595 #1 |
| D3 | SRCLK (pin 11) of both 595s |
| D4 | RCLK (pin 12) of both 595s |
| D5 … D12 | EEPROM I/O0 … I/O7 (pins 11, 12, 13, 15, 16, 17, 18, 19) |
| D13 | EEPROM /WE (pin 27) |

| 595 output | EEPROM signal (pin) |
|---|---|
| 595 #1 QA … QH | A0 … A7 (pins 10, 9, 8, 7, 6, 5, 4, 3) |
| 595 #1 QH' (pin 9) | SER of 595 #2 (daisy chain) |
| 595 #2 QA | A8 (pin 25) |
| 595 #2 QB | A9 (pin 24) |
| 595 #2 QC | A10 (pin 21) |
| 595 #2 QD | A11 (pin 23) |
| 595 #2 QE | A12 (pin 2) |
| 595 #2 QF | A13 (pin 26) |
| 595 #2 QG | **A14 (pin 1)** |
| 595 #2 QH | /OE (pin 22) |

Fixed connections: EEPROM /CE (pin 20) → GND, GND (pin 14) → GND, VCC (pin 28) → +5 V. On each 595: /SRCLR (pin 10) → +5 V and /OE (pin 13) → GND.

> **Important:** if A14 (pin 1) is tied to GND on the programmer, writing "bank 1" overwrites bank 0. A14 must come from QG of the second 595.

### 3.3 Video-card change for two images

Only one wire changes on the video card: EEPROM pin 1 (A14) goes to the common terminal of an SPDT switch whose other terminals go to GND and +5 V.

| Switch position | Pin 1 voltage | Image shown |
|---|---|---|
| GND | 0 V | Image 1 (bank 0) |
| +5 V | ≈ 5 V | Image 2 (bank 1) |

Never leave pin 1 floating: on the 28C256 it is a real CMOS input and a floating level can show random data or noise. A jumper to +5 V with a 10 kΩ pull-down to GND also works. Switching while the card is running is safe; the picture changes at once, possibly with a glitch on that frame.

### 3.4 Mixing logic families

The video card uses 74LS (TTL) logic and the EEPROM is CMOS. The AT28C256 inputs are TTL-compatible, and its CMOS outputs drive the resistor DAC directly. On the programmer, the 74HC595 outputs (CMOS, 5 V) drive the EEPROM inputs without issues.

---

## 4. Software setup

### 4.1 Firmware

1. Open `firmware/vga_eeprom_programmer/vga_eeprom_programmer.ino` in the Arduino IDE (or VS Code with an Arduino extension).
2. Board: **Arduino Nano**; processor: ATmega328P (use "Old Bootloader" if uploading fails).
3. Upload, then open the serial monitor at **57600 baud**. The help menu should appear.
4. **Close the serial monitor before using the Python tool**; otherwise the port is busy.

### 4.2 Python tool

Requirements: Python 3.8 or newer, plus Pillow and pySerial.

```
pip install -r tools/requirements.txt
```

**Windows with the MSYS2 Python** (`C:\msys64\ucrt64\bin\python.exe`): `pip` is usually blocked with an "externally-managed-environment" error. Install the packages with pacman instead. `pacman` is not available from PowerShell by name, so either open the MSYS2 terminal or call it with its full path:

```
C:\msys64\usr\bin\pacman.exe -S mingw-w64-ucrt-x86_64-python-pillow mingw-w64-ucrt-x86_64-python-pyserial
```

Check that both modules load:

```
python -c "import PIL, serial; print(PIL.__version__, serial.__version__)"
```

---

## 5. Step-by-step: writing two images

Run these from the folder that contains `image_to_eeprom.py` and your pictures. Replace `COM3` with your port (on Linux/macOS, something like `/dev/ttyUSB0`). Put file names with spaces in quotes.

**1. Check the hardware.** With the serial monitor open, send `a` (address lines) and `k` (A14 check). Then close the serial monitor.

**2. Convert both pictures**, with and without dithering, and compare the previews:

```
python image_to_eeprom.py convert "picture 1.jpg" -o img1.bin    --preview img1.png
python image_to_eeprom.py convert "picture 1.jpg" -o img1_nd.bin --preview img1_nd.png --no-dither
python image_to_eeprom.py convert picture2.jpg    -o img2.bin    --preview img2.png
python image_to_eeprom.py convert picture2.jpg    -o img2_nd.bin --preview img2_nd.png --no-dither
```

Rule of thumb: dithering helps photos and faces; `--no-dither` is usually better for text, logos and flat graphics.

**3. Write image 1 to bank 0:**

```
python image_to_eeprom.py write img1.bin --port COM3 --bank 0 --verify
```

**4. Write image 2 to bank 1:**

```
python image_to_eeprom.py write img2.bin --port COM3 --bank 1 --verify
```

**5. Re-verify bank 0** to prove that step 4 did not overwrite it:

```
python image_to_eeprom.py verify img1.bin --port COM3 --bank 0
```

Every step should end with `verification: 7500 of 7500 bytes correct.`

**6. Move the chip** to the video card (USB unplugged), wire pin 1 to the switch, and toggle between the two images.

---

## 6. Reference: firmware commands

Send single characters from the serial monitor (57600 baud). Lowercase acts on bank 0, uppercase on bank 1.

| Command | Bank 0 / Bank 1 | Action |
|---|---|---|
| Help | `?` | Shows the command list |
| Address test | `a` | Reads address 0 and each single-bit address A0–A14. A loose or shorted line returns the same value as another line. |
| A14 check | `k` | Compares 64 bytes of each bank and warns if they are identical (mirror) |
| Hex dump | `d` / `D` | First 4 rows in hexadecimal |
| Test pattern | `t` / `T` | 8 × 8 grid with all 64 colors and a 1-pixel border. `T` inverts the colors (black border) so you can tell banks apart on screen. |
| Receive image | `w` / `W` | Receives 7500 bytes and writes them (used by the Python tool) |
| Send image | `r` / `R` | Sends back 7500 raw bytes (used by the Python tool) |
| Erase | `z` / `Z` | Writes 0x00 over the image area of that bank |
| Disable SDP | `s` | Sends the Software Data Protection disable sequence (needs A14 on QG) |

Commands that write (`t`, `T`, `z`, `Z`, `s`) ask for confirmation: send an uppercase `Y` within 15 seconds.

**Why uppercase/lowercase instead of a "select bank" command?** Opening the serial port resets the Nano through the DTR line, so a bank chosen in advance would be forgotten. Encoding the bank in the command itself avoids that.

**Write safety.** In the idle state /WE is high and /OE is low; with /OE low the chip ignores write pulses in hardware. Only writing commands set an internal `armed` flag. Always move wires with the USB cable unplugged.

---

## 7. Reference: Python tool

```
python image_to_eeprom.py convert  IMAGE  [-o OUT.bin] [--preview OUT.png] [--no-dither]
python image_to_eeprom.py write    FILE.bin --port PORT [--bank 0|1] [--verify]
python image_to_eeprom.py verify   FILE.bin --port PORT [--bank 0|1]
python image_to_eeprom.py download          --port PORT [--bank 0|1] [-o OUT.png]
```

| Subcommand | What it does |
|---|---|
| `convert` | Centered crop to 4:3, resize to 100 × 75 (Lanczos), quantize each channel to 4 levels, optionally with Floyd–Steinberg dithering. Writes a 7500-byte `.bin` and reports how many of the 64 colors are used. `--preview` saves a 6× enlarged PNG with the exact colors the monitor will receive. |
| `write` | Sends the `.bin` to the given bank; with `--verify` it reads it back and compares. |
| `verify` | Reads a bank and compares it with a `.bin`. |
| `download` | Reads a bank and saves it as a PNG. |

The `--bank` option defaults to 0. The verifier also diagnoses common wiring faults: errors concentrated in rows 64–74 point to A13 (pin 26), and errors in a single bit point to one data line.

If your DAC is wired in a different channel order, edit the `ORDER` constant at the top of the script (for example `"BGR"`).

**Spanish aliases.** The original Spanish command names still work, so commands written for the earlier version keep running: `convertir`, `grabar`, `verificar`, `descargar`, `--puerto`, `--banco`, `--verificar`, `--vista`, `--sin-dither`, `--salida`.

---

## 8. Serial protocol

57600 baud, 8N1.

**Write (`w` for bank 0, `W` for bank 1):**

```
PC -> 'w' or 'W'
AR -> "READY\r\n"
repeat 7500 times:
    PC -> 1 data byte
    AR -> writes it, waits 10 ms, answers 0x06 (ACK)
AR -> "DONE 7500\r\n"
```

The per-byte ACK keeps the PC from overflowing the Nano's 64-byte receive buffer, because the PC can never go faster than the EEPROM's 10 ms write cycle. Bytes are sent row by row (Y = 0..74, X = 0..99), and the firmware computes each address.

**Read (`r` for bank 0, `R` for bank 1):**

```
PC -> 'r' or 'R'
AR -> "DATA\r\n"
AR -> 7500 raw bytes in the same row order
```

If the bank-1 mirror check fires, the firmware asks a human for confirmation instead of answering `READY`. The script then stops with an error, which is intentional: it is better than silently overwriting image 1.

---

## 9. Troubleshooting

| Symptom | Likely cause | What to check |
|---|---|---|
| `can't open file '...image_to_eeprom.py'` | The terminal is in another folder | `cd` into the folder that contains the script |
| `No module named 'serial'` or `'PIL'` | Missing Python packages | See [4.2](#42-python-tool); with MSYS2 use pacman |
| `could not open port 'COM3'` | Port busy | Close the Arduino serial monitor or IDE |
| `did not answer READY` (bank 1) | Firmware detected a mirror | Send `k`; check that pin 1 goes to QG of 595 #2 |
| Read-back is all `FF` after writing | Software Data Protection active, or a loose wire | Try `s` (needs A14 on QG), then `t` and `d` |
| Errors only in rows 64–74 | A13 (pin 26) not connected | Wire pin 26 to QF of 595 #2 |
| Errors always in the same bit | One data line | The tool prints the I/O number and chip pin |
| Same image in both switch positions | A14 not changing on the video card | Measure pin 1 against pin 14: 0 V or ≈ 5 V |
| Noise or a mix of both images | A14 floating or bad contact | Readings between ≈ 1.5 and 3.5 V on pin 1 mean a floating input |
| Black screen with the monitor awake | /OE polarity inverted, or no image data | An erased EEPROM shows white, not black. Check /OE (pin 22): period 26.4 µs, low for 20 µs |

---

## 10. Known limitations

- **Vertical streaks (EEPROM access time).** The AT28C256 needs up to t_ACC = 150 ns (‑15 speed grade) to settle its outputs after an address change, while the monitor samples every 25 ns (40 MHz). Up to 150 / 25 = 6 samples after each address change can be invalid, which appears as thin vertical lines, most visible on large flat areas. Verification on the programmer does not catch this, because the firmware waits 2 µs before every read. Real video hardware uses fast SRAM for this reason.
- **Dithering noise.** At 100 × 75 with 64 colors, Floyd–Steinberg dithering adds "confetti" to text and flat graphics. Compare both previews before writing.
- **Write time.** About 75–85 s per image because of the EEPROM's 10 ms write cycle. Data polling could reduce it to 3–5 ms per byte, at the cost of a new failure mode.
- **Mirror detection is a heuristic.** If both banks genuinely contain the same data (for example, a blank chip), `k` also reports a mirror. The definitive test is step 5 of section 5.
- The SDP disable sequence follows the published AT28C256 sequence; confirm it against the Microchip datasheet before relying on it.
- The English sources in `firmware/` and `tools/` are line-by-line translations of the Spanish versions in `legacy/` that produced the results in section 1. The Python tool was checked to produce byte-identical `.bin` files and the same serial behavior against a simulated programmer; the translated firmware passed a syntax check but has not yet been flashed to the board.

---

## 11. Repository layout

```
.
├── README.md
├── .gitignore
├── firmware/
│   └── vga_eeprom_programmer/
│       └── vga_eeprom_programmer.ino     Two-bank programmer (English)
├── tools/
│   ├── image_to_eeprom.py                Converter / writer / verifier (English)
│   └── requirements.txt
└── legacy/                               Original Spanish versions used in the lab
    ├── README.md
    ├── grabador_vga_28c256/
    │   └── grabador_vga_28c256.ino       Single-image programmer (v1)
    ├── grabador_vga_28c256_v2_bancos/
    │   └── grabador_vga_28c256_v2_bancos.ino   Two-bank programmer (v2)
    ├── imagen_a_eeprom.py                Single-image tool (v1)
    └── imagen_a_eeprom_para_dos_imagenes.py    Two-bank tool (v2)
```

Each Arduino sketch sits in a folder with the same name, as the Arduino IDE requires.

---

## 12. Credits

- Video-card design: Ben Eater, *The world's worst video card?* and *World's worst video card? The exciting conclusion* (2019), on YouTube.
- Programmer, two-bank extension, PC tool and documentation: Jean C. Chapa R. and Erick Olivo, Computer Architecture (ECMC1016), Yachay Tech University, Second Semester 2026.
