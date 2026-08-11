#pragma once

#include <stdint.h>

/* JSON event logger — writes one JSON object per line to
 * `logs/ir_events.jsonl` on the SD card.  Designed for post-mortem
 * analysis of IR connection attempts: the file can be pulled from the
 * 3DS and replayed against the protocol spec to see exactly where a
 * connection broke down.
 *
 * All events share a 't' field (3DS system tick at emission).
 *
 * Kinds currently emitted:
 *   start            ROM entry point, build marker
 *   ir_role          peerRole changed (RAM 0xF8BE)
 *   ir_err           commsErrorId changed (RAM 0xF7AD)
 *   ir_session       session / localNonce written (RAM 0xF8BA / 0xF8B6)
 *   ir_crc_retry     crcRetryCount incremented (RAM 0xF8C2)
 *   ir_got_packet    gotPacketFlag changed (RAM 0xF8C3)
 *   ir_tx            single byte sent over the wire
 *   ir_rx            batch of bytes read from SC16IS750 FIFO
 *   ir_tx_start      SCI3 TE rising edge — SC16IS750 switched to TX
 *   ir_tx_end        SCI3 TE falling edge
 *   ir_tx_flush      ROM's TX burst went idle — SC16IS750 switch TX→RX
 *   ir_rx_start      SCI3 RE rising edge
 *   ir_rx_stop       SCI3 RE falling edge
 *   frame            per-frame summary (elapsed cycles, steps, watts)
 *   end              graceful shutdown marker
 *
 * Robust to being disabled: if fopen fails (SD full / missing dir), all
 * emit calls silently drop. */

/* Open the log file.  `path` is relative to the homebrew working
 * directory (usually `sdmc:/3ds/pokeStride/`).  Safe to call with NULL
 * `path` to disable. */
void jsonlog_init(const char *path);

/* Close the file flushing any buffered writes. */
void jsonlog_close(void);

/* Emit an event with optional extra JSON key/value pairs (printf-style,
 * inserted between the kind and closing brace).  Pass NULL for no extras.
 *   jsonlog_ev("frame", "\"instr\":%llu", total);
 *   jsonlog_ev("start", NULL);
 */
void jsonlog_ev(const char *kind, const char *extra_fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Igual, pero con el tick del evento en vez del de ahora. Lo usa irtrace.c al
 * volcar el ring: los eventos se guardaron en crudo con SU instante, así que
 * el timestamp es el real y no el del volcado. */
void jsonlog_ev_at(uint64_t tick, const char *kind, const char *extra_fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Cambia a escritura directa al fichero (vuelca antes lo pendiente). Solo al
 * salir: a partir de ahí formatear puede congelar sin consecuencias. */
void jsonlog_set_direct(void);

/* Hex-byte helper (for ir_tx / ir_rx).  Writes a "bytes":[...] array. */
void jsonlog_ev_bytes(const char *kind, const uint8_t *buf, uint32_t len,
                      uint16_t pc);

/* Baja a la SD lo acumulado en RAM desde la última llamada.
 *
 * ⚠ Llamar SOLO desde un frame sin actividad IR. Un write a SD congela el
 * emulador cientos de ms mientras el SC16IS750 sigue vaciando su FIFO, lo que
 * parte los paquetes a media trama. El main loop lo condiciona a que
 * irTraceFlush() haya devuelto 0 eventos. Ver jsonlog.c. */
void jsonlog_flush(void);
