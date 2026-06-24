# Nintendo 3DS Hardware Reference

Source: 3DBrew wiki (https://www.3dbrew.org/wiki/Main_Page)
Focused on details relevant to PokeStride (homebrew framebuffer rendering, I2C/IR, input).

## CPU & Memory

- ARM11 MPCore (Old 3DS: 268 MHz, New 3DS: 804 MHz with `osSetSpeedupEnable(true)`)
- FCRAM: 128 MB at physical 0x20000000 (Old 3DS), +128 MB at 0x28000000 (New 3DS)
- VRAM: 6 MB at physical 0x18000000 (virtual 0x1F000000)
- IO registers base: physical 0x10000000

## Framebuffer (GPU External Registers)

### Layout

Framebuffers are **column-major** — data is stored left-to-right by column, not top-to-bottom by row. Each column is 240 pixels tall (top screen) or 320 pixels tall (bottom screen).

### Screen Resolutions

| Screen | Resolution | Notes |
|---|---|---|
| Top | 400×240 | 800×240 in wide mode (3D: 400×240 per eye) |
| Bottom | 320×240 | Touch screen |

### Pixel Format Register (0x1EF00470)

| Bits | Field | Values |
|---|---|---|
| 2–0 | Color format | 0=RGBA8, 1=RGB8, 2=RGB565, 3=RGB5A1, 4=RGBA4 |
| 5–4 | Interlacing | 0=None, 1=AA (doubling), 2=AB (interlace), 3=BA |
| 6 | Alt pixel mode | Platform-dependent |
| 9–8 | DMA size | 0=32B, 1=64B, 2=128B |

Color components are in reverse byte order (e.g. RGB8 is stored as B, G, R in memory — effectively BGR8).

### Framebuffer Address Registers

**Top screen (PDC0):**

| Register | Purpose |
|---|---|
| 0x1EF00468 | FB A first address (left eye) |
| 0x1EF0046C | FB A second address (left eye) |
| 0x1EF00494 | FB B first address (right eye) |
| 0x1EF00498 | FB B second address (right eye) |

**Bottom screen (PDC1):** Same layout, offset +0x100 (e.g. 0x1EF00568).

Addresses must be shifted right by 3 bits when writing to hardware.

### Double Buffering (0x1EF00478)

- Bit 0: Next framebuffer to display after VBlank
- Bit 4: Currently displaying framebuffer

### Stride Register (0x1EF00490)

Distance in bytes between start of two framebuffer rows. Bottom 3 bits ignored (must be multiple of 8). Negative values = vertical flip.

### Pixel Address Calculation (Top Screen, BGR8)

```c
// Column-major, y=0 at bottom of column
fb_offset = (column * 240 + (239 - row)) * bytes_per_pixel;
// For BGR8 (3 bytes/pixel):
fb[(x * 240 + (239 - y)) * 3 + 0] = B;
fb[(x * 240 + (239 - y)) * 3 + 1] = G;
fb[(x * 240 + (239 - y)) * 3 + 2] = R;
```

## I2C Buses

### Controller Base Addresses

| Bus | Data (offset +0x0) | CNT (+0x1) | CNTEX (+0x2) | SCL (+0x4) |
|---|---|---|---|---|
| I2C1 | 0x10161000 | 0x10161001 | 0x10161002 | 0x10161004 |
| I2C2 | 0x10144000 | 0x10144001 | 0x10144002 | 0x10144004 |
| I2C3 | 0x10148000 | 0x10148001 | 0x10148002 | 0x10148004 |

### I2C_CNT Register Bit Fields

| Bit | Name | Description |
|---|---|---|
| 0 | STOP | Generate stop condition |
| 1 | START | Generate start condition |
| 2 | ERROR | Error flag |
| 4 | ACK | ACK received (0=NACK/error, 1=OK) |
| 5 | DIR | Direction (0=send, 1=receive) |
| 6 | IRQ_EN | Interrupt enable |
| 7 | EN | Enable / busy flag |

### I2C_CNTEX Register

Bits 0–1: normally set to 2 (clock stretch enable = BIT(1)).

### I2C_SCL Register

Bits 8–12: normally set to 5.

### I2C Register Struct (as used in pokestride)

```c
typedef struct {
    vu8  data;   /* offset 0x0 */
    vu8  cnt;    /* offset 0x1 */
    vu16 cntex;  /* offset 0x2 */
    vu16 scl;    /* offset 0x4 */
} I2cBus;
```

Base for bus 3: `OS_MMIO_VADDR + 0x48000` (= 0x10148000 physical).

### Device Addresses (Bus 3 — relevant to pokestride)

| Device | Write Addr | Service | Notes |
|---|---|---|---|
| IR (SC16IS750) | 0x9A | i2c::IR | 7-bit addr 0x4D |
| Gyroscope | 0xA6 (or 0xD6 post-8.0) | i2c::HID | |
| NFC (New 3DS) | 0x54 | i2c::IR | v8.0.0-18+ |

## SC16IS750 — I2C-to-UART/IrDA Bridge

The 3DS uses an NXP SC16IS750 chip on I2C bus 3 for IR communication. This chip converts I2C transactions into UART/IrDA-SIR frames.

### Why Direct MMIO Instead of IR Services

Normal 3DS homebrew uses system services (`ir:u` or `ir:USER`) which go through the kernel's `i2c::IR` driver. PokeStride **bypasses all services** and writes directly to the I2C bus 3 MMIO registers at physical 0x10148000. The only known homebrew project that successfully communicates with a PokéWalker (pokewalker-rom-dumper) uses the same approach — its `ir.c` states that `ir:USER` has "too much IPC overhead" and is "unreliable for Pokéwalker communication".

**Why the services don't work for PokéWalker:**

1. **`ir:USER` adds its own protocol layer** — 2-byte header, varint payload size, XOR obfuscation, CRC-8 error byte. Designed for Circle Pad Pro, not raw IrDA. PokéWalker expects raw 115200 baud IrDA SIR with its own XOR 0xAA encoding — the framing is completely incompatible.

2. **`ir:u` behavior is undocumented** — 3DBrew documents the command headers but not whether it passes bytes raw or adds IrDA IrLAP framing (BOF/EOF, byte stuffing). Developers report data received never matches data sent when using the services.

3. **Blocking semantics** — libctru's `iruSendData`/`iruRecvData` call `WaitSendTransfer`/`WaitRecvTransfer` which block indefinitely. An emulator needs non-blocking RXLVL polling every ~512 CPU cycles while running the H8 emulation loop.

4. **Session exclusivity** — Only 1–2 sessions can be open to any IR service. On New 3DS, `ir:rst` (C-stick/ZL/ZR) and `ir:USER` are mutually exclusive — if the system claims `ir:rst`, homebrew can't open IR at all.

5. **No register-level control** — Services don't expose baud rate divisor, FIFO configuration, IrDA mode enable, or TXLVL/RXLVL polling.

6. **IPC overhead** — Each service call involves `svcSendSyncRequest` → kernel context switch → service process → I2C access → return. Prohibitive for PokéWalker's tight timing windows.

**The direct MMIO approach**: `i2c.c` casts the bus base address to a volatile struct pointer and bit-bangs I2C transactions (START, device address, register address, data, STOP) with busy-wait polling on the EN flag. `ir.c` then drives the SC16IS750 register-by-register.

This is possible because the Homebrew Launcher maps IO pages into process memory. Code lineage: profi200/libn3ds → francesco265/pwalkerHax → pokewalker-rom-dumper → pokestride.

### Old 3DS vs New 3DS: IR Must Be Explicitly Enabled

**Critical difference**: On Old 3DS, the OS boot sequence configures the SC16IS750 into IrDA SIR mode (MCR[6]=1). On **New 3DS family models, the chip may remain in plain UART mode** (MCR[6]=0) after boot — IrDA encoding is NOT active by default.

The fix is to unconditionally set MCR[6] at init time:
```c
I2C_write(REG_MCR, I2C_read(REG_MCR) | MCR_IRDA);
```

Additionally, the IR transceiver itself is enabled/disabled via the SC16IS750's own GPIO pin (IOSTATE register), not a separate 3DS GPIO:
- **Enable**: `IOSTATE = 0` (transceiver on) + `IER[4] = 0` (exit sleep)
- **Disable**: `IOSTATE[0] = 1` (transceiver off) + `IER[4] = 1` (enter sleep)

### Register Map (sub-address = bits 6:3 of I2C address byte)

| Sub-Addr | Name | R/W | Description |
|---|---|---|---|
| 0x00 | RHR/THR | R/W | Receive/Transmit Holding Register (FIFO access) |
| 0x00 | DLL | R/W | Divisor Latch Low (when LCR[7]=1, DLAB mode) |
| 0x08 | IER | R/W | Interrupt Enable Register |
| 0x08 | DLH | R/W | Divisor Latch High (when LCR[7]=1) |
| 0x10 | FCR | W | FIFO Control Register |
| 0x18 | LCR | R/W | Line Control Register |
| 0x20 | MCR | R/W | Modem Control Register |
| 0x28 | LSR | R | Line Status Register |
| 0x40 | TXLVL | R | TX FIFO Level (bytes free) |
| 0x48 | RXLVL | R | RX FIFO Level (bytes available) |
| 0x58 | IOSTATE | R/W | GPIO state register |
| 0x78 | EFCR | R/W | Extra Features Control Register |

### Key Bit Fields

**IER (0x08):**
- Bit 4: Sleep mode (1 = enter sleep, 0 = wake)

**FCR (0x10):**
- Bit 0: FIFO enable
- Bit 1: RX FIFO reset
- Bit 2: TX FIFO reset

**LCR (0x18):**
- Bits 1–0: Word length (0x03 = 8 bits)
- Bit 7: DLAB — divisor latch access bit

**MCR (0x20):**
- Bit 6: IrDA SIR mode enable (0 = UART, 1 = IrDA)

**LSR (0x28):**
- Bit 6: TEMT — TX empty (both THR and TSR empty)

**EFCR (0x78):**
- Bit 1: TX disable
- Bit 2: RX disable

### Baud Rate Configuration

Baud rate = 1,152,000 / divisor.

| Divisor | Baud Rate |
|---|---|
| 10 | 115,200 |
| 1 | 1,152,000 |

PokéWalker uses 115,200 baud (divisor = 10).

### FIFO

64-byte TX and RX FIFOs. TXLVL reports bytes free, RXLVL reports bytes available. Bulk I2C transfers can read/write multiple bytes to REG_FIFO in a single transaction.

### Init Sequence (as implemented in pokestride)

1. `I2C_init()` — configure I2C bus 3 registers
2. Disable TX+RX via EFCR, reset FIFOs via FCR, sleep 2ms
3. Wake chip (IER = 0, IOSTATE = 0)
4. Set DLAB, write DLL=10/DLH=0, clear DLAB → 115200 baud, 8N1
5. Read back divisor (mirrors Nintendo's ir module)
6. Write divisor again
7. Sleep chip, then wake for use
8. Set MCR bit 6 for IrDA SIR mode

### TX Sequence

1. Reset + enable TX FIFO: FCR = 0x05
2. Enable transmitter only: EFCR = 0x02
3. Loop: read TXLVL, write min(remaining, txlvl) bytes to FIFO
4. Wait for LSR TEMT bit (TX completely empty)
5. Disable TX+RX: EFCR = 0x06, FCR = 0x00

### RX Sequence

1. Start: FCR = 0x03 (reset + enable RX FIFO), EFCR = 0x04 (enable RX only)
2. Poll: read RXLVL, if > 0 bulk-read that many bytes from FIFO
3. Stop: EFCR = 0x06, FCR = 0x00

## IR Services (System Software)

The 3DS OS provides IR through services, but pokestride bypasses these with direct I2C MMIO:

| Service | Purpose | Notes |
|---|---|---|
| ir:u | Raw IrDA-SIR send/receive | Low-level byte I/O |
| ir:USER | Higher-level IR protocol | Used by Circle Pad Pro; has encryption |
| ir:rst | New 3DS HID (C-stick, ZL/ZR) | Added in 8.0.0-18 |

All services use `gpio::IR` and `i2c::IR` internally. Only 1–2 sessions can be open at a time. PokeStride uses direct MMIO instead of these services for lower latency.

## HID (Input)

### HID_PAD Register

- Address: 0x10146000 (2 bytes)
- **Inverted logic**: bit = 1 means key is NOT pressed

| Bit | Button |
|---|---|
| 0 | A |
| 1 | B |
| 2 | Select |
| 3 | Start |
| 4 | D-Right |
| 5 | D-Left |
| 6 | D-Up |
| 7 | D-Down |
| 8 | R |
| 9 | L |
| 10 | X |
| 11 | Y |

### HID Shared Memory (via libctru)

Total size: 0x2B0 bytes.

| Offset | Size | Section |
|---|---|---|
| 0x00 | 0xA8 | Button + circle pad state |
| 0xA8 | 0x60 | Touch screen |
| 0x108 | 0x50 | Accelerometer |
| 0x158 | 0xE0 | Gyroscope |

**Button state** uses an 8-entry rotating buffer at offset 0x28, indexed by value at 0x10. Each entry (0x10 bytes):
- +0x0: current PAD state
- +0x4: newly pressed
- +0x8: released
- +0xC: circle pad (s16 X | s16 Y), range ±0x9C

**Touch** uses 8-entry buffer at offset 0x20 from touch base. Each entry (8 bytes): X (u16), Y (u16), valid flag (u32).

Homebrew typically uses libctru's `hidScanInput()` / `hidKeysDown()` / `hidKeysHeld()` which wraps this shared memory.

## SPI Buses

| Bus | Base (legacy) | Base (NSPI) |
|---|---|---|
| SPI1 | 0x10142000 | 0x10142800 |
| SPI2 | 0x10143000 | 0x10143800 |
| SPI3 | 0x10160000 | 0x10160800 |

Not directly used by pokestride (IR goes through I2C, not SPI).

## 3DSX Format

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | Magic "3DSX" |
| 0x04 | 2 | Header size |
| 0x06 | 2 | Relocation header size |
| 0x08 | 4 | Format version |
| 0x0C | 4 | Flags |
| 0x10 | 4 | Code segment size |
| 0x14 | 4 | Rodata segment size |
| 0x18 | 4 | Data size (incl BSS) |
| 0x1C | 4 | BSS size |

Extended header (if header > 32 bytes): SMDH offset (4B), SMDH size (4B), RomFS offset (4B).

Loaded by Homebrew Launcher (hbmenu) into process memory. Homebrew accesses hardware through libctru service wrappers or direct MMIO (with appropriate service handles).

## Key libctru Functions Used by PokeStride

| Function | Purpose |
|---|---|
| `gfxInit()` / `gfxExit()` | Init/deinit framebuffer |
| `gfxGetFramebuffer()` | Get pointer to current FB + dimensions |
| `gfxFlushBuffers()` | Flush cache for FB |
| `gfxSwapBuffers()` | Swap double buffer |
| `gspWaitForVBlank()` | Wait for vertical blank |
| `hidScanInput()` | Update input state |
| `hidKeysDown()` / `hidKeysHeld()` | Read button state |
| `aptMainLoop()` | Check if app should keep running |
| `osSetSpeedupEnable(true)` | Enable New 3DS 804 MHz mode |
| `svcSleepThread(ns)` | Sleep for nanoseconds |
