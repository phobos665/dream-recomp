"""IP.BIN: the 32 KB boot header at the start of a Dreamcast disc's data track.

Field layout (offsets in bytes) follows Marcus Comstedt's documentation:

    0x00  16  Hardware ID       "SEGA SEGAKATANA "
    0x10  16  Maker ID          "SEGA ENTERPRISES"
    0x20  16  Device info       "<CRC> GD-ROM<n>/<m>"
    0x30   8  Area symbols      "JUE     " (Japan, USA, Europe)
    0x38   8  Peripherals       hex bitfield as ASCII
    0x40  10  Product number
    0x4A   6  Product version
    0x50  16  Release date      YYYYMMDD
    0x60  16  Boot filename     usually "1ST_READ.BIN"
    0x70  16  Company name
    0x80 128  Software name
    0x100 ... bootstrap code

The CRC at 0x20 is a CRC-16 (poly 0x1021, init 0xFFFF) over the 16 bytes at 0x40.
"""

from __future__ import annotations

from dataclasses import dataclass

IPBIN_SIZE = 0x8000
IPBIN_LOAD_ADDRESS = 0x8C008000
BOOT_LOAD_ADDRESS = 0x8C010000

PERIPHERAL_BITS = [
    (0x0000001, "Windows CE"),
    (0x0000010, "VGA box"),
    (0x0000100, "Other expansions"),
    (0x0000200, "Puru Puru pack"),
    (0x0000400, "Microphone"),
    (0x0000800, "Memory card (VMU)"),
    (0x0001000, "Start, A, B, D-pad"),
    (0x0002000, "C button"),
    (0x0004000, "D button"),
    (0x0008000, "X button"),
    (0x0010000, "Y button"),
    (0x0020000, "Z button"),
    (0x0040000, "Expanded direction buttons"),
    (0x0080000, "Analog R trigger"),
    (0x0100000, "Analog L trigger"),
    (0x0200000, "Analog horizontal"),
    (0x0400000, "Analog vertical"),
    (0x0800000, "Expanded analog horizontal"),
    (0x1000000, "Expanded analog vertical"),
    (0x2000000, "Light gun"),
    (0x4000000, "Keyboard"),
    (0x8000000, "Mouse"),
]


def crc16(data: bytes) -> int:
    n = 0xFFFF
    for b in data:
        n ^= b << 8
        for _ in range(8):
            n = ((n << 1) ^ 0x1021) if n & 0x8000 else (n << 1)
            n &= 0xFFFF
    return n


def _s(raw: bytes) -> str:
    return raw.decode("ascii", "replace").rstrip(" \0")


@dataclass
class IpBin:
    hardware_id: str
    maker_id: str
    device_info: str
    area_symbols: str
    peripherals_raw: str
    product_number: str
    product_version: str
    release_date: str
    boot_filename: str
    company: str
    title: str
    crc_stored: int
    crc_computed: int

    @property
    def peripherals(self) -> int:
        try:
            return int(self.peripherals_raw.strip() or "0", 16)
        except ValueError:
            return 0

    @property
    def peripheral_names(self) -> list[str]:
        p = self.peripherals
        return [name for bit, name in PERIPHERAL_BITS if p & bit]

    @property
    def is_wince(self) -> bool:
        return bool(self.peripherals & 0x1)

    @property
    def regions(self) -> list[str]:
        m = {"J": "Japan", "U": "USA", "E": "Europe"}
        return [m[c] for c in self.area_symbols if c in m]

    @property
    def crc_ok(self) -> bool:
        return self.crc_stored == self.crc_computed

    @property
    def looks_valid(self) -> bool:
        return self.hardware_id.startswith("SEGA SEGAKATANA")


def parse_ipbin(data: bytes) -> IpBin:
    if len(data) < 0x100:
        raise ValueError("IP.BIN too short")
    device_info = _s(data[0x20:0x30])
    try:
        crc_stored = int(device_info[:4], 16)
    except ValueError:
        crc_stored = -1
    return IpBin(
        hardware_id=_s(data[0x00:0x10]),
        maker_id=_s(data[0x10:0x20]),
        device_info=device_info,
        area_symbols=_s(data[0x30:0x38]),
        peripherals_raw=_s(data[0x38:0x40]),
        product_number=_s(data[0x40:0x4A]),
        product_version=_s(data[0x4A:0x50]),
        release_date=_s(data[0x50:0x60]),
        boot_filename=_s(data[0x60:0x70]),
        company=_s(data[0x70:0x80]),
        title=_s(data[0x80:0x100]),
        crc_stored=crc_stored,
        crc_computed=crc16(data[0x40:0x50]),
    )


def read_ipbin(image) -> bytes:
    """IP.BIN occupies the first 16 sectors of the boot data track."""
    t = image.hd_track
    return image.read_sectors(t.lba, IPBIN_SIZE // 2048)
