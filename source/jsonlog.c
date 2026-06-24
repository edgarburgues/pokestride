#include "jsonlog.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>

static FILE *jf = NULL;

/* 32 KiB block buffer: a large buffer means few SD writes, so the emulator
 * can keep up with real-time IR timing. Each IR byte event is ~100 bytes,
 * so 32 KiB holds ~300 events (enough for a full handshake + identity
 * exchange), flushed at frame boundaries. A per-event (line-buffered) SD
 * write would stall the H8 emulator enough to break the IR protocol's tight
 * timing (115200 baud, 4-tick gap detection). */
static char jf_buf[32 * 1024];

static uint64_t tick_now(void) {
    return svcGetSystemTick();
}

void jsonlog_init(const char *path) {
    if (!path) return;
    jf = fopen(path, "w");
    if (jf) {
        setvbuf(jf, jf_buf, _IOFBF, sizeof jf_buf);
    }
}

/* Force a flush — called from the per-frame code path so any buffered
 * events land on SD periodically even if they don't fill the buffer. */
void jsonlog_flush(void) {
    if (jf) fflush(jf);
}

void jsonlog_close(void) {
    if (jf) {
        fclose(jf);
        jf = NULL;
    }
}

static void header(const char *kind) {
    fprintf(jf, "{\"t\":%llu,\"ev\":\"%s\"",
            (unsigned long long)tick_now(), kind);
}

void jsonlog_ev(const char *kind, const char *extra_fmt, ...) {
    if (!jf) return;
    header(kind);
    if (extra_fmt && *extra_fmt) {
        fputc(',', jf);
        va_list ap;
        va_start(ap, extra_fmt);
        vfprintf(jf, extra_fmt, ap);
        va_end(ap);
    }
    fputs("}\n", jf);
}

void jsonlog_ev_bytes(const char *kind, const uint8_t *buf, uint32_t len,
                      uint16_t pc) {
    if (!jf || len == 0) return;
    header(kind);
    fputs(",\"bytes\":[", jf);
    for (uint32_t i = 0; i < len; i++)
        fprintf(jf, "%s\"0x%02X\"", i ? "," : "", buf[i]);
    fprintf(jf, "],\"pc\":\"0x%04X\",\"len\":%lu}\n", pc, (unsigned long)len);
}
