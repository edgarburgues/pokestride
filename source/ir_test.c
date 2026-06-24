/*
 * Standalone IR test: connect to a real PokeWalker over the IR hardware
 * layer, bypassing the emulator entirely. Useful for isolating whether a
 * problem is in the I2C/SC16IS750 setup or elsewhere. ir_test_run() is
 * called instead of the normal emulation loop.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include "i2c.h"
#include "ir.h"

#define REG_MCR     0x20u
#define XOR_KEY     0xAA
#define XOR_KEY16   0xAAAA
#define XOR_KEY32   0xAAAAAAAA

static uint8_t rxbuf[256] __attribute__((aligned(4)));
static uint8_t txbuf[256] __attribute__((aligned(4)));

/* ir_cmd1, ir_cmd2 live in ir.c (exported via ir.h) */

/* ---- Protocol ---- */

static uint16_t calc_crc(uint8_t cmd, uint8_t extra, uint32_t sess,
                          const uint8_t *payload, size_t len) {
    uint32_t crc = 0x0002;
    crc += (uint32_t)cmd   << 8;
    crc += (uint32_t)extra << 0;
    crc += ( sess        & 0xff) << 8;
    crc += ((sess >>  8) & 0xff) << 0;
    crc += ((sess >> 16) & 0xff) << 8;
    crc += ((sess >> 24) & 0xff) << 0;
    for (size_t i = 0; i < len; i++)
        crc += payload[i] << ((i & 1) ? 0 : 8);
    while (crc >> 16)
        crc = (crc & 0xffff) + (crc >> 16);
    return (uint16_t)crc;
}

static void pw_send_packet(uint8_t cmd, uint8_t extra, uint32_t sessid,
                            size_t size, const uint8_t *data) {
    uint16_t crc = calc_crc(cmd, extra, sessid, data, size);
    txbuf[0] = cmd ^ XOR_KEY;
    txbuf[1] = extra ^ XOR_KEY;
    ((uint16_t*)txbuf)[1] = crc ^ XOR_KEY16;
    ((uint32_t*)txbuf)[1] = sessid ^ XOR_KEY32;
    for (size_t i = 0; i < size; i++) txbuf[i+8] = data[i] ^ XOR_KEY;
    ir_cmd2(8 + size, txbuf);
}

/* ---- Logging helper ---- */

static FILE *tlog = NULL;

static void tprint(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (tlog) {
        va_start(ap, fmt);
        vfprintf(tlog, fmt, ap);
        va_end(ap);
        fflush(tlog);
    }
}

static void tprint_hex(const char *prefix, const uint8_t *data, size_t len) {
    tprint("%s (%zu B):", prefix, len);
    for (size_t i = 0; i < len && i < 16; i++)
        tprint(" %02X", data[i]);
    if (len > 16) tprint(" ...");
    tprint("\n");
}

/* ---- Test entry point ---- */

void ir_test_run(void) {
    /* Use the existing console (bottom screen) from main() */

    printf("IR Direct Test\n");
    printf("Point PW at 3DS IR.\n\n");

    chdir("sdmc:/3ds/pokeStride");
    tlog = NULL; /* file logging disabled — tprint falls back to console only */
    tprint("=== IR Direct Test ===\n");

    /* Init hardware */
    ir_init();

    tprint("IR init done. MCR=0x%02X\n\n", I2C_read(REG_MCR));

    /* Step 1: Scan for beacon */
    tprint("Scanning for PW beacon...\n");
    bool found = false;
    for (int attempt = 0; attempt < 30; attempt++) {
        size_t len = ir_cmd1(rxbuf);
        if (len == 1 && (rxbuf[0] ^ XOR_KEY) == 0xFC) {
            tprint("  BEACON found! (attempt %d)\n", attempt);
            found = true;
            break;
        }
        if (len > 0) {
            tprint_hex("  Got (not beacon)", rxbuf, len);
        }
        svcSleepThread(16666666LL);
    }

    if (!found) {
        tprint("\nNo beacon found. Is PW in CONNECT?\n");
        tprint("Press START to exit.\n");
        if (tlog) fclose(tlog);
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        return;
    }

    /* Step 2: Send SYN */
    uint32_t our_session = (u32)svcGetSystemTick();
    tprint("\nSending SYN (session=%08lX)...\n", (unsigned long)our_session);
    pw_send_packet(0xFA, 0x01, our_session, 0, NULL);
    tprint_hex("  TX SYN raw", txbuf, 8);

    /* Step 3: Receive ACK */
    tprint("Waiting for ACK...\n");
    bool got_ack = false;
    uint32_t pw_session = 0;
    for (int attempt = 0; attempt < 16; attempt++) {
        size_t len = ir_cmd1(rxbuf);
        if (len >= 8) {
            tprint_hex("  RX raw", rxbuf, len);
            ((uint32_t*)rxbuf)[0] ^= XOR_KEY32;
            ((uint32_t*)rxbuf)[1] ^= XOR_KEY32;
            tprint("  Decoded: cmd=%02X extra=%02X\n", rxbuf[0], rxbuf[1]);
            if (rxbuf[0] == 0xF8 && rxbuf[1] == 0x02) {
                pw_session = ((uint32_t*)rxbuf)[1];
                tprint("  ACK! PW session=%08lX\n", (unsigned long)pw_session);
                got_ack = true;
                break;
            }
        } else if (len == 1 && (rxbuf[0] ^ XOR_KEY) == 0xFC) {
            tprint("  Beacon (retry %d)... resending SYN\n", attempt);
            pw_send_packet(0xFA, 0x01, our_session, 0, NULL);
        } else if (len > 0) {
            tprint_hex("  RX unknown", rxbuf, len);
        }
        svcSleepThread(16666666LL);
    }

    if (!got_ack) {
        tprint("\nFAILED: No ACK. PW didn't see our SYN.\n");
        tprint("The 3DS IR TX is not reaching the PW.\n");
    } else {
        uint32_t agreed = pw_session ^ our_session;
        tprint("\nAgreed session: %08lX\n", (unsigned long)agreed);

        /* Step 4: Send PING */
        tprint("Sending PING...\n");
        pw_send_packet(0x24, 0x01, agreed, 0, NULL);

        /* Step 5: Receive PONG */
        for (int attempt = 0; attempt < 16; attempt++) {
            size_t len = ir_cmd1(rxbuf);
            if (len >= 8) {
                tprint_hex("  RX raw", rxbuf, len);
                ((uint32_t*)rxbuf)[0] ^= XOR_KEY32;
                ((uint32_t*)rxbuf)[1] ^= XOR_KEY32;
                tprint("  Decoded: cmd=%02X extra=%02X\n", rxbuf[0], rxbuf[1]);
                if (rxbuf[0] == 0x26) {
                    tprint("  PONG! Connection fully verified!\n");
                    break;
                }
            } else if (len > 0) {
                tprint_hex("  RX", rxbuf, len);
            }
            svcSleepThread(16666666LL);
        }

        /* Disconnect */
        pw_send_packet(0xF4, 0x01, agreed, 0, NULL);
        tprint("Disconnected.\n");
    }

    /* Disable IR */
    ir_deinit();

    tprint("\nTest complete.\n");
    if (tlog) fclose(tlog);
    printf("\n\x1b[33mPress START to exit.\x1b[0m\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}
