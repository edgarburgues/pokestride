# PokeStride

**Turn your Nintendo 3DS into a PokéWalker.** PokeStride runs the original PokéWalker
firmware, so your 3DS becomes the little step-counter that came with Pokémon
HeartGold/SoulSilver — steps, watts, dowsing, battles and all.

It was built hand-in-hand with its sibling **[PkwBridge](https://github.com/edgarburgues/pkwbridge)**,
which moves Pokémon between your HeartGold/SoulSilver save and the walker. The two are
meant to be used together.

> It's a hobby project and it runs well, but a few things are still partial — see
> [What works](#what-works).

<p align="center"><img src="img/screenshot.png" width="320" alt="PokeStride running on a Nintendo 3DS"></p>

## Get it

1. Download `pokeStride.3dsx` from the [**Releases**](https://github.com/edgarburgues/pokestride/releases).
2. Copy it to `sdmc:/3ds/pokeStride/` on your SD card.
3. Put your own `pwflash.rom` and `pweep.rom` in that same folder.
4. Open it from the Homebrew Launcher.

`pwflash.rom` (the firmware) and `pweep.rom` (the save) are dumped from a real PokéWalker
— for example with [pokewalker-rom-dumper](https://github.com/francesco265/pokewalker-rom-dumper).
They belong to Nintendo and are **not** included here.

## Controls

| Button | Action |
|--------|--------|
| **A** / touch | the walker's ENTER button |
| **D-pad ◀ ▶** / touch | LEFT / RIGHT |
| **START** | quit (your progress is saved) |
| **Close the lid** | sleep + count steps; open it to add them to the game |

## What works

| | |
|---|---|
| ✅ | CPU, screen, buttons, and save (auto-saved to `pweep.rom`) |
| ✅ | IR: the physical link and exchanging commands with a real PokéWalker (CONNECT) |
| ✅ | Step counting while the 3DS lid is closed |
| ✅ | **Peer play against [pwlink](https://git.diogenes-homelab.com/akadmin/pwlink)** — the complete encounter, first try: identity, 11 EEPROM writes, 11 reads, play-data and completion, in about 1.5 seconds. See [Pairing with pwlink](#pairing-with-pwlink) |
| ⚠️ | **A direct encounter with a real PokéWalker is close, but not there yet.** Both halves of the link are already proven separately: pwlink completes encounters with a real PokéWalker, and pwlink completes them with PokeStride. Put PokeStride and a real walker face to face and they do see each other — traffic flows both ways and roles are negotiated — but the session tears down before finishing. So the remaining work is optimisation and noise, not anything unknown: the receive path still hands bytes to the firmware at a fixed cadence, erasing the inter-packet silence the protocol uses to delimit frames |
| ⚠️ | **Audio is off for now** — the sound code is incomplete; planned for a later update |
| ⚠️ | **Steps only count with the lid closed** — they come from the console's own step counter, not the walker's motion sensor, so the lid has to be shut. This is a limitation on our side, not how the real walker behaves |
| ⚠️ | A few internal self-checks (watchdog / ADC / some clock counters) are skipped — not needed to run the firmware |

## Pairing with pwlink

**[pwlink](https://git.diogenes-homelab.com/akadmin/pwlink)** is a PC-side implementation of
the PokéWalker's infrared protocol. It acts as a second walker, so you can put PokeStride
through a complete encounter without needing two devices — and, because it logs both ends,
it is how the IR path in PokeStride was debugged in the first place.

You need an IrDA SIR transceiver on a serial port. The setup used here is a Flipper Zero in
USB-UART bridge mode with an MY018 module on its GPIO header; anything that gives you
115200-baud SIR will do.

```bash
python pwlink.py encounter --port COM3 --image eeprom.bin
```

Put PokeStride on its CONNECT screen, point it at the transceiver, and the encounter runs:
identity, eleven EEPROM writes, eleven reads, the play-data exchange and completion. pwlink
then prints what the other side revealed — trainer identifier, steps, watts, and the walking
Pokémon's sprite and name drawn from the bitmaps it sent.

Two things matter for this to work, and both cost hours to find:

- **Unplug the console's charger.** A switching supply couples enough noise into the
  receiver's front end to swamp the link; it showed up as a continuous stream of
  alternating-bit bytes and stopped the handshake dead.
- **Don't beacon without pause.** Anything you transmit resets the peer's silence timer, so
  a continuous beacon stops it ever timing out and re-announcing. pwlink goes quiet for
  300 ms every 1.2 s for exactly this reason.

The protocol itself, the timing constraints behind both of those, and a reference
implementation are documented in pwlink's specification.

## Adding more games

PokeStride and PkwBridge were made together as a pair. Today they target
HeartGold/SoulSilver, but the design is meant to make adding other games — newer **and**
older — straightforward. The big remaining job for newer generations is recreating their
sprites; I'm not an artist, so that part is happily left to the community ❤️.

## Build it yourself

Needs devkitPro/devkitARM + libctru/citro2d/citro3d. With Docker (no local devkitPro):

```bash
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/project" devkitpro/devkitarm:latest \
    bash -c "cd /project && make"
```

Or `make` with a native devkitPro install. Output: `pokeStride.3dsx`.

## Learn more

The **[wiki](https://git.diogenes-homelab.com/akadmin/pokestride/wiki)** covers the infrared
path in depth — the three protocol constants that decide whether an implementation
interoperates, the five root causes that had to be fixed to reach a complete encounter, and
how to capture and read a byte-level trace from both ends of the link.

The bundled reference documents cover the rest: `pokewalker_hardware.md` for the device,
`pokewalker_rom_map.md` for the EEPROM layout, and `3ds_hardware.md` for how the console's
IR transceiver is reached.

## Credits & license

- App icon by **[Lalogo](https://lalogo.dev/)** ([github.com/la-lo-go](https://github.com/la-lo-go)).
- Based on the original emulator by [jpcerrone](https://github.com/jpcerrone/pokestride),
  though almost all of the code has since been rewritten and extended.
- PokéWalker reverse engineering by [Dmitry.gr](https://dmitry.gr/?r=05.Projects&proj=28.%20pokewalker);
  IR driver lineage: [libn3ds](https://github.com/profi200/libn3ds) →
  [pwalkerHax](https://github.com/francesco265/pwalkerHax) → pokewalker-rom-dumper.

Licensed under the **GNU GPLv3** — see [`LICENSE`](LICENSE). The PokéWalker ROM/EEPROM
dumps are Nintendo property and are not included.
