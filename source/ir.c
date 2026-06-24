/*
 * IR layer — direct SC16IS750 register access via I2C bus 3. Acts as a
 * non-blocking bridge between the emulated PokeWalker SCI3 and the real 3DS
 * IR hardware.
 *
 * The ROM already contains the complete IR protocol code (packet
 * construction, CRC, session management, XOR encoding); this layer only
 * shuttles raw bytes between the emulated SCI3 registers and the physical
 * SC16IS750 chip.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <3ds.h>

#include "i2c.h"
#include "ir.h"
#include "log.h"
#include "irtrace.h"

/* SC16IS750 register sub-addresses (bits 6:3 of I2C address byte) */
#define REG_FIFO    0x00u  /* RHR (read) / THR (write)                   */
#define REG_DLL     0x00u  /* Divisor Latch Low  — when LCR[7]=1 (DLAB)  */
#define REG_IER     0x08u  /* Interrupt Enable Register                   */
#define REG_DLH     0x08u  /* Divisor Latch High — when LCR[7]=1 (DLAB)  */
#define REG_FCR     0x10u  /* FIFO Control Register (write)               */
#define REG_LCR     0x18u  /* Line Control Register                       */
#define REG_MCR     0x20u  /* Modem Control Register                      */
#define REG_LSR     0x28u  /* Line Status Register                        */
#define REG_TXLVL   0x40u  /* TX FIFO Level                               */
#define REG_RXLVL   0x48u  /* RX FIFO Level                               */
#define REG_IODIR   0x50u  /* IODir: 1 bit per pin (1=output, 0=input)    */
#define REG_IOSTATE 0x58u  /* IOState (GPIO)                              */
#define REG_IOCONTROL 0x70u /* IOControl: bit 3 = software reset          */
#define REG_EFCR    0x78u  /* Extra Features Control Register             */

/* MCR bits */
#define MCR_IRDA    BIT(6) /* IrDA SIR mode enable (0=UART, 1=IrDA)      */

/* LSR bits */
#define LSR_TEMT    BIT(6) /* TX empty: both THR and TSR are empty        */

/* Blocked-ticks accumulator — read and reset by ir_get_blocked_ticks() */
static u64 irBlockedTicks = 0;

/* Deferred event counters — logged from the main loop, not from the hot path.
 * fprintf inside ir_send/recv (deep call stack) can overflow the 32KB stack. */
static u32 irTxCount = 0;
static u32 irTxBytes = 0;
static u32 irTxLastUs = 0;
static u32 irRxCount = 0;
static u32 irRxBytes = 0;

/* First 8 bytes of last TX/RX for protocol debugging (logged per-frame) */
static uint8_t irTxHead[8];
static u32     irTxHeadLen = 0;
static uint8_t irRxHead[8];
static u32     irRxHeadLen = 0;

/* Full TX/RX capture buffers — contents inspectable under a debugger. */
static uint8_t irTxDump[256];
static u32     irTxDumpLen = 0;
static uint8_t irRxDump[2048];
static u32     irRxDumpLen = 0;

/* -------------------------------------------------------------------------- */

static void ir_enable(void) {
    I2C_write(REG_IER,     0);     /* clear IER[4] = exit sleep mode */
    I2C_write(REG_IOSTATE, 0);     /* IOState = 0                    */
}

static void ir_disable(void) {
    I2C_write(REG_IER,     BIT(4)); /* set IER[4]  = enter sleep mode */
    I2C_write(REG_IOSTATE, BIT(0)); /* IOState bit 0 = 1              */
}

/*
 * Program the baud-rate divisor into DLL/DLH.
 * Baud = 1 152 000 / divisor. For 115200 baud: divisor = 10.
 */
