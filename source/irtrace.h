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
    uint64_t tick;    /* systick del INSTANTE del evento, no del volcado */
    uint8_t  type;
    uint8_t  data;
    uint16_t pc;
    uint32_t cycle;
};

/* 262144 entradas x 16 B = 4 MiB.
 *
 * El ring guarda los eventos EN CRUDO y no se formatea nada hasta salir de la
 * aplicación. Formatear cuesta ~8 us por evento (vsnprintf con %llu) y un
 * frame de la fase de lectura mueve ~800 → ~7 ms de emulador parado, con el
 * SC16IS750 sin que nadie le vacíe el FIFO de 64 B mientras entra una
 * respuesta de 136 B. Medido en el aire: llegaron 113 de 136 bytes.
 *
 * En crudo un evento son 16 B frente a los ~68 B que ocupa ya formateado en
 * JSONL, así que 4 MiB dan para ~262k eventos ≈ 16 min de prueba continua
 * (medido: 261 eventos/s) o ~65 sesiones de emparejamiento. Al salir se
 * formatea todo de golpe, cuando ya da igual congelarse. */
#define IR_TRACE_SIZE 262144

void irTraceInit(void);
void irTracePush(uint8_t type, uint8_t data, uint16_t pc, uint32_t cycle);

/* Formatea y vuelca el ring entero. CARO — llamar solo al salir. */
uint32_t irTraceFlush(void);

/* Cabeza del ring, sin drenar nada ni formatear. El main loop la muestrea una
 * vez por frame para saber cuánta actividad IR hubo SOLO en ese frame, y con
 * eso decidir si puede permitirse renderizar. */
uint32_t irTraceHead(void);
