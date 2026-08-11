#include "jsonlog.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Por qué este logger vive en RAM
 *
 * La versión anterior escribía a un FILE con un buffer de 32 KiB. Parecía
 * suficiente, pero un frame con una sesión IR activa genera ~250 eventos de
 * byte ≈ 25 KiB: el buffer se llena y stdio hace el write a SD *dentro* del
 * camino caliente. Medido en ir_events.jsonl: los frames con tráfico IR
 * tardaban 640-720 ms en vez de 250, es decir 390-470 ms de emulador
 * CONGELADO — mientras el SC16IS750 seguía vaciando su FIFO y se quedaba
 * mudo a mitad de paquete. El peer recibía paquetes partidos y truncados
 * (10 B + 73 ms + 102 B; 82 de 136 B), o sea que la instrumentación rompía
 * justo lo que intentaba medir.
 *
 * Ahora todo se acumula en RAM y solo se baja a la SD en frames SIN actividad
 * IR (ver irTraceFlush()/jsonlog_flush() en 3ds_main.c) y al cerrar. Durante
 * una sesión de emparejamiento el coste de I/O es exactamente cero.
 * ------------------------------------------------------------------------ */

#define JL_CAP  (3u * 1024u * 1024u)   /* ~10 min de IR denso (314 KB / 72 s) */

static FILE  *jf         = NULL;
static char  *jl_buf     = NULL;
static size_t jl_len     = 0;   /* bytes acumulados en RAM            */
static size_t jl_written = 0;   /* de esos, cuántos ya están en la SD */
static uint32_t jl_dropped = 0; /* eventos perdidos por buffer lleno  */

static uint64_t tick_now(void) {
    return svcGetSystemTick();
}

/* Añade texto printf-style al buffer de RAM. Nunca toca la SD. */
static void jl_put(const char *fmt, ...) {
    if (!jl_buf) return;
    size_t space = JL_CAP - jl_len;
    if (space < 2) { jl_dropped++; return; }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(jl_buf + jl_len, space, fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if ((size_t)n >= space) {   /* no cabía: descartar el trozo a medias */
        jl_buf[jl_len] = '\0';
        jl_dropped++;
        return;
    }
    jl_len += (size_t)n;
}

void jsonlog_init(const char *path) {
    if (!path) return;
    jl_buf = malloc(JL_CAP);
    if (!jl_buf) return;          /* sin RAM: el logger queda deshabilitado */
    jl_len = jl_written = jl_dropped = 0;

    jf = fopen(path, "w");
    if (!jf) { free(jl_buf); jl_buf = NULL; return; }
    /* Sin buffer de stdio: escribimos bloques grandes nosotros mismos, y así
     * ningún fprintf puede disparar un write a SD por su cuenta. */
    setvbuf(jf, NULL, _IONBF, 0);
}

/* Baja a la SD lo acumulado desde la última vez. SOLO debe llamarse desde un
 * frame sin actividad IR: un write a SD congela el emulador cientos de ms. */
void jsonlog_flush(void) {
    if (!jf || !jl_buf) return;
    if (jl_len > jl_written) {
        fwrite(jl_buf + jl_written, 1, jl_len - jl_written, jf);
        jl_written = jl_len;
        fflush(jf);
    }
}

void jsonlog_close(void) {
    if (jf) {
        if (jl_dropped)
            jl_put("{\"t\":%llu,\"ev\":\"jsonlog_dropped\",\"n\":%lu}\n",
                   (unsigned long long)tick_now(), (unsigned long)jl_dropped);
        jsonlog_flush();
        fclose(jf);
        jf = NULL;
    }
    if (jl_buf) { free(jl_buf); jl_buf = NULL; }
}

static void header(const char *kind) {
    jl_put("{\"t\":%llu,\"ev\":\"%s\"",
           (unsigned long long)tick_now(), kind);
}

void jsonlog_ev(const char *kind, const char *extra_fmt, ...) {
    if (!jl_buf) return;
    header(kind);
    if (extra_fmt && *extra_fmt) {
        /* jl_put es variádica fija; para reenviar la lista formateamos aparte */
        char extra[256];
        va_list ap;
        va_start(ap, extra_fmt);
        vsnprintf(extra, sizeof extra, extra_fmt, ap);
        va_end(ap);
        jl_put(",%s", extra);
    }
    jl_put("}\n");
}

void jsonlog_ev_bytes(const char *kind, const uint8_t *buf, uint32_t len,
                      uint16_t pc) {
    if (!jl_buf || len == 0) return;
    header(kind);
    jl_put(",\"bytes\":[");
    for (uint32_t i = 0; i < len; i++)
        jl_put("%s\"0x%02X\"", i ? "," : "", buf[i]);
    jl_put("],\"pc\":\"0x%04X\",\"len\":%lu}\n", pc, (unsigned long)len);
}