static void ir_setbitrate(u16 divisor) {
    /* Disable TX and RX, then reset FIFOs */
    I2C_write(REG_EFCR, BIT(1) | BIT(2));
    I2C_write(REG_FCR,  BIT(1) | BIT(2));
    svcSleepThread(2000000LL); /* 2 ms */

    ir_enable();

    /* Set DLAB=1 to access DLL/DLH; configure 8N1 frame */
    I2C_write(REG_LCR, 0x03u | BIT(7));
    I2C_write(REG_DLL, (u8)(divisor & 0xFF));
    I2C_write(REG_DLH, (u8)(divisor >> 8));
    I2C_write(REG_LCR, 0x03u); /* clear DLAB */
    svcSleepThread(2000000LL); /* 2 ms */

    /* Read back DLL/DLH (mirrors the stock IR module behaviour) */
    I2C_write(REG_LCR, 0x03u | BIT(7));
    I2C_read(REG_DLL);
    I2C_read(REG_DLH);
    I2C_write(REG_LCR, 0x03u);

    /* Write divisor a second time */
    I2C_write(REG_LCR, 0x03u | BIT(7));
    I2C_write(REG_DLL, (u8)(divisor & 0xFF));
    I2C_write(REG_DLH, (u8)(divisor >> 8));
    I2C_write(REG_LCR, 0x03u);

    /* The chip is left awake after reprogramming the divisor — entering
     * sleep here would add a transient sleep state on every baud change. */
}

/* ---- Blocking RX/TX ----------------------------------------------------- */

#define RX_TIMEOUT  40000
#define RX_MAX_WAIT 30

size_t ir_cmd1(uint8_t *rxbuf) {
    uint8_t  rxlvl;
    u32      tc      = 0;
    int      timeout = RX_TIMEOUT;
    bool     loop    = true;

    I2C_write(REG_FCR,  0x03u);
    I2C_write(REG_EFCR, 0x04u);

    do {
        int i = 0;
        while (!(rxlvl = I2C_read(REG_RXLVL)) && i < timeout)
            i++;
        if (i == timeout) break;
        timeout = RX_MAX_WAIT;
        if (tc + rxlvl > 255u) {
            rxlvl = (uint8_t)(255u - tc);
            loop  = false;
        }
        I2C_read_array(REG_FIFO, rxbuf + tc, rxlvl);
        tc += rxlvl;
    } while (loop && tc < 136u);

    I2C_write(REG_EFCR, 0x06u);
    I2C_write(REG_FCR,  0x00u);
    return (size_t)tc;
}

void ir_cmd2(size_t size, const uint8_t *txbuf) {
    const uint8_t *ptr = txbuf;
    uint8_t txlvl;
    size_t  sent;

    I2C_write(REG_FCR,  0x05u);
    I2C_write(REG_EFCR, 0x02u);

    do {
        txlvl = I2C_read(REG_TXLVL);
        sent  = size > txlvl ? txlvl : size;
        I2C_write_array(REG_FIFO, ptr, (u32)sent);
        ptr  += sent;
        size -= sent;
    } while (size);

    while (!(I2C_read(REG_LSR) & LSR_TEMT));

    I2C_write(REG_EFCR, 0x06u);
    I2C_write(REG_FCR,  0x00u);
}

/* -------------------------------------------------------------------------- */

