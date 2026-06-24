#!/usr/bin/env python3
"""
Swap PokéWalker EEPROM language by replacing UI text images.

The PokéWalker ROM is identical across regions. All language-specific
content is pre-rendered text images stored in the EEPROM.

Usage:
    python swap_language.py <target_eeprom> <donor_eeprom> <output>

Example — make a Japanese EEPROM show Spanish text:
    python swap_language.py eeprom.j.bin pweep.es.rom eeprom.j.spanish.bin

The script copies the UI text image region (0x0280-0x7FB0) from the
donor EEPROM into the target, preserving all save data (steps, watts,
pokemon, route, team, identity, settings).
"""
import sys
import struct

# EEPROM regions containing language-specific text images
# These are pre-rendered bitmap strings (96x16 or 80x16 pixels)
TEXT_REGIONS = [
    # (start, end_exclusive, description)
    (0x0280, 0x041F + 1, "Numeric characters 0-9 : - /"),
    (0x0420, 0x04F7 + 1, "Symbols (watt, pokeball, item, cards, arrows)"),
    (0x04F8, 0x0617 + 1, "Arrow icons + menu arrows + return symbol"),
    (0x0630, 0x066F + 1, "Message indicators + icons"),
    (0x0670, 0x090F + 1, "Talk bubbles + feeling icons"),
    (0x0910, 0x108F + 1, "Menu headings (POKERADAR, DOWSING, CONNECT, etc)"),
    (0x1090, 0x124F + 1, "Menu icons + person icon"),
    # 0x1250-0x1389: trainer name (save-specific, skip)
    (0x13CA, 0x16C9 + 1, "Settings frames (steps, time, days, sound, shade)"),
    (0x16CA, 0x180F + 1, "Speaker icons + contrast demo"),
    (0x1810, 0x1DF0 + 1, "Game icons (chest, scroll, present, bushes, stars, cloud)"),
    (0x1DF1, 0x2010 + 1, "Battle UI (HP bar, star, attack/evade/catch placard)"),
    (0x2011, 0x206F + 1, "Misc icons (blank, IR, music note)"),
    (0x20C0, 0x7D3F + 1, "All text strings (Connecting, Cannot connect, etc)"),
    (0x7D70, 0x7FAF + 1, "Random checksum data"),
]

def copy_text_regions(target: bytearray, donor: bytes) -> int:
    """Copy all text regions from donor to target. Returns bytes copied."""
    total = 0
    for start, end, desc in TEXT_REGIONS:
        size = end - start
        target[start:end] = donor[start:end]
        total += size
    return total

def verify_eeprom(data: bytes, name: str) -> bool:
    """Basic EEPROM validation."""
    if len(data) != 65536:
        print(f"ERROR: {name} is {len(data)} bytes, expected 65536")
        return False
    # Check for 'nintendo' magic at 0x0000 (or all-FF if uninitialized)
    magic = data[0:8]
    if magic != b'nintendo' and magic != b'\xff' * 8:
        print(f"WARNING: {name} has unexpected magic: {magic!r}")
    return True

def verify_checksum(data: bytes, base: int, size: int) -> bool:
    """Verify reliable data checksum (initial value = 1)."""
    calc = (1 + sum(data[base:base + size])) & 0xFF
    stored = data[base + size]
    return calc == stored

def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)

    target_path = sys.argv[1]
    donor_path = sys.argv[2]
    output_path = sys.argv[3]

    with open(target_path, 'rb') as f:
        target = bytearray(f.read())
    with open(donor_path, 'rb') as f:
        donor = f.read()

    if not verify_eeprom(target, target_path):
        sys.exit(1)
    if not verify_eeprom(donor, donor_path):
        sys.exit(1)

    # Show current state
    for name, data in [("Target", target), ("Donor", donor)]:
        health_base = 0x0156
        lifetime = struct.unpack('>I', data[health_base:health_base+4])[0]
        watts = struct.unpack('>H', data[health_base+0x0E:health_base+0x10])[0]
        ck_ok = verify_checksum(data, health_base, 0x18)
        print(f"{name}: lifetime_steps={lifetime}, watts={watts}, health_cksum={'OK' if ck_ok else 'BAD'}")

    # Copy text regions
    copied = copy_text_regions(target, donor)
    print(f"\nCopied {copied} bytes of UI text from {donor_path}")

    # Verify health data wasn't touched
    for copy_name, base in [("A", 0x0156), ("B", 0x0256)]:
        ok = verify_checksum(target, base, 0x18)
        if not ok:
            print(f"WARNING: HealthData copy {copy_name} checksum is BAD after swap")

    # Write output
    with open(output_path, 'wb') as f:
        f.write(target)
    print(f"Written to {output_path}")

if __name__ == '__main__':
    main()
