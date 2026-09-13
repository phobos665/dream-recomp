import os
import random

from dcdisc.scramble import descramble, ensure_plain, is_scrambled, scramble, MAXCHUNK


def test_roundtrip_various_sizes():
    rnd = random.Random(3)
    for size in [0, 1, 31, 32, 33, 64, 1000, 4096, 65536 + 17, 3 * 1024 * 1024 + 5]:
        data = rnd.randbytes(size)
        assert descramble(scramble(data)) == data, size
        assert scramble(descramble(data)) == data, size


def test_scramble_is_a_permutation_of_slices():
    data = bytes(range(256)) * 16  # 4 KB
    s = scramble(data)
    assert s != data
    assert sorted(s[i:i + 32] for i in range(0, len(s), 32)) == \
        sorted(data[i:i + 32] for i in range(0, len(data), 32))


def test_size_dependent_seed():
    a = scramble(bytes(2048))
    b = scramble(bytes(2080))
    assert a == bytes(2048)  # all-zero input is invariant, sanity
    assert len(b) == 2080


def test_large_multi_chunk():
    rnd = random.Random(9)
    data = rnd.randbytes(2 * MAXCHUNK + 12345)
    assert descramble(scramble(data)) == data


def test_is_scrambled_detection(synthetic_files):
    files, boot_plain = synthetic_files
    assert not is_scrambled(boot_plain)
    assert is_scrambled(files["1ST_READ.BIN"])
    assert ensure_plain(files["1ST_READ.BIN"]) == (boot_plain, True)
    assert ensure_plain(boot_plain) == (boot_plain, False)