void ir_init(void) {
    I2C_init();

    /*
     * Soft reset before any reconfiguration: pulse IOCONTROL bit 3, then
     * wait 10 ms for the chip's internal state machine to settle.
     *
     * On Old 3DS the OS boot leaves the chip in a known good state. On the
     * New 3DS family the post-boot chip state is undefined — sometimes IrDA
     * mode, often raw UART, occasionally with stale FIFO contents. The soft
     * reset guarantees we start from the datasheet default and is harmless
     * (idempotent) on Old 3DS.
     */
    I2C_write(REG_IOCONTROL, BIT(3));
    svcSleepThread(10000000LL); /* 10 ms */

    /*
     * Configure SC16IS750 GPIO[0] as OUTPUT. This pin drives the IR
     * transceiver power: IOSTATE[0]=0 -> transceiver ON, =1 -> OFF.
     *
     * Must be set before any write to IOSTATE has effect. The soft reset
     * above clears IODIR back to default (all pins as input), and a pin in
     * input mode ignores IOSTATE writes — so without this step ir_enable()'s
     * `IOSTATE=0` is a no-op and the IR LED + photodiode never get powered
     * (no TX, no RX).
     */
    I2C_write(REG_IODIR, 0x01u);

    ir_setbitrate(10); /* 1 152 000 / 10 = 115 200 baud */

    ir_enable(); /* IOSTATE[0]=0 → IR transceiver powered ON, stays ON */

    /*
     * Chip setup sequence:
     *   1. LCR = 0xBF   -> remap REG2 (0x10) to EFR
     *   2. EFR |= 0x10  -> enable Enhanced Functions (TCR/TLR etc.)
     *   3. LCR = 0x03   -> restore 8N1, DLAB=0
     *   4. MCR  = (mode<<6) | 0x04   -> IrDA SIR + TCR/TLR access
     * MCR is written (not OR'd) so the chip reaches a known MCR=0x44 every
     * time, regardless of any DTR/RTS/loopback bits left set by the OS.
     */
    I2C_write(REG_LCR, 0xBFu);
    uint8_t efr = I2C_read(REG_FCR); /* reg at 0x10 is EFR while LCR=0xBF */
    I2C_write(REG_FCR, efr | 0x10u);
    I2C_write(REG_LCR, 0x03u);

    I2C_write(REG_MCR, MCR_IRDA | BIT(2));

    /*
     * FIFO permanently enabled; both TX and RX permanently active.
     * Setup ends with FCR=0x06 (reset) -> FCR=0x01 (enable) and EFCR=0 (no
     * TX/RX disable). Toggling EFCR between TX-only / RX-only / disabled per
     * byte would cost extra I2C transactions per burst and, worse, open a
     * window where bytes from the peer arrive while RX is disabled and get
     * dropped silently. In streaming mode the chip is left in one state.
     */
    I2C_write(REG_FCR, 0x06u);  /* reset RX + TX FIFOs */
    I2C_write(REG_FCR, 0x01u);  /* enable FIFO */
    I2C_write(REG_EFCR, 0x00u); /* TX and RX both enabled */

    LOG("IR: init done, MCR=0x%02x", I2C_read(REG_MCR));
}

void ir_deinit(void) {
    ir_disable();
}

/* -------------------------------------------------------------------------- */

/* svcGetSystemTick rate is 268,111,856 Hz, so 50 ms is ~13.4 M ticks. A
 * worst-case PokeWalker packet (136 B at 115200 baud SIR) takes ~12 ms over
 * the wire plus I2C overhead — 50 ms is a comfortable margin without letting
 * a stuck bus hang for a noticeable amount of real time. */
#define IR_SEND_DEADLINE_TICKS  (268111856ULL / 20ULL)  /* 50 ms */

void ir_send(const uint8_t *buf, size_t len) {
    u64 t0 = svcGetSystemTick();
    u64 deadline = t0 + IR_SEND_DEADLINE_TICKS;
    const uint8_t *ptr = buf;
    uint8_t txlvl;
    size_t  sent;
    size_t  origLen = len;
    bool    timed_out = false;

    /* No FCR/EFCR toggle — the chip stays in "both enabled, FIFO on" mode
     * set by ir_init(). Just write to the FIFO and wait for it to drain.
     *
     * Both inner loops are bounded by a soft deadline: if the SC16IS750
     * stops responding (loose IR link, I2C bus contention, chip wedged) the
     * app would otherwise spin forever. Bailing out lets the next frame
     * resume normal pacing — one dropped packet is recoverable, an infinite
     * spin is not. */
    while (len) {
        if (svcGetSystemTick() >= deadline) {
            timed_out = true;
            break;
        }
        txlvl = I2C_read(REG_TXLVL);
        sent  = len > txlvl ? txlvl : len;
        if (sent > 0) {
            I2C_write_array(REG_FIFO, ptr, (u32)sent);
            ptr += sent;
            len -= sent;
        }
    }

    /* Wait until both THR and TSR are empty (also bounded). */
    while (!timed_out && !(I2C_read(REG_LSR) & LSR_TEMT)) {
        if (svcGetSystemTick() >= deadline) {
            timed_out = true;
            break;
        }
    }

    u64 elapsed = svcGetSystemTick() - t0;
    irBlockedTicks += elapsed;
    irTxCount++;
    irTxBytes += origLen;
    irTxLastUs = (u32)(elapsed / 268);
    irTxHeadLen = origLen < 8 ? origLen : 8;
    memcpy(irTxHead, buf, irTxHeadLen);
    irTxDumpLen = origLen < sizeof(irTxDump) ? origLen : sizeof(irTxDump);
    memcpy(irTxDump, buf, irTxDumpLen);
    if (timed_out) {
        LOG("IR: ir_send timed out after %lu us, %lu/%lu bytes sent",
            (unsigned long)(elapsed / 268),
            (unsigned long)(origLen - len),
            (unsigned long)origLen);
    }
}

