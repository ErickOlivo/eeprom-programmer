#!/usr/bin/env python3
"""
============================================================================
 image_to_eeprom.py

 Converts any picture to 100 x 75 pixels with 64 colors (RRGGBB format) and
 sends it to the Arduino that programs the AT28C256 of the VGA video card.
 Supports two images in the same chip (bank 0 and bank 1, selected by A14).

 Firmware companion:  firmware/vga_eeprom_programmer/vga_eeprom_programmer.ino

 Requirements:   pip install pillow pyserial      (see README.md)

----------------------------------------------------------------------------
 TYPICAL USE

   1) Convert and look at the result BEFORE spending ~80 s writing:
        python image_to_eeprom.py convert photo.jpg -o image.bin --preview preview.png

   2) Write it to bank 0 (image 1) and verify:
        python image_to_eeprom.py write image.bin --port COM3 --bank 0 --verify

   3) Write a second image to bank 1 (image 2):
        python image_to_eeprom.py write image2.bin --port COM3 --bank 1 --verify

   4) Re-check bank 0 afterwards (proves bank 1 did not overwrite it):
        python image_to_eeprom.py verify image.bin --port COM3 --bank 0

   5) Download what is in the chip as a PNG:
        python image_to_eeprom.py download --port COM3 --bank 1 -o read_back.png

 On Linux/macOS the port is usually /dev/ttyUSB0 or /dev/cu.usbserial-XXXX.

 Spanish aliases from the original version are still accepted:
   convertir / grabar / verificar / descargar, --puerto, --banco,
   --verificar, --vista, --sin-dither, --salida.

----------------------------------------------------------------------------
 OUTPUT FORMAT

 A .bin file of 7500 bytes: 75 rows of 100 bytes, no padding. The 28 padding
 bytes per row (to reach the 128-byte row stride) are NOT included because
 those columns fall in the horizontal blanking interval and are never read;
 the firmware skips them. The file is not padded to 32 KB either: that is a
 requirement of the minipro programmer software, not of the hardware.

 Each byte:   0 0 R R G G B B    with R, G, B from 0 to 3.

 If your DAC is wired in a different order (e.g. blue on the high bits),
 change the ORDER constant below. It is the only place you need to touch.
============================================================================
"""

import argparse
import sys
import time

WIDTH, HEIGHT = 100, 75
NBYTES = WIDTH * HEIGHT          # 7500

# Channel order inside the byte, from the most to the least significant pair.
# "RGB" -> bits 5-4 = red, 3-2 = green, 1-0 = blue.
ORDER = "RGB"

_POS = {c: (4 - 2 * i) for i, c in enumerate(ORDER)}   # channel -> shift


def pack(r, g, b):
    """r, g, b in 0..3  ->  byte in the selected format."""
    return (r << _POS["R"]) | (g << _POS["G"]) | (b << _POS["B"])


def unpack(v):
    """byte -> (r, g, b) in 0..3."""
    return ((v >> _POS["R"]) & 3, (v >> _POS["G"]) & 3, (v >> _POS["B"]) & 3)


def palette():
    """The 64 colors as 8-bit RGB. 255 / 3 = 85 per step."""
    return [tuple(c * 85 for c in unpack(i)) for i in range(64)]


# ---------------------------------------------------------------------------
# Image conversion
# ---------------------------------------------------------------------------

