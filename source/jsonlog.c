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
 * Formatear tampoco sale gratis: ~8 us por evento y ~800 eventos en un frame
 * de la fase de lectura son ~7 ms parado, suficiente para que el FIFO de 64 B
 * del SC16IS750 se desborde con una respuesta de 136 B (medido: llegaron 113).
 * Por eso los eventos de byte IR ni siquiera pasan por aquí durante el run:
 * se guardan en crudo en el ring de irtrace.c y se formatean al SALIR. Aquí
 * solo caen los eventos sueltos, uno o dos por frame, que van a RAM y bajan a
 * la SD en frames sin actividad IR. Durante una sesión el coste es cero.
 * ------------------------------------------------------------------------ */

/* Buffer solo para los eventos SUELTOS que se emiten durante el run (sesión,
 * rol, error, stats: uno o dos por frame). Los miles de eventos de byte IR no
 * pasan por aquí: viven en crudo en el ring de irtrace.c y se formatean al
 * salir, en modo directo, cuando congelarse ya da igual. */
#define JL_CAP  (256u * 1024u)

static FILE  *jf         = NULL;
static char  *jl_buf     = NULL;
static size_t jl_len     = 0;      /* bytes pendientes de escribir      */
static uint32_t jl_dropped = 0;    /* eventos perdidos por buffer lleno */
static bool   jl_direct  = false;  /* true = puede vaciar por su cuenta */

static uint64_t tick_now(void) {
    return svcGetSystemTick();
}

/* Añade texto printf-style al buffer de RAM.
 *
 * Durante el run NO toca la SD: si el buffer se llena se descarta el evento,
 * porque escribir a mitad de sesión congela el emulador y el aire lo nota.
 * En modo directo (volcado final) sí se vacía solo, pero SIEMPRE en bloques de
 * JL_CAP. Nunca línea a línea: el fichero está en `_IONBF`, así que un fprintf
 * suelto es una escritura a SD, y con ~3 por evento el volcado se vuelve tan
 * lento que no termina — medido, se quedó en 82 KB de los ~700 KB, y con ellos
 * se perdió justo la traza de la sesión que sí funcionó. */
static void jl_put(const char *fmt, ...) {
    if (!jf || !jl_buf) return;

    for (int intento = 0; intento < 2; intento++) {
        size_t space = JL_CAP - jl_len;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(jl_buf + jl_len, space, fmt, ap);
        va_end(ap);

        if (n < 0) return;
        if ((size_t)n < space) { jl_len += (size_t)n; return; }

        jl_buf[jl_len] = '\0';       /* no cabía: deshacer el trozo a medias */
        if (!jl_direct) { jl_dropped++; return; }
        jsonlog_flush();             /* vaciar el bloque entero y reintentar */
    }
    jl_dropped++;
}

/* A partir de aquí el buffer puede vaciarse solo. Se llama al salir, antes de
 * formatear el ring de trazas, cuando ya no hay sesión que romper. */
void jsonlog_set_direct(void) {
    if (!jf) return;
    jsonlog_flush();
    jl_direct = true;
}

void jsonlog_init(const char *path) {
    if (!path) return;
    jl_buf = malloc(JL_CAP);
    if (!jl_buf) return;          /* sin RAM: el logger queda deshabilitado */
    jl_len = 0; jl_dropped = 0;

    jf = fopen(path, "w");
    if (!jf) { free(jl_buf); jl_buf = NULL; return; }
    /* Sin buffer de stdio: escribimos bloques grandes nosotros mismos, y así
     * ningún fprintf puede disparar un write a SD por su cuenta. */
    setvbuf(jf, NULL, _IONBF, 0);
}

/* Baja a la SD lo acumulado desde la última vez. SOLO debe llamarse desde un
 * frame sin actividad IR: un write a SD congela el emulador cientos de ms. */
void jsonlog_flush(void) {
    if (!jf || !jl_buf || jl_len == 0) return;
    fwrite(jl_buf, 1, jl_len, jf);   /* un solo write grande, nunca por lineas */
    jl_len = 0;
    fflush(jf);
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

static void header(uint64_t tick, const char *kind) {
    jl_put("{\"t\":%llu,\"ev\":\"%s\"", (unsigned long long)tick, kind);
}

void jsonlog_ev_at(uint64_t tick, const char *kind, const char *extra_fmt, ...) {
    if (!jf) return;
    header(tick, kind);
    if (extra_fmt && *extra_fmt) {
        char extra[256];
        va_list ap;
        va_start(ap, extra_fmt);
        vsnprintf(extra, sizeof extra, extra_fmt, ap);
        va_end(ap);
        jl_put(",%s", extra);
    }
    jl_put("}\n");
}

void jsonlog_ev(const char *kind, const char *extra_fmt, ...) {
    if (!jf) return;
    header(tick_now(), kind);
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
    if (!jf || len == 0) return;
    header(tick_now(), kind);
    jl_put(",\"bytes\":[");
    for (uint32_t i = 0; i < len; i++)
        jl_put("%s\"0x%02X\"", i ? "," : "", buf[i]);
    jl_put("],\"pc\":\"0x%04X\",\"len\":%lu}\n", pc, (unsigned long)len);
}
