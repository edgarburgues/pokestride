#include <3ds.h>
#include <string.h>
#include "irtrace.h"
#include "jsonlog.h"
#include "log.h"

static struct IrTraceEvent traceRing[IR_TRACE_SIZE];
static volatile uint32_t traceHead = 0;  /* written by hot path */
static uint32_t traceTail = 0;           /* read by main loop */

void irTraceInit(void) {
    traceHead = 0;
    traceTail = 0;
}

void irTracePush(uint8_t type, uint8_t data, uint16_t pc, uint32_t cycle) {
    uint32_t idx = traceHead & (IR_TRACE_SIZE - 1);
    traceRing[idx].type  = type;
    traceRing[idx].data  = data;
    traceRing[idx].pc    = pc;
    traceRing[idx].cycle = cycle;
    traceHead++;
}

void irTraceFlush(void) {
    /* Drain the ring to BOTH the text log AND the structured JSON log.
     * Either sink may be NULL-disabled independently. */
    uint32_t head = traceHead;
    while (traceTail != head) {
        uint32_t idx = traceTail & (IR_TRACE_SIZE - 1);
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
            jsonlog_ev("ir_tx",
                       "\"byte\":\"0x%02X\",\"pc\":\"0x%04X\",\"cyc\":%lu",
                       e->data, e->pc, (unsigned long)e->cycle);
            break;
        case IR_TRACE_RX_BYTE:
            jsonlog_ev("ir_rx_byte",
                       "\"byte\":\"0x%02X\",\"pc\":\"0x%04X\",\"cyc\":%lu",
                       e->data, e->pc, (unsigned long)e->cycle);
            break;
        case IR_TRACE_RX_POLL:
            jsonlog_ev("ir_rx_poll",
                       "\"n\":%u,\"cyc\":%lu",
                       (unsigned)e->data, (unsigned long)e->cycle);
            break;
        case IR_TRACE_TE_ON:
            jsonlog_ev("ir_te_on",  "\"pc\":\"0x%04X\"", e->pc); break;
        case IR_TRACE_TE_OFF:
            jsonlog_ev("ir_te_off", "\"pc\":\"0x%04X\"", e->pc); break;
        case IR_TRACE_RE_ON:
            jsonlog_ev("ir_re_on",  "\"pc\":\"0x%04X\"", e->pc); break;
        case IR_TRACE_RE_OFF:
            jsonlog_ev("ir_re_off", "\"pc\":\"0x%04X\"", e->pc); break;
        case IR_TRACE_ROLE:
            jsonlog_ev("ir_role_trace",
                       "\"role\":%u,\"cyc\":%lu",
                       (unsigned)e->data, (unsigned long)e->cycle);
            break;
        default:
            break;
        }

        traceTail++;
    }
}