/* ---- Streaming TX/RX ---------------------------------------------------- */
/*
 * Half-duplex strategy:
 *   The chip stays in EFCR=0 (TX+RX both enabled) and FCR=0x01 (FIFO on)
 *   permanently after ir_init(); EFCR is not toggled per burst.
 *
 *   The SC16IS750 in IrDA SIR mode (MCR[6]=1) handles half-duplex media
 *   access internally: while TX is shifting bits out, the RX decoder
 *   ignores its own pulses (the chip has its own loopback gate for SIR).
 *   Using the EFCR disable bits for echo suppression would, in a streaming
 *   model, drop bytes from the peer that arrive while EFCR=0x02 (RX
 *   disabled), so EFCR stays 0 throughout.
 *
 *   The `irInTxMode` flag is a software gate on ir_recv_poll: while a TX
 *   burst is in progress RX polls are skipped, so any byte that did land in
 *   the RX FIFO (echo or otherwise) is drained by ir_tx_flush_to_rx()
 *   rather than handed up to the SCI3 emulator as if it were peer data.
 */

static u32 irStreamTxBytes = 0;
static bool irTxActive = false;
static bool irRxActive = false;
static bool irInTxMode = false;  /* true = TX burst in progress */

void ir_tx_start(void) {
    irTxActive = true;
    irStreamTxBytes = 0;
    /* No hardware change — irInTxMode set on first byte */
}

void ir_tx_byte(uint8_t b) {
    irTracePush(IR_TRACE_TX_BYTE, b, 0, 0);

    u64 t0 = svcGetSystemTick();

    irInTxMode = true; /* gate ir_recv_poll until burst ends */

    while (I2C_read(REG_TXLVL) == 0);
    I2C_write(REG_FIFO, b);

    irBlockedTicks += svcGetSystemTick() - t0;
    irStreamTxBytes++;

    /* Deferred logging */
    irTxBytes++;
    if (irStreamTxBytes <= 8) {
        irTxHead[irStreamTxBytes - 1] = b;
        irTxHeadLen = irStreamTxBytes;
    }
    if (irStreamTxBytes <= sizeof(irTxDump)) {
        irTxDump[irStreamTxBytes - 1] = b;
        irTxDumpLen = irStreamTxBytes;
    }
}

/* Called when 320 cycles pass after the last TX byte without a new TDR3
 * write — i.e. the ROM's packet burst is done. Waits for the TX shift
 * register to drain, then drains any echo bytes the chip captured in the RX
 * FIFO during the burst, before ir_recv_poll resumes reading the FIFO. */
void ir_tx_flush_to_rx(void) {
    if (!irInTxMode) return;

    u64 t0 = svcGetSystemTick();
    while (!(I2C_read(REG_LSR) & LSR_TEMT));

    /* Drain own-echo bytes captured during TX (paranoia — IrDA SIR
     * mode shouldn't echo, but if anything ended up in the RX FIFO
     * it will not be a valid packet from the peer). */
    while (I2C_read(REG_RXLVL) > 0)
        (void)I2C_read(REG_FIFO);

    irBlockedTicks += svcGetSystemTick() - t0;
    irInTxMode = false;
}

