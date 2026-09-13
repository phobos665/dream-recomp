"""Dreamcast disc image tooling (WP0.3).

Two container formats are supported through one interface:

* GDI: a text index plus raw track files (``gdi.py``).
* CHD: MAME's compressed container (``chd.py``), decoded with libchdr when a shared library is
  available, otherwise by shelling out to ``chdman extractcd`` and reading the result as a GDI.

Everything above the container (ISO9660, IP.BIN, descrambling, code scanning, the inspection
report) works on either.
"""

from .image import DiscImage, Track, open_image  # noqa: F401

__version__ = "0.1.0"