def convert(path, dither=True):
    """Opens the image, crops it to 4:3, resizes to 100x75 and quantizes it."""
    from PIL import Image

    im = Image.open(path).convert("RGB")
    w, h = im.size

    # Centered crop to 4:3 so the picture is not distorted.
    if w * HEIGHT > h * WIDTH:
        nw = h * WIDTH // HEIGHT
        x0 = (w - nw) // 2
        im = im.crop((x0, 0, x0 + nw, h))
    else:
        nh = w * HEIGHT // WIDTH
        y0 = (h - nh) // 2
        im = im.crop((0, y0, w, y0 + nh))

    im = im.resize((WIDTH, HEIGHT), Image.LANCZOS)

    # Quantization. The palette is a uniform cube with 4 levels per channel,
    # so rounding each channel independently ALREADY gives the nearest color:
    # no table lookup is needed.
    #
    # With dithering (Floyd-Steinberg) the error of each pixel is spread over
    # the neighbours not yet processed. With only 64 colors this makes a big
    # difference on photos (smoother gradients), but it adds "confetti" noise
    # to text and flat graphics, where --no-dither usually looks better.
    raw = im.tobytes()                       # R, G, B, R, G, B, ...
    buf = [[raw[3 * i], raw[3 * i + 1], raw[3 * i + 2]] for i in range(NBYTES)]
    out = bytearray(NBYTES)

    for y in range(HEIGHT):
        for x in range(WIDTH):
            i = y * WIDTH + x
            old = buf[i]
            lvl = [min(3, max(0, int(round(c / 85.0)))) for c in old]
            out[i] = pack(*lvl)
            if dither:
                err = [old[k] - lvl[k] * 85 for k in range(3)]
                for dx, dy, f in ((1, 0, 7 / 16), (-1, 1, 3 / 16),
                                  (0, 1, 5 / 16), (1, 1, 1 / 16)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < WIDTH and 0 <= ny < HEIGHT:
                        j = ny * WIDTH + nx
                        for k in range(3):
                            buf[j][k] += err[k] * f

    return bytes(out)


def save_preview(data, path, scale=6):
    """Saves an enlarged PNG with the exact colors the monitor will show."""
    from PIL import Image
    pal = palette()
    im = Image.new("RGB", (WIDTH, HEIGHT))
    im.putdata([pal[b & 0x3F] for b in data])
    im.resize((WIDTH * scale, HEIGHT * scale), Image.NEAREST).save(path)


# ---------------------------------------------------------------------------
# Serial port
# ---------------------------------------------------------------------------

def open_port(port, baud=57600, timeout=10):
    import serial
    s = serial.Serial(port, baud, timeout=timeout)
    time.sleep(2.5)              # the Nano resets when the port is opened
    s.reset_input_buffer()
    return s


def wait_for(s, token, limit=25):
    """Reads until the token is found and consumes the rest of the line."""
    target = token.encode()
    buf = b""
    t0 = time.time()
    while time.time() - t0 < limit:
        ch = s.read(1)
        if not ch:
            continue
        buf += ch
        if buf.endswith(target):
            s.readline()
            return True
        if len(buf) > 4096:
            buf = buf[-64:]
    return False


def write(s, data, bank=0):
    if len(data) != NBYTES:
        sys.exit(f"The file has {len(data)} bytes; {NBYTES} were expected.")
    s.reset_input_buffer()
    s.write(b"W" if bank else b"w")
    if not wait_for(s, "READY"):
        sys.exit("The Arduino did not answer READY. Check the port and the baud "
                 "rate (57600).\nIf you were writing bank 1: the firmware may have "
                 "detected A14 as a mirror (pin 1 on GND?). Open the serial "
                 "monitor and use command k.")

    t0 = time.time()
    for i, b in enumerate(data):
        s.write(bytes([b]))
        a = s.read(1)
        if a != b"\x06":
            sys.exit(f"\nNo ACK at byte {i} (got {a!r}). "
                     f"Row {i // WIDTH}, column {i % WIDTH}.")
        if i % 250 == 0:
            pct = 100.0 * i / NBYTES
            print(f"\r  writing  {pct:5.1f} %   ({i}/{NBYTES})", end="", flush=True)
    print(f"\r  writing  100.0 %   ({NBYTES}/{NBYTES})")
    print(f"  time: {time.time() - t0:.0f} s")


def read(s, bank=0):
    s.reset_input_buffer()
    s.write(b"R" if bank else b"r")
    if not wait_for(s, "DATA"):
        sys.exit("The Arduino did not answer DATA.")
    previous, s.timeout = s.timeout, 30
    got = s.read(NBYTES)
    s.timeout = previous
    if len(got) != NBYTES:
        sys.exit(f"Only {len(got)} of {NBYTES} bytes arrived.")
    return got


def compare(expected, got, bank=0):
    bad = [i for i in range(NBYTES) if expected[i] != got[i]]
    if not bad:
        print(f"  verification: {NBYTES} of {NBYTES} bytes correct.")
        return 0

    print(f"  verification: {len(bad)} bytes differ out of {NBYTES}.")
    for i in bad[:10]:
        y, x = divmod(i, WIDTH)
        addr = (bank << 14) | (y << 7) | x
        print(f"    row {y:2d} col {x:3d} (addr 0x{addr:04X}): "
              f"written 0x{expected[i]:02X}, read 0x{got[i]:02X}")

    # Row diagnosis: if errors concentrate in rows 64 to 74, A13 (pin 26)
    # is almost certainly not reaching QF of the 595.
    high = sum(1 for i in bad if i // WIDTH >= 64)
    if high > len(bad) * 0.9:
        print("\n  WARNING: almost all errors are in rows 64 to 74.")
        print("  That is the signature of A13 (pin 26) unconnected or grounded:")
        print("  with A13 stuck at 0, nothing above row 63 can be addressed.")

    # Bit diagnosis: a single failing bit points to one data line.
    mask = 0
    for i in bad:
        mask |= expected[i] ^ got[i]
    bits = [k for k in range(8) if mask & (1 << k)]
    if len(bits) == 1:
        print(f"\n  WARNING: all errors are in bit {bits[0]} "
              f"(I/O{bits[0]}, chip pin {[11, 12, 13, 15, 16, 17, 18, 19][bits[0]]}).")
    return len(bad)


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------

def _add_bank(p):
    p.add_argument("--bank", "--banco", dest="bank", type=int, choices=[0, 1],
                   default=0,
                   help="0 = image 1 (A14=0, default), 1 = image 2 (A14=1)")


def main():
    ap = argparse.ArgumentParser(
        description="Converts and writes images to the AT28C256 of the VGA card.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("convert", aliases=["convertir"],
                       help="picture -> 7500-byte .bin")
    c.add_argument("image")
    c.add_argument("-o", "--output", "--salida", dest="output", default="image.bin")
    c.add_argument("--preview", "--vista", dest="preview",
                   help="also save an enlarged PNG of how it will look")
    c.add_argument("--no-dither", "--sin-dither", dest="no_dither",
                   action="store_true",
                   help="quantize without Floyd-Steinberg (cleaner text, "
                        "visible bands in gradients)")

    w = sub.add_parser("write", aliases=["grabar"], help="send the .bin to the Arduino")
    w.add_argument("binary")
    w.add_argument("--port", "--puerto", dest="port", required=True)
    _add_bank(w)
    w.add_argument("--verify", "--verificar", dest="verify", action="store_true",
                   help="read the chip back afterwards and compare")

    v = sub.add_parser("verify", aliases=["verificar"],
                       help="read the chip back and compare with a .bin")
    v.add_argument("binary")
    v.add_argument("--port", "--puerto", dest="port", required=True)
    _add_bank(v)

    d = sub.add_parser("download", aliases=["descargar"],
                       help="save what is in the chip as a PNG")
    d.add_argument("--port", "--puerto", dest="port", required=True)
    _add_bank(d)
    d.add_argument("-o", "--output", "--salida", dest="output", default="read_back.png")

    a = ap.parse_args()
    cmd = {"convertir": "convert", "grabar": "write",
           "verificar": "verify", "descargar": "download"}.get(a.cmd, a.cmd)

    if cmd == "convert":
        data = convert(a.image, dither=not a.no_dither)
        with open(a.output, "wb") as f:
            f.write(data)
        print(f"  {a.output}: {len(data)} bytes ({WIDTH}x{HEIGHT}, 64 colors)")
        print(f"  distinct colors used: {len(set(data))} of 64")
        if a.preview:
            save_preview(data, a.preview)
            print(f"  preview: {a.preview}")

    elif cmd == "write":
        data = open(a.binary, "rb").read()
        s = open_port(a.port)
        try:
            write(s, data, a.bank)
            if a.verify:
                compare(data, read(s, a.bank), a.bank)
        finally:
            s.close()

    elif cmd == "verify":
        data = open(a.binary, "rb").read()
        s = open_port(a.port)
        try:
            compare(data, read(s, a.bank), a.bank)
        finally:
            s.close()

    elif cmd == "download":
        s = open_port(a.port)
        try:
            data = read(s, a.bank)
        finally:
            s.close()
        save_preview(data, a.output)
        print(f"  {a.output}")


if __name__ == "__main__":
    main()