void ir_tx_end(void) {
    u64 t0 = svcGetSystemTick();
    if (irInTxMode) {
        while (!(I2C_read(REG_LSR) & LSR_TEMT));
        while (I2C_read(REG_RXLVL) > 0)
            (void)I2C_read(REG_FIFO);
        irInTxMode = false;
    }
    irBlockedTicks += svcGetSystemTick() - t0;

    irTxActive = false;
    irTxCount++;
    irTxLastUs = 0;
}

/* -------------------------------------------------------------------------- */

void ir_recv_start(void) {
    /* RX is permanently enabled by ir_init().  This is now a state-only
     * marker — nothing to write to the chip. */
    irRxActive = true;
}

size_t ir_recv_poll(uint8_t *buf, size_t maxlen) {
    if (irInTxMode) return 0;  /* gate during TX burst — see header comment */

    uint8_t rxlvl = I2C_read(REG_RXLVL);
    if (rxlvl == 0) return 0;
    if (rxlvl > maxlen) rxlvl = (uint8_t)maxlen;
    I2C_read_array(REG_FIFO, buf, rxlvl);

    irRxCount++;
    irRxBytes += rxlvl;
    irRxHeadLen = rxlvl < 8 ? rxlvl : 8;
    memcpy(irRxHead, buf, irRxHeadLen);
    if (irRxDumpLen + rxlvl <= sizeof(irRxDump)) {
        memcpy(irRxDump + irRxDumpLen, buf, rxlvl);
        irRxDumpLen += rxlvl;
    }

    return (size_t)rxlvl;
}

void ir_recv_stop(void) {
    /* No hardware change — RX stays enabled.  Just clear the flag. */
    irRxActive = false;
}

/* Eager RX drain hook (see ir.h). The SC16IS750 buffers bytes in its own RX
 * FIFO, which ir_recv_poll reads directly, so there is nothing to pre-drain
 * here. */
void ir_recv_pump(void) {
    /* no-op: real-hardware FIFO model */
}

uint64_t ir_get_blocked_ticks(void) {
    u64 t = irBlockedTicks;
    irBlockedTicks = 0;
    return t;
}

void ir_log_events(void) {
    if (irTxCount) {
        LOG("IR TX: %lu sends, %lu B, %lu us  hdr:%02X %02X %02X %02X %02X %02X %02X %02X",
            (unsigned long)irTxCount, (unsigned long)irTxBytes,
            (unsigned long)irTxLastUs,
            irTxHeadLen > 0 ? irTxHead[0] : 0, irTxHeadLen > 1 ? irTxHead[1] : 0,
            irTxHeadLen > 2 ? irTxHead[2] : 0, irTxHeadLen > 3 ? irTxHead[3] : 0,
            irTxHeadLen > 4 ? irTxHead[4] : 0, irTxHeadLen > 5 ? irTxHead[5] : 0,
            irTxHeadLen > 6 ? irTxHead[6] : 0, irTxHeadLen > 7 ? irTxHead[7] : 0);
        irTxDumpLen = 0;
        irTxCount = 0;
        irTxBytes = 0;
        irTxHeadLen = 0;
    }
    if (irRxCount) {
        LOG("IR RX: %lu polls, %lu B  last:%02X %02X %02X %02X %02X %02X %02X %02X",
            (unsigned long)irRxCount, (unsigned long)irRxBytes,
            irRxHeadLen > 0 ? irRxHead[0] : 0, irRxHeadLen > 1 ? irRxHead[1] : 0,
            irRxHeadLen > 2 ? irRxHead[2] : 0, irRxHeadLen > 3 ? irRxHead[3] : 0,
            irRxHeadLen > 4 ? irRxHead[4] : 0, irRxHeadLen > 5 ? irRxHead[5] : 0,
            irRxHeadLen > 6 ? irRxHead[6] : 0, irRxHeadLen > 7 ? irRxHead[7] : 0);
        irRxDumpLen = 0;
        irRxCount = 0;
        irRxBytes = 0;
        irRxHeadLen = 0;
    }
}
