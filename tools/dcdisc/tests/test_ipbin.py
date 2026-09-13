from dcdisc.ipbin import parse_ipbin, crc16
from conftest import make_ipbin


def test_parse_fields():
    ip = parse_ipbin(make_ipbin(title="HELLO WORLD"))
    assert ip.looks_valid
    assert ip.title == "HELLO WORLD"
    assert ip.boot_filename == "1ST_READ.BIN"
    assert ip.regions == ["Japan", "USA", "Europe"]
    assert ip.crc_ok
    assert not ip.is_wince
    assert "Memory card (VMU)" in ip.peripheral_names
    assert "Start, A, B, D-pad" in ip.peripheral_names


def test_wince_flag():
    ip = parse_ipbin(make_ipbin(wince=True))
    assert ip.is_wince
    assert "Windows CE" in ip.peripheral_names


def test_crc16_known_vector():
    # CRC-16/CCITT-FALSE of "123456789" is 0x29B1
    assert crc16(b"123456789") == 0x29B1
