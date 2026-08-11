#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "walker.h"
#include "ir.h"
#include "log.h"
#include "audio.h"
#include "irtrace.h"
#include "jsonlog.h"
#include "irtrace.h"
#include "ir_test.h"
#include "ui.h"
#include "pokeicon.h"
#include "species_names.h"

/* ---- Pedometer --------------------------------------------------------- */

/* Uses the 3DS MCU's built-in step counter (PTM), which runs regardless of
 * lid state. Each frame we read the total and inject any new steps, covering
 * both open-lid walking and closed-lid sleep in one mechanism. */
static u32  lastPtmSteps     = 0;   /* last PTM reading */
static u32  ptmStepsInjected = 0;   /* total steps injected into emulation */

static void sleepWakeCallback(APT_HookType hook, void *param) {
    (void)param;
    if (hook == APTHOOK_ONSLEEP) {
        saveEeprom();
        LOG("SLEEP: EEPROM saved");
    }
    else if (hook == APTHOOK_ONWAKEUP) {
        LOG("WAKE");
    }
}

/* ---- Timing constants --------------------------------------------------- */

/* svcGetSystemTick() runs at exactly 268,111,856 Hz (constant, unaffected by
 * N3DS overclock). The PokeWalker H8 CPU runs at 3,686,400 Hz.
 *
 * The ratio 268111856 / 3686400 = 72.750… is not an integer, so to avoid
 * floating-point we split it into an integer part (72 ticks/cycle) and a
 * fractional remainder accumulated over the denominator. Over time this is
 * exact with zero drift.
 */
#define SYSTICK_HZ          268111856ULL
#define H8_HZ               3686400ULL
#define TICKS_PER_CYCLE_INT (SYSTICK_HZ / H8_HZ)           /* 72 */
#define TICKS_PER_CYCLE_REM (SYSTICK_HZ % H8_HZ)           /* fractional part */

/* RTC fires every quarter-second → 4 frames/sec */
#define TICKS_PER_SEC       4
#define CYCLES_PER_FRAME    (SYSTEM_CLOCK_CYCLES_PER_SECOND / TICKS_PER_SEC) /* 921600 */

/* How many H8 cycles to run in each batch before checking the real clock.
 * 64 cycles is about 17 us of emulated time — a balance between
 * svcGetSystemTick overhead and pacing granularity. */
#define BATCH_CYCLES        64

/* Input is polled every this many instructions (not cycles) inside the
 * emulation loop.  8000 instructions ≈ every ~4 ms at H8 speed.            */
#define INPUT_POLL_INTERVAL 8000

/* ---- Display ------------------------------------------------------------ */

/* Walker LCD upscaled to the 400x240 top screen via citro2d. Scale of 3
 * gives a 288x192 quad centered with 56/24 px margins. */
#define LCD_SCALE         3
#define LCD_SCREEN_X      ((TOP_W - LCD_TEX_W * LCD_SCALE) / 2)   /* 56 */
#define LCD_SCREEN_Y      ((TOP_H - LCD_TEX_H * LCD_SCALE) / 2)   /* 24 */

static uint32_t videoBuffer[LCD_WIDTH * LCD_HEIGHT];

/* ---- Logging ------------------------------------------------------------ */

FILE *logFile;  /* global — shared via log.h */

/* File-based logging disabled — LOG(...) becomes a no-op when logFile==NULL.
 * jsonlog_ev(...) behaves the same way when its file handle is NULL. */
static void logOpen(void)  { logFile = NULL; }
static void logClose(void) { /* nothing to close */ }

/* ---- Tick ↔ H8 cycle conversion ---------------------------------------- */

/* Convert an absolute H8 cycle count to system ticks: ticks = cycles *
 * 268111856 / 3686400. Split into integer and remainder parts to avoid the
 * u64 overflow a direct multiplication would hit around ~67 billion cycles.
 * Since TICKS_PER_CYCLE_REM < H8_HZ this stays exact for practical run
 * lengths. Pure function — no side effects. */
static inline u64 cyclesToTick(u64 cycles) {
    u64 intPart = cycles * TICKS_PER_CYCLE_INT;
    u64 remPart = cycles * TICKS_PER_CYCLE_REM / H8_HZ;
    return intPart + remPart;
}

