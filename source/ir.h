#pragma once

#include <stddef.h>
#include <stdint.h>

/* Initialize SC16IS750 IR hardware (I2C bus 3, 115200 baud, IrDA SIR mode). */
void ir_init(void);

/* Shut down IR hardware (enter sleep mode). */
void ir_deinit(void);

/* Blocking RX: wait for a packet with timeout. Returns bytes received. */
size_t ir_cmd1(uint8_t *rxbuf);

/* Blocking TX: send size bytes, wait for completion. */
void ir_cmd2(size_t size, const uint8_t *txbuf);

/* Send len bytes from buf over IR. Blocks until TX FIFO drains. */
void ir_send(const uint8_t *buf, size_t len);

/* Streaming TX: set up SC16IS750 TX mode (call when TE rises). */
void ir_tx_start(void);

/* Streaming TX: write one byte to SC16IS750 TX FIFO (call when SCI3
 * shift register completes, i.e. TDRE countdown fires). */
void ir_tx_byte(uint8_t b);

/* Streaming TX: signal that no more bytes are pending — drain the TX FIFO
 * and switch the SC16IS750 to RX mode. Called when 320 H8 cycles pass after
 * TDRE without a new TDR3 write (= end of the ROM's packet burst). */
void ir_tx_flush_to_rx(void);

/* Streaming TX: wait for SC16IS750 to finish, disable TX (call when TE falls). */
void ir_tx_end(void);

/* Enable RX FIFO — call when ROM sets RE. */
void ir_recv_start(void);

/* Discard everything in the SC16IS750 RX FIFO. Call on the RE rising edge
 * so bytes captured while the ROM's receiver was off (impossible to receive
 * on real hardware) are not replayed into the ROM as fresh traffic. */
void ir_rx_flush(void);

/* Non-blocking RX poll: read RXLVL, if >0 read up to maxlen bytes into buf.
 * Returns the number of bytes read (0 if nothing available). */
size_t ir_recv_poll(uint8_t *buf, size_t maxlen);

/* Eager RX drain hook, called every IR poll tick. The SC16IS750 RX FIFO
 * already buffers in hardware and ir_recv_poll reads it on demand, so this
 * is a no-op; it exists so the core links against this IR layer. */
void ir_recv_pump(void);

/* Disable RX — call when ROM clears RE. */
void ir_recv_stop(void);

/* Return (and reset) the total system ticks spent blocked in ir_send().
 * Used by the pacing loop to compensate for I2C transfer time.          */
uint64_t ir_get_blocked_ticks(void);

/* Flush deferred IR event counters to the log file.
 * Must be called from the main loop (safe stack depth for fprintf).     */
void ir_log_events(void);

/* Diagnostics for the peer-handshake investigation: bytes read from the IR
 * FIFO this period, how many were stripped as our own echo, and the current
 * outstanding echo debt. Reading resets the two totals. */
void ir_get_rx_stats(uint32_t *raw, uint32_t *stripped, uint32_t *owed);
