#include <3ds.h>
#include <string.h>
#include <stdlib.h>
#include "irtrace.h"
#include "jsonlog.h"
#include "log.h"

/* Reservado en tiempo de ejecución, no en .bss: son 4 MiB y si el loader no
 * pudiera con ellos la aplicación ni arrancaría. Así, como mucho, se queda sin
 * trazas — que es un fastidio, no un ladrillo. */
static struct IrTraceEvent *traceRing = NULL;
static uint32_t traceCap = 0;            /* entradas reales; potencia de 2 */
static volatile uint32_t traceHead = 0;  /* written by hot path */
static uint32_t traceTail = 0;           /* read by main loop */

void irTraceInit(void) {
    traceHead = 0;
    traceTail = 0;
    if (!traceRing) {
        for (uint32_t cap = IR_TRACE_SIZE; cap >= 8192u; cap /= 2u) {
            traceRing = malloc((size_t)cap * sizeof *traceRing);
            if (traceRing) { traceCap = cap; break; }
        }
        LOG("IR trace ring: %lu entradas (%lu KiB)",
            (unsigned long)traceCap,
            (unsigned long)((size_t)traceCap * sizeof *traceRing / 1024));
    }
}

void irTracePush(uint8_t type, uint8_t data, uint16_t pc, uint32_t cycle) {
    if (!traceRing) return;
    uint32_t idx = traceHead & (traceCap - 1);
    /* El tick se toma AQUI, no al volcar. Cuando se estampaba al formatear,
     * todos los eventos de un frame salian con la misma hora y las paradas del
     * emulador habia que deducirlas de los huecos entre frames. */
    traceRing[idx].tick  = svcGetSystemTick();
    traceRing[idx].type  = type;
    traceRing[idx].data  = data;
    traceRing[idx].pc    = pc;
    traceRing[idx].cycle = cycle;
    traceHead++;
}

uint32_t irTraceHead(void) { return traceHead; }

uint32_t irTraceFlush(void) {
    /* Drain the ring to BOTH the text log AND the structured JSON log.
     * Either sink may be NULL-disabled independently. */
    if (!traceRing) return 0;
    uint32_t head = traceHead;
    uint32_t drained = head - traceTail;
    if (drained > traceCap) {
        /* El ring dio la vuelta: lo viejo ya se sobrescribió. Saltar a lo que
         * queda vivo y dejar constancia en vez de emitir basura. */
        jsonlog_ev("ir_trace_overflow", "\"lost\":%lu",
                   (unsigned long)(drained - traceCap));
        traceTail = head - traceCap;
    }
    while (traceTail != head) {
        uint32_t idx = traceTail & (traceCap - 1);
        struct IrTraceEvent *e = &traceRing[idx];

        /* --- Text log --- */
        if (logFile) {
            switch (e->type) {
            case IR_TRACE_TX_BYTE:
                fprintf(logFile, "[%08lX] TX %02X (PC=%04X)\n",
                        (unsigned long)e->cycle, e->data, e->pc);
                break;
            case IR_TRACE_RX_BYTE:
                fprintf(logFile, "[%08lX] RX %02X (PC=%04X)\n",
                        (unsigned long)e->cycle, e->data, e->pc);
                break;
            case IR_TRACE_RX_POLL:
                fprintf(logFile, "[%08lX] RX_POLL %u bytes\n",
                        (unsigned long)e->cycle, e->data);
                break;
            case IR_TRACE_ROLE:
                fprintf(logFile, "[%08lX] ROLE=%u\n",
                        (unsigned long)e->cycle, e->data);
                break;
            default:
                fprintf(logFile, "[%08lX] %c\n",
                        (unsigned long)e->cycle, e->type);
                break;
            }
        }

        /* --- Structured JSON log --- */
        switch (e->type) {
        case IR_TRACE_TX_BYTE:
            jsonlog_ev_at(e->tick, "ir_tx",
                       "\"byte\":\"0x%02X\",\"pc\":\"0x%04X\",\"cyc\":%lu",
                       e->data, e->pc, (unsigned long)e->cycle);
            break;
        case IR_TRACE_RX_BYTE:
            jsonlog_ev_at(e->tick, "ir_rx_byte",
                       "\"byte\":\"0x%02X\",\"pc\":\"0x%04X\",\"cyc\":%lu",
                       e->data, e->pc, (unsigned long)e->cycle);
            break;
        case IR_TRACE_RX_POLL:
            jsonlog_ev_at(e->tick, "ir_rx_poll",
                       "\"n\":%u,\"cyc\":%lu",
                       (unsigned)e->data, (unsigned long)e->cycle);
            break;
        case IR_TRACE_TE_ON:
            jsonlog_ev_at(e->tick, "ir_te_on",  "\"pc\":\"0x%04X\"", e->pc); break;
        case IR_TRACE_TE_OFF:
            jsonlog_ev_at(e->tick, "ir_te_off", "\"pc\":\"0x%04X\"", e->pc); break;
        case IR_TRACE_RE_ON:
            jsonlog_ev_at(e->tick, "ir_re_on",  "\"pc\":\"0x%04X\"", e->pc); break;
        case IR_TRACE_RE_OFF:
            jsonlog_ev_at(e->tick, "ir_re_off", "\"pc\":\"0x%04X\"", e->pc); break;
        case IR_TRACE_ROLE:
            jsonlog_ev_at(e->tick, "ir_role_trace",
                       "\"role\":%u,\"cyc\":%lu",
                       (unsigned)e->data, (unsigned long)e->cycle);
            break;
        default:
            break;
        }

        traceTail++;
    }
    return drained;
}
