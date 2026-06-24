# PokéWalker Hardware Reference

Source: Dmitry.gr reverse engineering (https://dmitry.gr/?r=05.Projects&proj=28.%20pokewalker)

## CPU — Renesas H8/38606

- H8/300 advanced derivative, big-endian
- System clock: 3.6864 MHz, sub-clock: 32.768 KHz
- 8 × 32-bit registers (ER0–ER7, ER7 = SP), accessible as 16-bit (eX/rX) or 8-bit (rXh/rXl)
- 24-bit address bus but runs in normal mode (64 KB address space, top 8 bits ignored)

### Memory Map

| Range | Size | Contents |
|---|---|---|
| 0x0000–0xBFFF | 49 KB | ROM |
| 0xF020–0xF0FF | 128 B | MMIO |
| 0xF780–0xFF7F | 2 KB | RAM |
| 0xFF80–0xFFFF | 128 B | MMIO |

Writable RAM range (safe for code upload): 0xF956–0xFF40.

### Key Interrupt Vectors

| Vector | Address | Purpose |
|---|---|---|
| Timer B1 | 0x06FA | Timer B1 overflow |
| Timer W | 0x3A4A | Timer W compare match |
| IRQ0 | 0xA300 | External interrupt 0 |
| RTC quarter-sec | 0xA65E | RTC every 0.25 s |
| RTC half-sec | 0xA674 | RTC every 0.5 s |
| RTC every-sec | 0xA682 | RTC every 1.0 s |
| SCI3 | 0x075A | SCI3 interrupt (just RTE, unused) |

### Key ROM Function Addresses

| Address | Function |
|---|---|
| 0x0714 | calcPacketCrc |
| 0x0772 | Packet TX (r0l=payload len, r0h=cmd, r1l=extra; payload at 0xF8D6) |
| 0x08D6 | Comms app main loop pointer |
| 0x1D22 | Set event participation bit |
| 0x1D7A | Check event participation bit |
| 0x259E | Watchdog pet |
| 0x5990 | Factory test entry |

### Event Loop

Global pointer at RAM address 0xF7E0 controls the main-loop dispatch.

## GPIO / Port Pin Assignments

| Port.Pin | Function |
|---|---|
| Port 1, Pin 0 | LCD chip select (active low) |
| Port 1, Pin 1 | LCD data/command select |
| Port 1, Pin 2 | EEPROM chip select (active low) |
| Port 9, Pin 0 | Accelerometer chip select (active low) |
| Port B, Pin 0 | ENTER button |
| Port B, Pin 2 | LEFT button |
| Port B, Pin 4 | RIGHT button |

Port addresses: PORT1 = 0xFFD4, PORT9 = 0xFFDC.

## LCD — SSD1854

- 96×64 pixels, 2-bit grayscale (4 levels)
- SPI interface, directly connected to SSU
- **Data format**: Stripe-based, 8-pixel-tall horizontal bands. Each column = 2 bytes (two bitplanes, LSB = top pixel). Scanned left-to-right, top-to-bottom.
- LCD init commands stored in EEPROM at 0x00AC (copy A) / 0x01AC (copy B), up to 63 bytes. 0xFE = end of stream, 0xFD = delay (next byte = length).
- If byte 0 of LcdConfigCmds is 0, ROM defaults at 0xBEB8 are used instead.

## EEPROM — ST M95512

- 64 KB capacity, SPI interface
- 128-byte page size, 16-bit addresses
- Standard SPI EEPROM commands (read, write, write-enable, etc.)

## Accelerometer — Bosch BMA150

- SPI interface on Port 9 Pin 0

## IR Communication

### Physical Layer

- SIR-compatible transceiver, 115200 baud, 8N1
- All bytes XOR'd with 0xAA before transmission

### Packet Format

8-byte header + 0–128 bytes payload:

```
Byte 0:    Command (0x00–0xFE)
Byte 1:    "Extra" data byte
Bytes 2–3: CRC-16 checksum
Bytes 4–7: Session ID (32-bit)
Bytes 8+:  Payload (0–128 bytes)
```

### CRC Algorithm

1. Sum all even-indexed bytes → `evenSum`
2. Sum all odd-indexed bytes → `oddSum`
3. `combined = (evenSum << 8) + oddSum`  (i.e., evenSum × 256 + oddSum)
4. Fold: while top 16 bits nonzero, add top 16 to bottom 16
5. Result is the 16-bit CRC
6. To verify: set checksum field to zero, recalculate, compare

ROM function: 0x0714 (`calcPacketCrc`).

### Connection Handshake

1. **Slave** advertises: sends raw 0xFC byte every few hundred milliseconds
2. **Master** sends CMD 0xFA with random 32-bit session ID
3. **Slave** replies CMD 0xF8 with its own random 32-bit session ID
4. **Agreed session ID** = XOR of both values
5. All subsequent packets must carry the agreed session ID; mismatched packets are ignored

### Compression (Commands 0x00, 0x80)

LZ-variant compression for EEPROM writes:

- 4-byte header: type 0x10, decompressed length, 2 unused bytes
- Up to 124 bytes compressed payload → always decompresses to exactly 128 bytes
- Each packet: 1-byte header (parsed high-to-low bit), followed by 8 chunks
- Bit = 1 → 2-byte backreference: top nibble of first byte = copy length − 3; bottom nibble + next byte = 12-bit distance − 1
- Bit = 0 → literal byte
- Max backreference distance: 256 bytes; max copy length: 18 bytes

### IR Command Set

**Connection control:**

| Cmd | Dir | Description |
|---|---|---|
| 0xFC | W→? | Advertisement (slave beacon) |
| 0xFA | G→W, W→W | Connection initiation (master) |
| 0xF8 | W→G, W→W | Connection reply (slave) |

**EEPROM operations:**

| Cmd | Dir | Description |
|---|---|---|
| 0x00, 0x80 | G→W | Compressed EEPROM write (128 bytes on boundary) |
| 0x02, 0x82 | W↔W, G→W | Direct EEPROM write (128 bytes) |
| 0x04 | W↔W, W→G | ACK for EEPROM write |
| 0x06 | ?→W | **Direct RAM/MMIO write (arbitrary address!)** |
| 0x0C | W↔W, G→W | EEPROM read request (16-bit addr, 8-bit len) |
| 0x0E | W↔W, W→G | EEPROM read reply |

**Walk management:**

| Cmd | Dir | Description |
|---|---|---|
| 0x20 | G→W | Request identity data; reply is CMD 0x22 |
| 0x2A | G↔W | Reset walker with unique ID data (0x28 bytes) |
| 0x38 | ?→W | Start walk (preserve special event status) |
| 0x4E | G→W | End walk; reply is CMD 0x50 |
| 0x5A | G→W | Start walk with UI; reply is CMD 0x5A |

**Peer play (walker↔walker):**

| Cmd | Dir | Description |
|---|---|---|
| 0x10 | W→W | Peer identity (master→slave, 0x68 bytes) |
| 0x12 | W→W | Peer identity (slave→master, 0x68 bytes) |
| 0x14 | W↔W | Peer play data (0x34 bytes, bidirectional) |
| 0x16 | W↔W | Peer play completion (UI + gift award) |

**Events:**

| Cmd | Dir | Description |
|---|---|---|
| 0xA0–0xAE | ?↔W | Event participation query/bitmap (0x11 bytes) |
| 0xB8–0xBE | ?→W | Award stamp (heart/spade/diamond/club) |
| 0xC0 | ?↔W | Award special map |
| 0xC2 | ?↔W | Award event Pokémon |
| 0xC4 | ?↔W | Award event item |
| 0xC6 | ?↔W | Assign special route |

Directions: G = game (NDS), W = walker, ? = either.

## Factory Test

- Triggered at boot if SPI response to 0xB2 is 0xAA
- Build string at ROM 0x0050 (12 bytes, e.g. "Jun 26 2009"), version at 0x005C–0x005D
- Test stages: 0x04 (EEPROM) → 0x03 → 0x02 (accelerometer) → 0x01 (ADC/battery) → 0x00 (pass)
- Failure codes: 0xF4 (EEPROM), 0xF3 (RTC), 0xF1 (battery)

## Boot Sequence

1. Check 0xF020 for CopyMarker 0xA5 → if set, resume interrupted walk initialization copy
2. Check EEPROM 0x0000–0x0007 for "nintendo" magic → determines initialization state
3. SPI probe for factory test mode (0xB2 → 0xAA response)
4. Normal boot into main event loop

## EEPROM Layout

### Reliable Dual-Copy Data (checksummed, fault-tolerant)

Each section stored at two addresses with independent checksums. On read, both copies are validated; if only one passes checksum, that copy is used.

| Data | Copy A | Copy B | Size |
|---|---|---|---|
| ADC calibration | 0x0080–0x0082 | 0x0180–0x0182 | 3 B |
| UniqueIdentityData | 0x0083–0x00AB | 0x0183–0x01AB | 0x29 B |
| LcdConfigCmds | 0x00AC–0x00EC | 0x01AC–0x01EC | 0x41 B |
| IdentityData | 0x00ED–0x0155 | 0x01ED–0x0255 | 0x69 B |
| HealthData | 0x0156–0x016E | 0x0256–0x026E | 0x19 B |
| CopyMarker | 0x016F | 0x026F | 1 B |

Checksum: sum of all bytes in the section.

CopyMarker values: 0xA5 = walk init in progress, 0x00 = complete.

### Graphics & UI Assets (0x0280–0x7FAF)

Pre-rendered images stored in EEPROM. Format: 2-bit grayscale packed.

| Address | Size | Description |
|---|---|---|
| 0x0280–0x041F | 0x1A0 | Numeric chars "0123456789:-/" (8×16 ea) |
| 0x0420–0x045F | 0x40 | WATT symbol (16×16) |
| 0x0460–0x046F | 0x10 | Pokéball icon (8×8) |
| 0x0470–0x047F | 0x10 | Pokéball light grey (event icon) |
| 0x0488–0x0497 | 0x10 | Item symbol (8×8) |
| 0x0498–0x04A7 | 0x10 | Item symbol light grey |
| 0x04A8–0x04B7 | 0x10 | Tiny map icon (8×8) |
| 0x04B8–0x04F7 | 0x40 | Card faces ♥♠♦♣ (8×8 ea) |
| 0x04F8–0x05B7 | 0xC0 | Arrow icons (8×8, multiple configs) |
| 0x05B8–0x0617 | 0x60 | Menu arrows L/R + return (8×16 ea) |
| 0x0650–0x065F | 0x10 | Medicine vial (8×8) |
| 0x0660–0x066F | 0x10 | Low battery icon (8×8) |
| 0x0670–0x08AF | 0x240 | Talk bubbles with mood icons (24×16 × 6) |
| 0x08B0–0x090F | 0x60 | Talk bubble exclamation (24×16) |
| 0x0910–0x0F4F | 0x640 | Menu headings: POKÉRADAR, DOWSING, CONNECT, TRAINER CARD, POKÉMON & ITEMS (80×16 ea) |
| 0x0F50–0x108F | 0x140 | SETTINGS heading (80×16) |
| 0x1090–0x120F | 0x180 | Menu icons × 6 + person icon (16×16 ea) |
| 0x1250–0x1389 | 0x140 | Trainer name rendered (80×16) |
| 0x138A–0x13C9 | 0x40 | Small route image (16×16) |
| 0x13CA–0x16C9 | 0x300 | Settings frames: steps, time, days, total days, sound, shade |
| 0x16CA–0x17EF | 0x126 | Speaker icons × 3 (24×16 ea) |
| 0x17F0–0x180F | 0x20 | Contrast demo (8×16) |
| 0x1810–0x19BF | 0x1B0 | Large icons: treasure chest, map scroll, present (32×24 ea) |
| 0x19C0–0x1ABF | 0x100 | Bush icons dark/light (16×16 ea) + blank (16×24) |
| 0x1AC0–0x1B5F | 0xA0 | Bush dark large (32×24) |
| 0x1B60–0x1C5F | 0x100 | Word bubbles !, !!, !!! + radiating lines (16×16 ea) |
| 0x1C60–0x1D5F | 0x100 | 7-point stars small + large (16×32 ea) |
| 0x1D60–0x1DF0 | 0x90 | Cloud "pokémon appeared" (32×24) |
| 0x1DF1–0x1E10 | 0x20 | HP bar segment + catch star (8×8 ea) |
| 0x1E11–0x2010 | 0x200 | Attack/evade/catch placard (96×32) |

Text image strings (96×16 or 80×16) span 0x20C0–0x7D3F — all pre-rendered game text.

### Route & Pokémon Data (0x7FB0–0xB7FF)

| Address | Size | Description |
|---|---|---|
| 0x7FB0–0x806F | 0xC0 | Current RouteInfo |
| 0x8070–0x80AF | 0x40 | Current area graphic (32×24) |
| 0x80B0–0x81EF | 0x140 | Current area text name (80×16) |
| 0x81F0–0x836F | 0x180 | Current pokémon animated sprite (32×24 × 2 frames) |
| 0x8370–0x856F | 0x200 | Current pokémon large sprite (64×48 × 2 frames) |
| 0x8570–0x86AF | 0x140 | Current pokémon name image (80×16) |
| 0x86B0–0x93AF | 0xD00 | Route available pokémon sprites (32×24 × 2 × 3) |
| 0x93B0–0xA3AF | 0x1000 | 3rd route pokémon large sprite (64×48 × 2 frames) |
| 0xA3B0–0xA6CF | 0x320 | Available pokémon name images (80×16 × 3) |
| 0xA6D0–0xB4CF | 0xE00 | Item name images (96×16 × 10) |
| 0x8F00–0xB7FF | 0x2900 | Active route data (staged from 0xD700 at walk start) |

### Special Items & Events (0xB500–0xC0BF)

| Address | Size | Description |
|---|---|---|
| 0xB500 | 1 | Special items bitfield (bit0=♥, bit1=♠, bit2=♦, bit3=♣, bit4=map, bit5=event poke, bit6=event item, bit7=special route) |
| 0xB504–0xB683 | 0x180 | Special map data |
| 0xB684–0xB693 | 0x10 | Event pokémon summary |
| 0xB694–0xB6BF | 0x2C | Event pokémon extra data |
| 0xB6C0–0xB83F | 0x180 | Event pokémon sprite (32×24 × 2) |
| 0xB840–0xB97F | 0x140 | Event pokémon name (80×16) |
| 0xB980–0xB987 | 8 | Event item (6 zero bytes + u16 LE) |
| 0xB988–0xBAF7 | 0x170 | Event item name (96×16) |
| 0xBB30–0xC0BB | 0x58C | Special route data (SpecialRoute struct) |

### Team & Peer Data (0xC100–0xE8F7)

| Address | Size | Description |
|---|---|---|
| 0xC100–0xC323 | 0x224 | Active TeamData |
| 0xC400–0xC623 | 0x224 | Staged TeamData (pre-walk) |
| 0xD800–0xDA23 | 0x224 | Current peer TeamData |
| 0xDA24–0xE5AB | 0xB88 | Historical peers (10 × TeamData) |
| 0xE600–0xE77F | 0x180 | Current peer pokémon sprite |
| 0xE780–0xE8BF | 0x140 | Current peer pokémon name |
| 0xE8C0–0xE8F7 | 0x38 | PeerPlayData |

## Data Structures

### PokemonSummary (6 bytes, LE)

| Offset | Size | Field |
|---|---|---|
| 0x00 | 2 | species |
| 0x02 | 2 | heldItem |
| 0x04 | 2 | moves (start of 4 × u16, 8 bytes total — struct is actually larger in some contexts) |
| 0x0C | 1 | level |
| 0x0D | 1 | variantAndFlags (bits 0–4 = variant, bit 5 = female) |
| 0x0E | 1 | moreFlags (0x02 = shiny, 0x01 = has form) |

### IdentityData (0x69 bytes, mixed endianness)

| Offset | Size | Field | Endian | Notes |
|---|---|---|---|---|
| 0x00 | 4 | unk_0 | LE | usually 1 at walk start |
| 0x04 | 4 | unk_1 | LE | zeroed at walk end |
| 0x08 | 2 | unk_2 | LE | usually 7 |
| 0x0A | 2 | unk_3 | LE | zeroed at walk end |
| 0x0C | 2 | trainerTID | LE | |
| 0x0E | 2 | trainerSID | LE | |
| 0x10 | 0x28 | uniq | — | UniqueIdentityData |
| 0x38 | 0x10 | evtBmp | — | Event bitmap (128 bits, 1 bit per event ID 1–127) |
| 0x48 | 16 | trainerName | LE | 8 × u16 wide chars |
| 0x58–0x5A | 3 | unk_4/5/6 | — | |
| 0x5B | 1 | flags | — | 0x01=paired, 0x02=has poke, 0x04=poke on walk |
| 0x5C | 1 | protoVer | — | always 0x02 |
| 0x5E | 1 | protoSubver | — | always 0x00 |
| 0x5F | 1 | unk_8 | — | 0x02 at walk start |
| 0x60 | 4 | lastSyncTime | **BE** | seconds since 2001 |
| 0x64 | 4 | stepCount | **BE** | |

### HealthData (0x19 bytes, mostly BE)

| Offset | Size | Field | Endian |
|---|---|---|---|
| 0x00 | 4 | lifetimeTotalSteps | BE |
| 0x04 | 4 | todaySteps | BE |
| 0x08 | 4 | lastSyncTime | BE |
| 0x0C | 2 | totalDays | BE |
| 0x0E | 2 | curWatts | BE |
| 0x17 | 1 | settings | — |

Settings byte: bit 0 = special route, bits 1–2 = volume, bits 3–6 = contrast.

### TeamPokeData (0x8C bytes, LE)

| Offset | Size | Field |
|---|---|---|
| 0x00 | 2 | species |
| 0x02 | 2 | itemHeld |
| 0x04 | 8 | moves[4] (u16 × 4) |
| 0x0C | 2 | otTid |
| 0x0E | 2 | otSid |
| 0x10 | 4 | pid (personality value) |
| 0x14 | 4 | IVs (packed: 5 bits each for HP/Atk/Def/Spd/SpA/SpD) |
| 0x18 | 6 | EVs (1 byte each) |
| 0x1E | 1 | variant |
| 0x1F | 1 | sourceGame |
| 0x20 | 1 | ability |
| 0x21 | 1 | happiness |
| 0x22 | 1 | level |
| 0x24 | 20 | nickname (10 × u16 wide chars) |

### TeamData (0x224+ bytes, LE)

| Offset | Size | Field |
|---|---|---|
| 0x08 | 0x28 | UniqueIdentityData |
| 0x30 | 2 | tid |
| 0x32 | 2 | sid |
| 0x38 | 16 | name (8 × u16) |
| 0x60 | 0x348 | pokes[6] (6 × TeamPokeData, 0x8C ea) |

### PeerPlayData (0x34 bytes, LE)

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | curStepCount |
| 0x04 | 2 | curWatts |
| 0x0E | 2 | species |
| 0x10 | 22 | pokeNickname (11 × u16) |
| 0x1C | 16 | trainerName (8 × u16) |
| 0x2C | 1 | pokeGenderForm |
| 0x2D | 1 | pokeIsSpecial |

### RouteInfo (0xFE bytes, LE)

| Offset | Size | Field |
|---|---|---|
| 0x00 | 6 | walking pokémon summary |
| 0x06 | 22 | nickname (11 × u16) |
| 0x1C | 1 | friendship |
| 0x1D | 1 | routeImageIdx |
| 0x1E | 42 | routeName (21 × u16) |
| 0x48 | 18 | routePokes[3] (3 × PokemonSummary) |
| 0x5A | 6 | routePokeMinSteps[3] (u16 × 3) |
| 0x60 | 3 | routePokeChance[3] (percent) |
| 0x64 | 20 | routeItems[10] (u16 × 10) |
| 0x78 | 20 | routeItemMinSteps[10] (u16 × 10) |
| 0x8C | 10 | routeItemChance[10] (percent) |

### RouteImageIdx Enum

| Value | Scene |
|---|---|
| 0 | Field & Trees |
| 1 | Forest & Trees |
| 2 | Suburbs |
| 3 | Urban |
| 4 | Hill & Volcano |
| 5 | Dim Cave |
| 6 | Lake |
| 7 | Beach & Waves |

## NDS Side (Overlay 9_112)

| Address | Function |
|---|---|
| 0x20DDFE0 | irDoTx(u8* data, u32 len) |
| 0x20DDE94 | irRx(u8* data) → length |
| 0x21E5900 | Packet CRC / overlay base |
| 0x21E59B4 | Full send function |
| 0x21E5A68 | Basic send function |
| 0x21E5B98 | Receive state machine |
| 0x21F4138 | Route data |

## Event System

- Event indices 1–127 (0 = no-op)
- 16-byte bitmap in IdentityData at offset 0x38
- One bit per event, bit = 1 means already participated
- Query via IR commands 0xA0–0xAE (0x11 bytes payload)
- Award stamps (♥♠♦♣) via commands 0xB8–0xBE
- Award special content via 0xC0 (map), 0xC2 (pokémon), 0xC4 (item), 0xC6 (route)
