#pragma once

#include <stdint.h>

enum IrTraceType {
    IR_TRACE_TX_BYTE   = 'T',
    IR_TRACE_RX_BYTE   = 'R',
    IR_TRACE_TE_ON     = '+',
    IR_TRACE_TE_OFF    = '-',
    IR_TRACE_RE_ON     = '>',
    IR_TRACE_RE_OFF    = '<',
    IR_TRACE_RX_POLL   = 'P',
    IR_TRACE_ROLE      = 'S',
};

struct IrTraceEvent {
    uint8_t  type;
    uint8_t  data;
    uint16_t pc;
    uint32_t cycle;
};

#define IR_TRACE_SIZE 2048

void irTraceInit(void);
void irTracePush(uint8_t type, uint8_t data, uint16_t pc, uint32_t cycle);
void irTraceFlush(void);