/* ---- Main loop ---------------------------------------------------------- */

int main(int argc, char *argv[])
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    hidScanInput();
    if (hidKeysHeld() & KEY_SELECT) {
        ir_test_run();
        gfxExit();
        return 0;
    }

    /* Switch from the ir_test console to citro2d. ir_test_run() above
     * took the SELECT-held boot path; if we're here the user wants the
     * normal GPU-rendered emulator UI. */
    gfxExit();
    osSetSpeedupEnable(true);
    if (!ui_init()) {
        /* citro2d / gfx init failed: fall back to a console so the user
         * can at least see the error message and quit cleanly. */
        gfxInitDefault();
        consoleInit(GFX_TOP, NULL);
        printf("\x1b[1;1HERROR: UI init failed.\n");
        printf("\nPress START to quit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        gfxExit();
        return 1;
    }
    if (!lcd_tex_init()) {
        ui_fini();
        return 1;
    }

    /* Box icon atlas — optional. If the data files aren't on the SD the
     * companion panel just renders without a sprite. */
    if (!pokeicon_init()) {
        LOG("pokeicon: not loaded — bottom UI will skip the species sprite");
    }

    /* Enable pedometer (PTM counts steps during sleep via MCU) */
    ptmuInit();

    /* Hook sleep/wake for pedometer mode */
    static aptHookCookie sleepHookCookie;
    aptHook(&sleepHookCookie, sleepWakeCallback, NULL);

    chdir("sdmc:/3ds/pokeStride");

    logOpen();
    /* DEBUG BUILD: captura el intercambio IR para diagnosticar el pairing.
     * Escribe sdmc:/3ds/pokeStride/ir_events.jsonl, truncándolo en cada
     * arranque (fopen "w") para que el log sea siempre el del run actual.
     * Los eventos se acumulan en RAM y solo bajan a SD en frames sin IR;
     * ver jsonlog.c para por qué escribir durante la sesión la rompía. */
    jsonlog_init("ir_events.jsonl");
    LOG("=== PokeStride started ===");
    LOG("SYSTICK_HZ=%llu  H8_HZ=%llu  TICKS_PER_CYCLE_INT=%llu  REM=%llu",
        SYSTICK_HZ, H8_HZ, TICKS_PER_CYCLE_INT, TICKS_PER_CYCLE_REM);
    LOG("CYCLES_PER_FRAME=%lu  BATCH_CYCLES=%u", (unsigned long)CYCLES_PER_FRAME, BATCH_CYCLES);
    jsonlog_ev("start",
               "\"systick_hz\":%llu,\"h8_hz\":%llu,\"cycles_per_frame\":%lu",
               SYSTICK_HZ, H8_HZ, (unsigned long)CYCLES_PER_FRAME);

    initWalker();
    ir_init();
    audioInit();
    irTraceInit();
    PTMU_GetTotalStepCount(&lastPtmSteps);
    LOG("walker + IR init done, PTM steps=%lu", (unsigned long)lastPtmSteps);

    uint64_t cycleCount   = 0;
    uint64_t frameCycles  = 0;    /* cycles into current quarter-second frame */
    u32      frameNumber  = 0;
    u64      tickEpoch    = svcGetSystemTick();  /* real-time reference point */
    u64      totalCycles  = 0;                   /* absolute H8 cycles since start */

    /* Stats per frame */
    u64  frameStartTick = tickEpoch;
    u32  batchesThisFrame = 0;
    u64  spinTicks = 0;   /* ticks spent spin-waiting */

    LOG("tickEpoch=%llu  entering main loop", tickEpoch);

    /* Touch button hitboxes in bottom-screen pixels (320x240). These must
     * match the rectangles drawn each frame below; named constants keep the
     * layout in sync. */
    struct rect { int x0, y0, x1, y1; };
    const struct rect touch_btn_left  = {  20, 130, 100, 180 };
    const struct rect touch_btn_a     = { 120, 130, 200, 180 };
    const struct rect touch_btn_right = { 220, 130, 300, 180 };
    const struct rect touch_btn_save  = {  50, 195, 150, 230 };
    const struct rect touch_btn_exit  = { 170, 195, 270, 230 };

    /* Light flash on press, decays after this many frames (~250ms). */
    int flash_left = 0, flash_a = 0, flash_right = 0;
    int flash_save = 0, flash_exit = 0;
    bool quit_requested = false;

    #define IN_RECT(t, r) \
        ((t).px >= (r).x0 && (t).px < (r).x1 && \
         (t).py >= (r).y0 && (t).py < (r).y1)

    while (aptMainLoop()) {
        /* Check quit + walker input — physical buttons */
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;
        if (kDown & KEY_A)     setKeys(ENTER);
        if (kDown & KEY_LEFT)  setKeys(LEFT);
        if (kDown & KEY_RIGHT) setKeys(RIGHT);

        /* Touch buttons — same logical actions as the physical buttons,
         * plus SAVE and EXIT. We track the rising edge of touch ourselves
         * with `prev_touch_held` because the inner emulation loop also
         * calls hidScanInput() every ~4 ms, which would otherwise consume
         * the KEY_TOUCH down-edge before this outer check runs. Reading
         * hidKeysHeld() keeps the held state visible regardless of which
         * hidScanInput call sampled the start of the touch. */
        u32 kHeld = hidKeysHeld();
        static bool prev_touch_held = false;
        if ((kHeld & KEY_TOUCH) && !prev_touch_held) {
            touchPosition touch;
            hidTouchRead(&touch);
            if      (IN_RECT(touch, touch_btn_left))  { setKeys(LEFT);    flash_left  = 2; }
            else if (IN_RECT(touch, touch_btn_a))     { setKeys(ENTER);   flash_a     = 2; }
            else if (IN_RECT(touch, touch_btn_right)) { setKeys(RIGHT);   flash_right = 2; }
            else if (IN_RECT(touch, touch_btn_save))  { saveEeprom();    flash_save  = 2; }
            else if (IN_RECT(touch, touch_btn_exit))  { quit_requested = true; flash_exit = 2; }
        }
        prev_touch_held = (kHeld & KEY_TOUCH) != 0;

        /* Mirror physical-button presses onto the touch UI for visual
         * feedback when the user uses the d-pad / A. */
        if (kDown & KEY_LEFT)  flash_left  = 2;
        if (kDown & KEY_A)     flash_a     = 2;
        if (kDown & KEY_RIGHT) flash_right = 2;
        if (quit_requested) break;

        /* ---- Cycle-accurate emulation for one quarter-second frame ----
         *
         * We run BATCH_CYCLES H8 cycles at a time, then spin-wait until
         * real time catches up.  This distributes cycles evenly across
         * the 250 ms frame instead of executing them in a burst.
         */
        int  error        = 0;
        int  inputCounter = 0;

        batchesThisFrame = 0;
        spinTicks = 0;
        frameStartTick = svcGetSystemTick();

        while (frameCycles < CYCLES_PER_FRAME) {
            /* Run one batch */
            uint64_t batchEnd = frameCycles + BATCH_CYCLES;
            if (batchEnd > CYCLES_PER_FRAME)
                batchEnd = CYCLES_PER_FRAME;

            while (cycleCount < batchEnd) {
                error = runNextInstruction(&cycleCount);
                if (error) break;

                /* Poll input periodically */
                if (++inputCounter >= INPUT_POLL_INTERVAL) {
                    inputCounter = 0;
                    hidScanInput();
                    kDown = hidKeysDown();
                    if (kDown & KEY_START) { error = -1; break; }
                    if (kDown & KEY_A)     setKeys(ENTER);
                    if (kDown & KEY_LEFT)  setKeys(LEFT);
                    if (kDown & KEY_RIGHT) setKeys(RIGHT);
                }
            }
            if (error) break;

            frameCycles = cycleCount;   /* might overshoot batchEnd slightly */
            batchesThisFrame++;

            /* Compensate for any time ir_send blocked during this batch */
            tickEpoch += ir_get_blocked_ticks();

            /* Spin-wait until real time catches up to where we are */
            u64 targetTick = tickEpoch + cyclesToTick(totalCycles + frameCycles);
            u64 spinStart  = svcGetSystemTick();
            while (svcGetSystemTick() < targetTick)
                ;  /* spin */
            spinTicks += svcGetSystemTick() - spinStart;
        }

        if (error == -1) break;  /* START pressed */
        if (error) {
            uint16_t p = getPC();
            LOG("frame %lu: UNIMPL at PC=0x%04X bytes=%02X %02X %02X %02X %02X %02X",
                frameNumber, p,
                readMem(p), readMem(p+1), readMem(p+2), readMem(p+3),
                readMem(p+4), readMem(p+5));
            break;
        }

        /* Frame complete — advance bookkeeping */
        totalCycles += frameCycles;
        frameCycles  = 0;
        cycleCount   = 0;

        /* ---- RTC + Render ------------------------------------------------ *
         *
         * No renderizar si la ROM está a media ráfaga de TX. El frame de GPU
         * usa C3D_FRAME_SYNCDRAW y bloquea ~1 vblank; con el emulador parado
         * ahí, el SC16IS750 vacía su FIFO y el paquete sale PARTIDO. Medido
         * en el aire: 131 B + 26 ms de silencio + 5 B, que el peer lee como
         * dos tramas rotas. Un walker real es aún menos tolerante: enmarca
         * con 122 us de silencio.
         *
         * Tampoco se renderiza si el frame anterior tuvo actividad IR, porque
         * entonces puede estar llegando una respuesta del peer. Ese es el otro
         * lado del mismo problema y mata por RX en vez de por TX: el FIFO del
         * SC16IS750 son 64 B y una respuesta 0x0E son 136 B, así que basta con
         * dejar de vaciarlo 5.5 ms para perder bytes. Medido en el aire: la
         * petición de lectura #5 salió al final de un frame, el peer contestó
         * durante la congelación, y de los 136 B de respuesta entraron
         * exactamente 64 — el FIFO lleno — con los otros 72 perdidos.
         *
         * Saltarse el frame es barato: el LCD del walker va a 4 FPS y una
         * sesión de emparejamiento entera dura ~1 s (4-5 frames). El tope de
         * 20 frames acota la UI congelada a 5 s por si la ROM se queda
         * beaconeando en Conectar, y evita un bloqueo permanente si
         * irInTxMode se colgara. */
        static uint32_t skippedRenders = 0;
        static uint32_t prevTraceHead  = 0;
        uint32_t traceHeadNow = irTraceHead();
        uint32_t irEventsThisFrame = traceHeadNow - prevTraceHead;  /* solo este frame */
        prevTraceHead = traceHeadNow;

        /* Menos de ~64 eventos = beaconeo suelto, cuyos paquetes son de 8 B y
         * caben de sobra en el FIFO; ahí renderizar es inofensivo. A partir de
         * ahí hay transferencia de verdad y cualquier parón cuesta bytes. */
        bool inSession  = ir_tx_busy() || irEventsThisFrame >= 64;
        bool skipRender = inSession && skippedRenders < 240;
        skippedRenders  = skipRender ? skippedRenders + 1 : 0;

        u64 renderStart = svcGetSystemTick();

        quarterRTCInterrupt();   /* el RTC nunca se salta: es el reloj del walker */
        if (!skipRender) {
            fillVideoBuffer(videoBuffer);
            lcd_tex_upload(videoBuffer);  /* CPU 96x64 -> tiled GPU texture */
        }

        /* ---- Audio ------------------------------------------------------- *
         * audioUpdate() is a no-op while audio is disabled in audio.c. The
         * Timer W reads still run so the values can be logged and the call
         * paths stay exercised. */
        {
            static uint16_t lastGra = 0;
            static bool lastTimerOn = false;
            bool timerOn = isTimerWActive();
            uint16_t gra = getTimerWGRA();
            uint8_t vol = getWalkerVolume();
            if (timerOn && gra > 0 && vol > 0) {
                if (gra != lastGra || !lastTimerOn) {
                    audioUpdate(gra, vol, true);
                }
            }
            lastGra = gra;
            lastTimerOn = timerOn;
        }

        /* Compensate: the emulated clock was frozen during render */
        u64 renderTicks = svcGetSystemTick() - renderStart;
        tickEpoch += renderTicks;

        /* ---- Pedometer (PTM step counter) ------------------------------- */
        {
            u32 ptmNow = 0;
            PTMU_GetTotalStepCount(&ptmNow);
            u32 delta = ptmNow - lastPtmSteps;
            if (delta > 0 && delta < 100000) {
                injectSteps(delta);
                ptmStepsInjected += delta;
                LOG("STEP: +%lu (ptm=%lu, injected=%lu)",
                    (unsigned long)delta, (unsigned long)ptmNow,
                    (unsigned long)ptmStepsInjected);
            }
            /* Log PTM value once per second to diagnose lid-open counting */
            if ((frameNumber & 3) == 0 && ptmNow != lastPtmSteps) {
                LOG("PTM changed: %lu -> %lu", (unsigned long)lastPtmSteps, (unsigned long)ptmNow);
            }
            lastPtmSteps = ptmNow;
        }

        /* After a long gap (sleep/resume), resync pacing epoch */
        {
            u64 nowTick = svcGetSystemTick();
            u64 expectedTick = tickEpoch + cyclesToTick(totalCycles);
            s64 gap = (s64)(nowTick - expectedTick);
            if (gap > (s64)(SYSTICK_HZ / 4)) {  /* >250ms gap = probable sleep */
                tickEpoch   = nowTick;
                totalCycles = 0;
                LOG("RESYNC after gap of %lu ms", (unsigned long)(gap / 268112));
            }
        }

        /* ---- Per-frame logging ------------------------------------------ */
        u64 frameEndTick = svcGetSystemTick();
        u64 frameTotalTicks = frameEndTick - frameStartTick;

        /* Convert ticks to µs: ticks * 1000000 / 268111856
         * Approximate to avoid overflow: ticks / 268 ≈ µs */
        u32 frameUs   = (u32)(frameTotalTicks / 268);
        u32 spinUs    = (u32)(spinTicks / 268);
        u32 workUs    = frameUs - spinUs;

        /* Log every 4th frame (once per second) to avoid excessive writes */
        if ((frameNumber & 3) == 0) {
            LOG("frame %lu: total=%lu us  work=%lu us  spin=%lu us  batches=%lu  drift=%lld ticks",
                frameNumber, frameUs, workUs, spinUs, batchesThisFrame,
                (s64)(frameEndTick - (tickEpoch + cyclesToTick(totalCycles))));
        }

        /* Flush deferred IR event counters (safe stack depth here) */
        ir_log_events();

        /* Flush IR protocol trace + poll the RAM variables relevant to an
         * IR connection. Every changed byte is emitted to the JSON event
         * log for later inspection. */
        {
            /* peerRole @ 0xF8BE — 1=INITIATOR 2=SYN_SENT 3=SLAVE 4=CONNECTED */
            uint8_t role = readMem(0xF8BE);
            static uint8_t lastRole = 0xFF;
            if (role != lastRole) {
                irTracePush(IR_TRACE_ROLE, role, 0, 0);
                jsonlog_ev("ir_role",
                           "\"old\":%u,\"new\":%u", lastRole, role);
                lastRole = role;
            }

            /* commsErrorId @ 0xF7AD — 0 ok / 1 no response / 2 cannot connect
             * / 3 cannot complete / 4 sync error / 5 already received /
             * 6 already played / 7 could not receive / 8 buf overflow */
            uint8_t err = readMem(0xF7AD);
            static uint8_t lastErr = 0;
            if (err != lastErr) {
                jsonlog_ev("ir_err",
                           "\"old\":%u,\"new\":%u", lastErr, err);
                lastErr = err;
            }

            /* sessionId @ 0xF8BA (4B BE) — negotiated after handshake */
            uint32_t sess = ((uint32_t)readMem(0xF8BA) << 24) |
                            ((uint32_t)readMem(0xF8BB) << 16) |
                            ((uint32_t)readMem(0xF8BC) <<  8) |
                            ((uint32_t)readMem(0xF8BD));
            static uint32_t lastSess = 0;
            if (sess != lastSess) {
                jsonlog_ev("ir_session",
                           "\"session\":\"0x%08lX\",\"old\":\"0x%08lX\"",
                           (unsigned long)sess, (unsigned long)lastSess);
                lastSess = sess;
            }

            /* localNonce @ 0xF8B6 (4B BE) — our half of the XOR */
            uint32_t nonce = ((uint32_t)readMem(0xF8B6) << 24) |
                             ((uint32_t)readMem(0xF8B7) << 16) |
                             ((uint32_t)readMem(0xF8B8) <<  8) |
                             ((uint32_t)readMem(0xF8B9));
            static uint32_t lastNonce = 0;
            if (nonce != lastNonce) {
                jsonlog_ev("ir_nonce",
                           "\"nonce\":\"0x%08lX\"", (unsigned long)nonce);
                lastNonce = nonce;
            }

            /* crcRetryCount @ 0xF8C2 */
            uint8_t crcRetry = readMem(0xF8C2);
            static uint8_t lastCrcRetry = 0;
            if (crcRetry != lastCrcRetry) {
                jsonlog_ev("ir_crc_retry",
                           "\"old\":%u,\"new\":%u",
                           lastCrcRetry, crcRetry);
                lastCrcRetry = crcRetry;
            }

            /* gotPacketFlag @ 0xF8C3 — bit 0 set when any valid packet seen */
            uint8_t gotPkt = readMem(0xF8C3);
            static uint8_t lastGotPkt = 0;
            if (gotPkt != lastGotPkt) {
                jsonlog_ev("ir_got_packet",
                           "\"old\":%u,\"new\":%u",
                           lastGotPkt, gotPkt);
                lastGotPkt = gotPkt;
            }

            /* lastRxedCmd @ 0xF8C4 — last command byte successfully dispatched */
            uint8_t lastCmd = readMem(0xF8C4);
            static uint8_t lastLastCmd = 0xFF;
            if (lastCmd != lastLastCmd) {
                jsonlog_ev("ir_last_cmd",
                           "\"cmd\":\"0x%02X\"", lastCmd);
                lastLastCmd = lastCmd;
            }

            /* peerPlayStage @ 0xF8C5 */
            uint8_t stage = readMem(0xF8C5);
            static uint8_t lastStage = 0;
            if (stage != lastStage) {
                jsonlog_ev("ir_peer_stage",
                           "\"old\":%u,\"new\":%u", lastStage, stage);
                lastStage = stage;
            }

            /* numBytesRxed @ 0xF7BA — 0 means packet just processed / fresh */
            /* Not polled per-frame (too noisy); included in other events. */

            /* currentEventLoopFunc @ 0xF7E0 — which micro-app is running.
             * 0x08D6 = irCommsEventLoop, 0x7882 = sleepModeEventLoop, etc. */
            uint16_t evtLoop = ((uint16_t)readMem(0xF7E0) << 8) |
                                (uint16_t)readMem(0xF7E1);
            static uint16_t lastEvtLoop = 0;
            if (evtLoop != lastEvtLoop) {
                jsonlog_ev("event_loop",
                           "\"old\":\"0x%04X\",\"new\":\"0x%04X\"",
                           lastEvtLoop, evtLoop);
                lastEvtLoop = evtLoop;
            }

            /* Echo/RX diagnostics: raw bytes off the FIFO, how many were
             * stripped as our own echo, and the outstanding echo debt. This
             * answers whether the master's post-SYN-ACK stream (10-02) is
             * arriving at all and whether echo cancellation is eating it. */
            u32 rawrx = 0, stripped = 0, owed = 0;
            ir_get_rx_stats(&rawrx, &stripped, &owed);
            if (rawrx || stripped || owed) {
                jsonlog_ev("ir_rx_stats",
                           "\"raw\":%lu,\"stripped\":%lu,\"owed\":%lu",
                           (unsigned long)rawrx, (unsigned long)stripped,
                           (unsigned long)owed);
            }
        }
        /* Aquí NO se formatea ni se escribe nada. Las trazas IR se quedan en
         * crudo en el ring y se vuelcan enteras al salir (ver irtrace.h): un
         * frame de la fase de lectura son ~800 eventos y formatearlos cuesta
         * ~7 ms, tiempo de sobra para que el FIFO de 64 B del SC16IS750 se
         * desborde con una respuesta de 136 B. Medido en el aire: llegaron 113
         * de 136. Los eventos sueltos (sesión, rol, error, stats) sí se emiten
         * al vuelo, pero son uno o dos por frame y van a un buffer de RAM. */
        if (!inSession) {
            if (logFile) fflush(logFile);
            jsonlog_flush();
        }

        /* Bajar el log a la SD SOLO en frames sin actividad IR.
         *
         * Un write a SD congela el emulador cientos de ms (medido: frames con
         * tráfico IR tardaban 640-720 ms en vez de 250). El SC16IS750 no se
         * congela con él: sigue vaciando su FIFO y se queda mudo a mitad de
         * paquete, así que el peer recibía tramas partidas y truncadas. Es
         * decir, instrumentar la sesión era lo que la rompía. Los eventos se
         * acumulan en RAM (ver jsonlog.c) y aterrizan en disco en cuanto el
         * enlace queda en silencio — típicamente el frame siguiente al final
         * de la sesión, así que el post-mortem sigue completo. */
        if (irEventsThisFrame == 0) {
            if (logFile) fflush(logFile);
            jsonlog_flush();
        }

        /* ---- GPU frame: walker LCD on top, companion UI on bottom -------*
         * Se salta entero si hay una ráfaga TX en vuelo (ver arriba). */
        if (!skipRender) {
            uint32_t today_steps    = getWalkerSteps();         /* RAM 0xF79C */
            uint32_t lifetime_steps = getWalkerLifetimeSteps(); /* RAM 0xF780 */
            uint16_t watts          = getWalkerWatts();

            uint16_t species = getWalkedSpecies();
            uint8_t  level   = getWalkedLevel();
            uint8_t  form    = getWalkedForm();
            uint8_t  sex     = getWalkedSex();
            bool     shiny   = getWalkedShiny();

            frame_begin();

            /* --- Top screen: dark background + walker LCD centered ----- */
            scene_top();
            lcd_tex_draw((float)LCD_SCREEN_X, (float)LCD_SCREEN_Y,
                         (float)LCD_SCALE);
            /* Subtle frame around the LCD so it doesn't look pasted on. */
            draw_frame((float)LCD_SCREEN_X - 2.0f,
                       (float)LCD_SCREEN_Y - 2.0f,
                       LCD_TEX_W * LCD_SCALE + 4.0f,
                       LCD_TEX_H * LCD_SCALE + 4.0f,
                       1.5f, COL_DIM);

            /* --- Bottom screen: companion + touch UI ------------------- */
            scene_bottom();

            /* Title bar */
            draw_rect(0, 0, BOT_W, 24, COL_TITLEBG);
            draw_text_centered(BOT_W * 0.5f, 5, 0.62f, COL_TITLEFG,
                                "PokeStride");

            /* Companion strip (32 - 96): sprite on left, name + stats on
             * right. The 32x32 box icon is drawn at 2x = 64x64. */
            draw_rect(8, 30, 304, 68, COL_BTN_BG);
            draw_frame(8, 30, 304, 68, 1.0f, COL_BTN_BORDER);

            /* Sprite slot — falls back to a placeholder block if pokeicon
             * isn't loaded or species is out of range. */
            C2D_Image sprite;
            if (pokeicon_loaded() &&
                species != 0 && pokeicon_get(species, &sprite)) {
                C2D_DrawImageAt(sprite, 14.0f, 32.0f, 0.5f, NULL, 2.0f, 2.0f);
            } else {
                /* Empty slot frame so the layout still looks intentional. */
                draw_frame(14, 32, 64, 64, 1.0f, COL_DIM);
                draw_text_centered(46, 56, 0.45f, COL_DIM, "??");
            }

            /* Right block: species name (or "(none)") in accent, then
             * Lv.NN + form/sex/shiny indicators, then Today + Watts. */
            const char *name = species ? species_name(species) : "(none)";
            if (!name) name = "(unknown)";
            draw_text(88, 34, 0.60f, COL_ACCENT, name);

            {
                char meta[40];
                int n = snprintf(meta, sizeof(meta), "Lv. %u", level ? level : 0);
                if (form > 0)  n += snprintf(meta + n, sizeof(meta) - n,
                                              "  form %u", form);
                if (sex == 0)  snprintf(meta + n, sizeof(meta) - n, "  M");
                else if (sex == 1) snprintf(meta + n, sizeof(meta) - n, "  F");
                draw_text(88, 52, 0.45f, shiny ? COL_OK : COL_DIM, meta);
            }

            draw_textf(88, 70, 0.45f, COL_TEXT,
                       "Today  %lu", (unsigned long)today_steps);
            draw_textf(88, 84, 0.45f, COL_TEXT,
                       "Watts  %u", watts);

            /* Total panel — lifetime steps in one wide row. */
            draw_rect(8, 104, 304, 22, COL_BTN_BG);
            draw_frame(8, 104, 304, 22, 1.0f, COL_BTN_BORDER);
            draw_textf(BOT_W * 0.5f - 70.0f, 109, 0.50f, COL_TEXT,
                       "Total  %lu steps", (unsigned long)lifetime_steps);

            /* D-pad / A touch row */
            ui_button(touch_btn_left.x0,  touch_btn_left.y0,
                       touch_btn_left.x1 - touch_btn_left.x0,
                       touch_btn_left.y1 - touch_btn_left.y0,
                       "<-",  flash_left  > 0);
            ui_button(touch_btn_a.x0,     touch_btn_a.y0,
                       touch_btn_a.x1 - touch_btn_a.x0,
                       touch_btn_a.y1 - touch_btn_a.y0,
                       "A",   flash_a     > 0);
            ui_button(touch_btn_right.x0, touch_btn_right.y0,
                       touch_btn_right.x1 - touch_btn_right.x0,
                       touch_btn_right.y1 - touch_btn_right.y0,
                       "->",  flash_right > 0);

            /* Save / Exit row */
            ui_button(touch_btn_save.x0, touch_btn_save.y0,
                       touch_btn_save.x1 - touch_btn_save.x0,
                       touch_btn_save.y1 - touch_btn_save.y0,
                       "SAVE", flash_save > 0);
            ui_button(touch_btn_exit.x0, touch_btn_exit.y0,
                       touch_btn_exit.x1 - touch_btn_exit.x0,
                       touch_btn_exit.y1 - touch_btn_exit.y0,
                       "EXIT", flash_exit > 0);

            /* Hint footer */
            draw_text_centered(BOT_W * 0.5f, 232, 0.42f, COL_DIM,
                                "Tap a button or use the D-pad.");

            frame_end();

            /* Decay flash timers one frame closer to off. */
            if (flash_left  > 0) flash_left--;
            if (flash_a     > 0) flash_a--;
            if (flash_right > 0) flash_right--;
            if (flash_save  > 0) flash_save--;
            if (flash_exit  > 0) flash_exit--;
        }

        frameNumber++;

        /* Autosave EEPROM every 60 seconds (240 frames at 4 FPS) */
        if ((frameNumber % 240) == 0) {
            saveEeprom();
        }

        /* (resync is handled above in the pedometer section) */
        {
        }
    }

    LOG("=== PokeStride exiting at frame %lu, totalCycles=%llu ===", frameNumber, totalCycles);
    {
        int rc = saveEeprom();
        LOG("EEPROM save: %s (rc=%d)", rc == 0 ? "OK" : "FAILED", rc);
    }
    jsonlog_ev("end", "\"frame\":%lu,\"cycles\":%llu",
               (unsigned long)frameNumber, (unsigned long long)totalCycles);

    /* Volcado de las trazas IR: se acumularon en crudo durante todo el run
     * precisamente para no formatear nada mientras el enlace estaba vivo.
     * Aquí ya no hay sesión que romper, así que se formatea todo de golpe y
     * directo al fichero (~8 us por evento; un run de 40 s son ~0.1 s). */
    jsonlog_set_direct();
    {
        uint32_t n = irTraceFlush();
        LOG("IR trace: %lu eventos volcados", (unsigned long)n);
    }
    jsonlog_close();
    logClose();

    audioExit();
    ptmuExit();
    ir_deinit();
    pokeicon_fini();
    ui_fini();
    return 0;
}
