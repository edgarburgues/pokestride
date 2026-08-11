#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <memory.h>
#include <string.h>
#include <assert.h>

#include "definitions.h"
#include "walker.h"
#include "queue.h"
#include "utils.c"
#include "regRef.h"

#ifdef __3DS__
#include "ir.h"
#include "irtrace.h"
#endif

// Walker variables
static struct Queue inputQueue;
static uint8_t quartersEllapsed;
static uint64_t subClockCyclesEllapsed;
static struct TimerB_t TimerB;
static struct TimerW_t TimerW;
static uint8_t* CKSTPR1; // Clock halt register 1
static uint8_t* CKSTPR2; // Clock halt register 2
static struct Flags_t flags;
static uint16_t pc;
static uint8_t* IRQ_IENR1; // Interrupt enable register 1
static uint8_t* IRQ_IENR2; // Interrupt enable register 2
static uint8_t* IRQ_IRR1; // Interrupt flag register 1
static uint8_t* IRQ_IRR2; // Interrupt flag register 2
static uint8_t* RTCFLG; // RTC Interrupt Flag Register
/* Interrupt context save. PC+CCR are restored from saved state at RTE rather
 * than from the guest stack, so a LIFO of save slots is needed to support
 * nested/re-entrant interrupts (each ISR preserves its own return point). */
#define INTERRUPT_SAVE_DEPTH 8
static uint16_t interruptSavedAddressStack[INTERRUPT_SAVE_DEPTH];
static struct Flags_t interruptSavedFlagsStack[INTERRUPT_SAVE_DEPTH];
static int interruptSaveDepth;
/* top-of-stack mirror */
static uint16_t interruptSavedAddress;
static struct Flags_t interruptSavedFlags;

/* Push the current (pc, flags) as an interrupt return context. Call at the
 * moment of delivery, BEFORE setting flags.I, so the saved CCR reflects the
 * interrupted context. */
static inline void interruptPushContext(void){
	if (interruptSaveDepth < INTERRUPT_SAVE_DEPTH){
		interruptSavedAddressStack[interruptSaveDepth] = pc;
		interruptSavedFlagsStack[interruptSaveDepth]   = flags;
		interruptSaveDepth++;
	}
	/* mirror for inspection */
	interruptSavedAddress = pc;
	interruptSavedFlags   = flags;
}

/* Restore the most recently saved interrupt context (used by RTE). */
static inline void interruptPopContext(void){
	if (interruptSaveDepth > 0){
		interruptSaveDepth--;
		pc    = interruptSavedAddressStack[interruptSaveDepth] - 2;
		flags = interruptSavedFlagsStack[interruptSaveDepth];
	} else {
		/* underflow: fall back to the single-slot mirror */
		pc    = interruptSavedAddress - 2;
		flags = interruptSavedFlags;
	}
	if (interruptSaveDepth > 0){
		interruptSavedAddress = interruptSavedAddressStack[interruptSaveDepth - 1];
		interruptSavedFlags   = interruptSavedFlagsStack[interruptSaveDepth - 1];
	}
}
static uint32_t* ER[8]; // General purpose registers
static uint16_t* R[8];
static uint16_t* E[8];
static uint8_t* RL[8];
static uint8_t* RH[8];
static uint32_t* SP;
static uint8_t* memory;
static struct SSU_t SSU;
static struct Accelerometer_t accel;
static struct Eeprom_t eeprom;
static struct Lcd_t lcd;
static struct SCI3_t SCI3;
static bool sleep;
static int entry;  /* ROM entry point — read from reset vector */

/* PC of ui_keypoll_main for an alternate firmware build (entry==0x0080),
 * resolved from the ELF symbol table at startup so the button-inject hook
 * survives rebuilds that move the function. 0 = unresolved/disabled. */
uint32_t walkerV2KeypollPC = 0;

/* H8/300H Tiny flag-clear protocol ("read-1-then-write-0"): writing 0 to a
 * status flag clears it only if the last read of the register returned that
 * flag as 1. This protects firmware read-modify-write sequences (read SSR3 →
 * write back sr&mask) against bytes that arrive between the read and the write:
 * the new RDRF survives the write-back because the read latch saw it as 0.
 * lastReadSSR3 is consumed by each write (a fresh read is required before the
 * next clear), mirroring the per-access latch. */
static uint8_t lastReadSSR3 = 0;

/* Draw-call trace (debug-only, opt-in). When enabled, each time the CPU enters
 * lcd_draw_image (the blit primitive) the unpacked rectangle + source + caller
 * are logged, so a frame's draw sequence can be enumerated without a step-over
 * debugger. Enable via env POKESTRIDE_DRAW_TRACE (PWDBG_DRAW_TRACE also honored) (=1 uses the default PC, or =0xNNNN
 * to set the PC). drawTracePC defaults to 0x80ac (lcd_draw_image address varies
 * between firmware builds; pass the right address explicitly otherwise). Pure
 * observability: does not change any executed semantics. */
bool     drawTraceEnabled = false;
uint16_t drawTracePC       = 0x80ac;
/* PC of the instruction that last wrote the LCD SSTDR (0xF0EB). Captured so the
 * raw-SSU footer trace can name the routine writing pixels directly (bypassing
 * lcd_draw_image). Only maintained while drawTraceEnabled. */
static uint16_t lcdSstdrWriterPC = 0;
void walkerSetDrawTrace(int on, unsigned pcOpt){
	drawTraceEnabled = on ? true : false;
	if (pcOpt) drawTracePC = (uint16_t)pcOpt;
}

// Audio event latch: set when Timer W activates, consumed by audioUpdate
static bool audioEventPending = false;
static uint16_t audioEventGRA = 0;

// Last PC before instruction execution (for error logging)
static uint16_t lastExecPC = 0;

// Debug anchor — locatable in memory by an external inspector via magic bytes
struct DebugAnchor {
	uint32_t magic1;        // 0x504B5354 "PKST"
	uint32_t magic2;        // 0x524C4421 "RLD!"
	uint8_t* memory;        // → H8 64KB memory (SCI3 regs at 0xFF98-0xFF9D)
	uint16_t* pc;           // → H8 program counter
	struct SCI3_t* sci3;    // → SCI3 state (txBuf, rxBuf, lengths)
	struct Flags_t* flags;  // → CPU flags
	uint32_t** ER;          // → ER0-ER7 register pointers
};
static volatile struct DebugAnchor debugAnchor;

uint8_t clearBit8(uint8_t operand, int bit){
	return operand & ~(1 << bit);			
}

static inline struct RegRef8 getRegRef8(uint8_t operand){
	struct RegRef8 newRef;
	newRef.idx = operand & 0b0111;
	newRef.loOrHiReg = (operand & 0b1000) ? 'l' : 'h';
	newRef.ptr = (newRef.loOrHiReg == 'l') ? RL[newRef.idx] : RH[newRef.idx];
	return newRef;
}

static inline struct RegRef16 getRegRef16(uint8_t operand){
	struct RegRef16 newRef;
	newRef.idx = operand & 0b0111;
	newRef.loOrHiReg = (operand & 0b1000) ? 'e' : 'r';
	newRef.ptr = (newRef.loOrHiReg == 'r') ? R[newRef.idx] : E[newRef.idx];
	return newRef;
}

static inline struct RegRef32 getRegRef32(uint8_t operand){
	struct RegRef32 newRef;
	newRef.idx = operand & 0b0111;
	newRef.ptr = ER[newRef.idx];
	return newRef;
}

void printRegistersState(){
#ifdef PRINT_STATE
	for(int i=0; i < 8; i++){
		printf("ER%d: [0x%08X], ", i, *ER[i]); 
	}
	printf("\n");
	printf("I: %d, H: %d, N: %d, Z: %d, V: %d, C: %d ", flags.I, flags.H, flags.N, flags.Z, flags.V, flags.C);
	printf("\n\n");
#endif

}

void printMemory(uint32_t address, int byteCount){
#ifdef PRINT_STATE
	address = address & 0x0000ffff; // Keep lower 16 bits only
	for(int i = 0; i < byteCount; i++){ 
		printf("MEMORY - 0x%04x -> %02x\n", address + i, memory[address + i]);
	}
#endif
}

void printInstruction(const char* format, ...){
#ifdef PRINT_STATE
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
#endif
}

void setFlags(uint8_t value){
	flags.C = value & (1<<0);
	flags.V = value & (1<<1);
	flags.Z = value & (1<<2);
	flags.N = value & (1<<3);
	flags.U = value & (1<<4);
	flags.H = value & (1<<5);
	flags.UI = value & (1<<6);
	flags.I = value & (1<<7);
}

/* Pack CCR (the flag struct) back into its H8 byte form. Inverse of setFlags;
 * used by STC ccr,Rd and the ANDC/ORC/XORC immediate-CCR ops. */
uint8_t packFlags(void){
	return (uint8_t)(
		(flags.C  ? (1<<0) : 0) |
		(flags.V  ? (1<<1) : 0) |
		(flags.Z  ? (1<<2) : 0) |
		(flags.N  ? (1<<3) : 0) |
		(flags.U  ? (1<<4) : 0) |
		(flags.H  ? (1<<5) : 0) |
		(flags.UI ? (1<<6) : 0) |
		(flags.I  ? (1<<7) : 0));
}

void fillVideoBuffer(uint32_t* videoBuffer){
	/* The SSD1854 GDDRAM is 16 pages (128 rows) tall: two stacked 64-row
	 * halves. The panel scans out 64 rows starting at the display-start-line
	 * (wrapping mod 128), set by the 0x40 command. start-line 0 shows rows
	 * 0–63 (pages 0–7); start-line 64 shows rows 64–127 (pages 8–15).
	 *
	 * If the ROM has issued a 0x40, honour it. Otherwise fall back to a
	 * free-running half toggle (used by firmware that never sets the
	 * start line). */
	int startRow;
	if (lcd.startLineActive) {
		startRow = lcd.displayStartLine & 0x7F;     /* mod 128 */
	} else {
		startRow = lcd.currentBuffer ? 64 : 0;      /* half 0 or 1 */
		lcd.currentBuffer ^= 1;
	}
	for(int y = 0; y < LCD_HEIGHT; y++){
		const int physRow = (startRow + y) & 0x7F;  /* wrap mod 128 */
		const int yBit  = physRow & 7;              /* row within page */
		const int yPage = physRow >> 3;             /* GDDRAM page 0..15 */
		const int rowOff = yPage * LCD_WIDTH * LCD_BYTES_PER_STRIPE;
		const uint8_t mask = (uint8_t)(1 << yBit);
		for(int x = 0; x < LCD_WIDTH; x++){
			int base = rowOff + 2*x;
			uint8_t firstBit  = (lcd.memory[base    ] & mask) >> yBit;
			uint8_t secondBit = (lcd.memory[base + 1] & mask) >> yBit;
			videoBuffer[y*LCD_WIDTH + x] = palette[(firstBit << 1) | secondBit];
		}
	}
}

// With masking here we're ignoring the 0x00XX0000 part of the address for this emulator, as we have one big memory block that goes up to 0xFFFF
void setMemory8(uint32_t address, uint8_t value){
	address = address & 0x0000ffff; // Keep lower 16 bits only

	/* Draw trace: remember which instruction wrote the LCD data/command byte
	 * (SSTDR = 0xF0EB), so a footer write can be attributed to its caller. */
	if (drawTraceEnabled && address == 0xF0EB) lcdSstdrWriterPC = pc;

#ifdef __3DS__
	// SCR3 write — detect TE/RE edges immediately (not via polling)
	if (address == 0xFF9A) {
		uint8_t prev = memory[address];
		memory[address] = value;
		// TE rising edge → prepare SC16IS750 for streaming TX
		if (!(prev & SCI3_TE) && (value & SCI3_TE)) {
			irTracePush(IR_TRACE_TE_ON, 0, pc, 0);
			ir_tx_start();
		}
		// TE falling edge → wait for SC16IS750 to finish, disable TX
		if ((prev & SCI3_TE) && !(value & SCI3_TE)) {
			irTracePush(IR_TRACE_TE_OFF, 0, pc, 0);
			ir_tx_end();
		}
		// RE rising edge → start RX. Flush the hardware FIFO first: bytes
		// captured while RE was off (stale peer beacons piling up between
		// connection attempts) were never receivable on real hardware and
		// must not be replayed into the ROM as fresh traffic.
		if (!(prev & SCI3_RE) && (value & SCI3_RE)) {
			irTracePush(IR_TRACE_RE_ON, 0, pc, 0);
			ir_rx_flush();
			ir_recv_start();
			SCI3.rxLen = 0;
			SCI3.rxPos = 0;
		}
		// RE falling edge → stop RX
		if ((prev & SCI3_RE) && !(value & SCI3_RE)) {
			irTracePush(IR_TRACE_RE_OFF, 0, pc, 0);
			ir_recv_stop();
		}
		return;
	}
	// SSR3 write — read-1-then-write-0 flag clearing (H8/300H Tiny). A
	// status flag (bits 2-7: TEND/PER/FER/OER/RDRF/TDRE) is cleared by
	// writing 0 ONLY if the guest's last SSR3 read returned it as 1; flags
	// that got set after that read survive the write-back (this is what
	// makes the ROM's `sr = SSR3; SSR3 = sr & mask` drain pattern safe on
	// hardware when a byte lands between the read and the write). Writing
	// 1 never sets a flag (hardware-set only). Bits 0-1 (MPBT/MPB) are
	// plain read/write. The read latch is consumed by the write.
	if (address == 0xFF9C) {
		uint8_t old = memory[address];
		uint8_t clearable = (uint8_t)(~value) & lastReadSSR3 & 0xFCu;
		memory[address] = (uint8_t)(((old & 0xFCu) & ~clearable) | (value & 0x03u));
		lastReadSSR3 = 0;
		return;
	}
	// TDR3 write — accept byte into emulated SCI3 shift register.
	// TDRE clears immediately; it will be re-set after 320 cycles
	// (one byte time at 115200 baud) when the countdown fires,
	// at which point the byte is also sent to the SC16IS750 TX FIFO.
	if (address == 0xFF9B && (*SCI3.SCR3 & SCI3_TE)) {
		memory[address] = value;
		SCI3.txPending = value;
		SCI3.txHasPending = true;
		*SCI3.SSR3 &= ~(SCI3_TDRE | SCI3_TEND);
		SCI3.txCountdown = 320;
		SCI3.txIdleCountdown = 0;  /* cancel idle detector — more data coming */
		return;
	}
#endif

	memory[address] = value;
}

void setMemory16(uint32_t address, uint16_t value){
	address = address & 0x0000ffff; // Keep lower 16 bits only
	memory[address] = value >> 8; 
	memory[address + 1] = value & 0xFF; 
}

void setMemory32(uint32_t address, uint32_t value){
	address = address & 0x0000ffff; // Keep lower 16 bits only
	memory[address] = value >> 24; 
	memory[address + 1] = (value >> 16) & 0xFF; 
	memory[address + 2] = (value >> 8) & 0xFF; 
	memory[address + 3] = value & 0xFF; 
}

uint16_t getMemory8(uint32_t address){
	address = address & 0x0000ffff; // Keep lower 16 bits only
	uint8_t value = memory[address];

#ifdef __3DS__
	// RDR3 read — clear RDRF, start countdown for next byte.
	// Bytes are delivered at baud rate (~320 cycles/byte) so the ROM's
	// inter-packet gap detection (4 SYSTICK ticks) works correctly.
	// SSR3 read — latch which flags the guest saw as 1 (read-1-then-write-0
	// clear protocol; see lastReadSSR3).
	if (address == 0xFF9C) {
		lastReadSSR3 = value;
	}

	if (address == 0xFF9D) {
		*SCI3.SSR3 &= ~SCI3_RDRF;
		if (SCI3.rxPos < SCI3.rxLen) {
			SCI3.rxCountdown = 320; // next byte arrives after one baud period
		}
		{
			static int sciTrace3 = -1;
			if (sciTrace3 < 0) { const char *e = getenv("POKESTRIDE_SCI_TRACE"); if (!e) e = getenv("PWDBG_SCI_TRACE"); sciTrace3 = (e && *e && *e != '0') ? 1 : 0; }
			if (sciTrace3) fprintf(stderr, "[RD] %u/%u pc=%04X\n", SCI3.rxPos, SCI3.rxLen, pc);
		}
	}
#endif

	return value;
}

uint16_t getMemory16(uint32_t address){
	address = address & 0x0000ffff; // Keep lower 16 bits only
	return (uint16_t)((memory[address] << 8) | (memory[address + 1]));
}

uint32_t getMemory32(uint32_t address){
	address = address & 0x0000ffff; // Keep lower 16 bits only
	return (uint32_t)((memory[address] << 24) | (memory[address + 1] << 16) | (memory[address + 2] << 8) | memory[address + 3]);
}

// Parameters are unsigned: signed ones get sign-extended and break the carry calculations.
void setFlagsADD(uint32_t value1, uint32_t value2, int numberOfBits){
	// TODO: might be greately simplified, see setFlagsINC
	uint32_t maxValue;
	uint32_t maxValueLo;
	uint32_t negativeFlag;
	uint32_t halfCarryFlag;
	switch(numberOfBits){
		case 8:{
			maxValue = 0xFF;
			maxValueLo = 0xF;
			negativeFlag = 0x80;
			halfCarryFlag = 0x8;

			// TODO: maybe we can just cast to a signed int here and not have to use the flags
			flags.Z = (uint8_t)(value1 + value2) == 0x0;  
			flags.N = (uint8_t)(value1 + value2) & negativeFlag;  

		}break;
		case 16:{
			maxValue = 0xFFFF;
			maxValueLo = 0xFF;
			negativeFlag = 0x8000;
			halfCarryFlag = 0x100;

			flags.Z = (uint16_t)(value1 + value2) == 0x0;  
			flags.N = (uint16_t)(value1 + value2) & negativeFlag;  

		}break;
		case 32:{
			maxValue = 0xFFFFFFFF;
			maxValueLo = 0xFFFF;
			negativeFlag = 0x80000000;
			halfCarryFlag = 0x10000;

			flags.Z = (uint32_t)(value1 + value2) == 0x0;  
			flags.N = (uint32_t)(value1 + value2) & negativeFlag;  


		}break;
	}

	flags.V = ~(value1 ^ value2) & ((value1 + value2) ^ value1) & negativeFlag; // If both operands have the same sign and the results is from a different sign, overflow has occured.
	flags.C = (value1 & negativeFlag) && !(value2 & negativeFlag) && !((value1 + value2) & negativeFlag);
	flags.H = (((value1 & maxValueLo) + (value2 & maxValueLo) & halfCarryFlag) == halfCarryFlag) ? 1 : 0; 
}

void setFlagsSUB(uint32_t value1, uint32_t value2, int numberOfBits){
	// TODO: might be greately simplified, see setFlagsINC
	uint32_t maxValueLo;
	uint32_t negativeFlag;
	uint32_t halfCarryFlag;
	switch(numberOfBits){
		case 8:{
			maxValueLo = 0xF;
			negativeFlag = 0x80;
			halfCarryFlag = 0x8;

			// TODO: maybe we can just cast to a signed int here and not have to use the flags
			flags.N = (uint8_t)(value1 - value2) & negativeFlag;  

		}break;
		case 16:{
			maxValueLo = 0xFF;
			negativeFlag = 0x8000;
			halfCarryFlag = 0x100;

			flags.N = (uint16_t)(value1 - value2) & negativeFlag;  

		}break;
		case 32:{
			maxValueLo = 0xFFFF;
			negativeFlag = 0x80000000;
			halfCarryFlag = 0x10000;

			flags.N = (uint32_t)(value1 - value2) & negativeFlag;  


		}break;
	}

	flags.Z = (value1 - value2) == 0x0;  
	flags.V = ((value1 ^ value2) & negativeFlag) && (~((value1 - value2) ^ value2) & negativeFlag); // If both operands have a different sign and the results is from the same sing as the 2nd op, overflow has occured.
	flags.C = value2 > value1;
	flags.H = (value2 & maxValueLo) > (value1 & maxValueLo); 
}

void setFlagsINC(uint32_t value1, uint32_t value2, int numberOfBits){
	uint32_t negativeFlag = (1 << (numberOfBits-1));
	flags.N = (value1 + value2) & negativeFlag;  
	flags.Z = ((value1 + value2) == 0) ? true : false;
	flags.V = ~(value1 ^ value2) & ((value1 + value2) ^ value1) & negativeFlag; // If both operands have the same sign and the results is from a different sign, overflow has occured.
}

void setFlagsMOV(uint32_t value, int numberOfBits){
	flags.V = 0;
	flags.Z = (value == 0x0);
	switch(numberOfBits){
		case 8:{
			flags.N = value & 0x80;
		}break;
		case 16:{
			flags.N = value & 0x8000;
		}break;
		case 32:{
			flags.N = value & 0x80000000;
		}break;
	}
}
void setKeys(uint8_t input){
	// IRQ0 is generated on rising edge only!
	if (!flags.I && (input & ENTER)){
		*IRQ_IRR1 |= IRRI0;
	}
	else{
		addElement(&inputQueue, input);
		addElement(&inputQueue, 0); // Simulate key release
		sleep = false;
	}
}

void runSubClock(){
	// Timer handling
	if (TimerB.on && ((subClockCyclesEllapsed % 256) == 0)){
		if(++(*TimerB.TCB1) == 0){
			*IRQ_IRR2 |= IRRTB1;
			*TimerB.TCB1 = TimerB.TLBvalue;
		}
	}
	if (TimerW.on){
		if(*TimerW.TMRW & CTS){
			/* Tick TCNT at the rate determined by TCRW CKS bits [6:4].
			 * runSubClock fires at 32768 Hz (sub-clock). CKS selects:
			 *   CKS=4: phi_w undivided (32768 Hz) → divider 1
			 *   CKS=5: phi_w/2 (16384 Hz) → divider 2
			 *   CKS=6: phi_w/4 (8192 Hz)  → divider 4
			 *   CKS=7: phi_w/8 (4096 Hz)  → divider 8
			 *   CKS=0-3: system clock rates — approximate as every tick
			 */
			static uint8_t twDivCounter = 0;
			uint8_t cks = (*TimerW.TCRW >> 4) & 0x07;
			uint8_t divider;
			switch (cks) {
				case 4:  divider = 1; break;  /* phi_w */
				case 5:  divider = 2; break;  /* phi_w/2 */
				case 6:  divider = 4; break;  /* phi_w/4 */
				case 7:  divider = 8; break;  /* phi_w/8 */
				default: divider = 1; break;  /* CKS=0-3: system clock, approx */
			}
			if (++twDivCounter >= divider) {
				twDivCounter = 0;
				setMemory16(TCNT_ADDRESS, getMemory16(TCNT_ADDRESS) + 1);
			}
		}
		if (getMemory16(TCNT_ADDRESS) == getMemory16(0xf0f8)){ // TCNT == GRA
			if (*TimerW.TCRW & CCLR){
				setMemory16(TCNT_ADDRESS, 0);
			}
			*TimerW.TSRW |= 0x1; // IMFA
			if (*TimerW.TIERW & 0x1){ // IMIEA
				if (!flags.I){
					interruptPushContext();
					flags.I = true;
					pc = VECTOR_TIMER_W;
					sleep = false;
				}
			}
		}
	}

	TimerB.on = (*CKSTPR1 & TB1CKSTP) && (*TimerB.TMB1 & TMB_COUNTING);
	TimerW.on = (*CKSTPR2 & TWCKSTP) && (*TimerW.TMRW & CTS);
}

/* (Timer W ticks in runSubClock at sub-clock rate) */

/*
 * H8/300H base cycle counts indexed by first opcode byte.
 * Sub-cases (e.g. 0x01 prefix instructions that access memory)
 * may override via the 'cycles' variable inside the switch.
 */
static const uint8_t h8_cycles[256] = {
	/* 0x00-0x0F  NOP, MOV/ADD/INC/ADDS reg, CCR ops          */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* 0x10-0x1F  shifts, SUB/DEC/SUBS/CMP/SUBX reg           */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* 0x20-0x2F  MOV.B @aa:8, Rd                              */ 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	/* 0x30-0x3F  MOV.B Rs, @aa:8                              */ 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	/* 0x40-0x4F  Bcc d:8                                      */ 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	/* 0x50       MULXU.B          12                          */
	/* 0x51       DIVXU.B          12                          */
	/* 0x54       RTS               8                          */
	/* 0x55       BSR d:8           6                          */
	/* 0x56       RTE              10                          */
	/* 0x58       Bcc d:16          6                          */
	/* 0x59       JMP @ERn          4                          */
	/* 0x5A       JMP @aa:16        6                          */
	/* 0x5B       JMP @@aa:8        8                          */
	/* 0x5C       BSR d:16          8                          */
	/* 0x5D       JSR @ERn          6                          */
	/* 0x5E       JSR @aa:16        6                          */
	/* 0x5F       JSR @@aa:8        8                          */
	                                                            12,12, 2, 2, 8, 6,10, 2, 6, 4, 6, 8, 8, 6, 6, 8,
	/* 0x60-0x67  bit ops reg (2)  0x68-0x6F  MOV @ERn/aa:16  */ 2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 6, 6, 6, 6, 6, 6,
	/* 0x70-0x77  bit ops reg (2)  0x78-0x7F  extended/imm    */ 2, 2, 2, 2, 2, 2, 2, 2,10, 4, 6, 2, 8, 8, 6, 6,
	/* 0x80-0x8F  ADD.B #imm:8                                 */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* 0x90-0x9F  ADDX  #imm:8                                 */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* 0xA0-0xAF  CMP.B #imm:8                                 */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* 0xB0-0xBF  SUBX  #imm:8                                 */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* 0xC0-0xCF  OR.B  #imm:8                                 */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* 0xD0-0xDF  XOR.B #imm:8                                 */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* 0xE0-0xEF  AND.B #imm:8                                 */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* 0xF0-0xFF  MOV.B #imm:8                                 */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
};

int runNextInstruction(uint64_t* cycleCount){
	uint32_t cycles = 2;
	if (!sleep){
		// ROM-specific hooks — original pwflash.rom (entry=0x02C4)
		if (entry == 0x02C4) {
			if (pc == 0x336){ pc += 4; return 0; }        // Skip factory test
			if (pc == 0x350){ pc += 4; *RL[0] = 0; return 0; } // Skip battery check
			if (pc == 0x7700){ pc += 2; return 0; }       // Skip accel SLEEP
			if (pc == 0x9b84) {                            // Input hook
				if (!isEmpty(&inputQueue))
					setMemory8(0xffde, popElement(&inputQueue));
			}
		}
		// Compiled ROM hooks (entry=0x3F0E)
		if (entry == 0x3F0E) {
			if (pc >= 0x118E && pc < 0x1230){ *RL[0] = 1; /* return true */
				/* Force return — pop PC from stack */
				pc = (memory[*SP & 0xFFFF] << 8) | memory[(*SP & 0xFFFF) + 1];
				*SP += 2;
				return 0;
			}
			if (pc == 0x10A8){ *RL[0] = 0;
				/* checkBatteryBelowLevel → return false — pop PC */
				pc = (memory[*SP & 0xFFFF] << 8) | memory[(*SP & 0xFFFF) + 1];
				*SP += 2;
				return 0;
			}
			if (pc == 0x2ABE){
				/* factoryTestPerformIfNeeded → skip — pop PC */
				pc = (memory[*SP & 0xFFFF] << 8) | memory[(*SP & 0xFFFF) + 1];
				*SP += 2;
				return 0;
			}
			// Input hook: readAndProcessKeyInput at 0x19D8
			if (pc == 0x19D8) {
				if (!isEmpty(&inputQueue))
					setMemory8(0xffde, popElement(&inputQueue));
			}
		}
		// Alternate firmware hooks (entry=0x0080)
		if (entry == 0x0080) {
			// Input hook: ui_keypoll_main. The hooks above are gated to the
			// other firmware builds; this build needs the inject queue drained
			// here or LEFT/RIGHT never reach the button port and the menu can't
			// navigate. (ENTER still works: it is delivered through IRQ0, not
			// the port.)
			//
			// The button input register is PORT B at 0xFFDE. Pop one queued
			// sample per keypoll call (press, then release) and write only the
			// button bits (0,2,4 = ENTER/LEFT/RIGHT), preserving the firmware's
			// own output bits on the same port.
			//
			// ui_keypoll_main's PC varies between builds, so it is resolved from
			// the ELF symbol table into walkerV2KeypollPC at startup.
			// 0 = unresolved.
			if (walkerV2KeypollPC && pc == walkerV2KeypollPC) {
				if (!isEmpty(&inputQueue)) {
					uint8_t key = popElement(&inputQueue);
					setMemory8(0xffde, (memory[0xffde] & 0xEA) | (key & 0x15));
				}
			}
		}
		lastExecPC = pc;
		if (drawTraceEnabled && pc == drawTracePC) {
			/* lcd_draw_image entry: r0=(y<<8)|x, r1=(h<<8)|w; the source
			 * pointer is in er2 (24-bit). The caller's return address sits on
			 * top of the stack (normal-mode 2-byte PC pushed by JSR/BSR). */
			uint16_t r0 = *R[0], r1 = *R[1], r2 = *R[2];
			uint32_t er2 = *ER[2];
			uint16_t sp = (uint16_t)(*SP & 0xFFFF);
			uint16_t caller = (uint16_t)((memory[sp] << 8) | memory[(uint16_t)(sp + 1)]);
			fprintf(stderr,
				"[DRAW] x=%-3u y=%-3u w=%-3u h=%-3u  r0=%04X r1=%04X  src=er2:%06X(r2=%04X)  caller=0x%04X\n",
				r0 & 0xFF, (r0 >> 8) & 0xFF, r1 & 0xFF, (r1 >> 8) & 0xFF,
				r0, r1, er2 & 0xFFFFFF, r2, caller);
		}
		uint16_t* currentInstruction = (uint16_t*)(memory + pc);
		// IMPROVEMENT: maybe just use pointers to the ROM, left this way cause it seems cleaner
		uint16_t ab = (*currentInstruction << 8) | (*currentInstruction >> 8); // 0xbHbL aHaL -> aHaL bHbL

		uint8_t a = ab >> 8;
		cycles = h8_cycles[a];
		uint8_t aH = (a >> 4) & 0xF;
		uint8_t aL = a & 0xF;

		uint8_t b = ab & 0xFF;
		uint8_t bH = (b >> 4) & 0xF;
		uint8_t bL = b & 0xF;

		uint16_t cd = (*(currentInstruction + 1) << 8) | (*(currentInstruction + 1) >> 8);
		uint8_t c = cd >> 8;
		uint8_t cH = (c >> 4) & 0xF; 
		uint8_t cL = c & 0xF;

		uint8_t d = cd & 0xFF;
		uint8_t dH = (d >> 4) & 0xF;
		uint8_t dL = d & 0xF;

		uint16_t ef = (*(currentInstruction + 2) << 8) | (*(currentInstruction + 2) >> 8);
		uint8_t e = ef >> 8;
		uint8_t eH = (e >> 4) & 0xF; 
		uint8_t eL = e & 0xF;

		uint8_t f = ef & 0xFF;
		uint8_t fH = (f >> 4) & 0xF;
		uint8_t fL = f & 0xF;

		uint32_t cdef = cd << 16 | ef;                     

		switch(a){
			case 0x00 ... 0x0F:{
				switch(aL){
					case 0x0:{
					printInstruction("%04x - NOP\n", pc);
					}break;
					case 0x1:{
						switch(bH){ // NOTE. we're ignoring bL here, might not be necesary
							case 0x1: case 0x2: case 0x3:{ // STM.L/LDM.L — 01 10/20/30 6D Fx
								if (c == 0x6D){
									uint8_t count = bH + 1; /* bH=1→2 regs, 2→3, 3→4 */
									bool isLdm = (dH & 0x8) != 0;
									if (isLdm){
										uint8_t firstReg = dL;
										for (int i = 0; i < count; i++){
											struct RegRef32 Rd = getRegRef32(firstReg + i);
											*Rd.ptr = getMemory32(*SP);
											*SP += 4;
										}
									} else {
										uint8_t lastReg = dL;
										for (int i = 0; i < count; i++){
											*SP -= 4;
											struct RegRef32 Rs = getRegRef32(lastReg - i);
											setMemory32(*SP, *Rs.ptr);
										}
									}
									pc += 4;
								} else { return 1; }
							}break;
							case 0x0:{ // Lots of MOV.l type instructions and push.l + pop.l
								switch(c){
									case 0x6B:{
										switch(dH){
											case 0x0:{ // MOV.l @aa:16, Rd
												uint32_t address = (ef & 0x0000FFFF) | 0x00FF0000; 
												uint32_t value = getMemory32(address);

												struct RegRef32 Rd = getRegRef32(dL);

												setFlagsMOV(value, 32);
												*Rd.ptr = value;

												printInstruction("%04x - MOV.l @%x:16, ER%d\n", pc, address, Rd.idx); 
												printRegistersState();

											}break;

											case 0x8:{ // MOV.l Rs, @aa:16 
												uint32_t address = (cdef & 0x0000FFFF) | 0x00FF0000; 

												struct RegRef32 Rs = getRegRef32(dL);

												uint32_t value = *Rs.ptr;
												setFlagsMOV(value, 32);
												setMemory32(address, value);

												printInstruction("%04x - MOV.l ER%d,@%x:16 \n", pc, Rs.idx, address); 
												printMemory(address, 4);
												printRegistersState();

											}break;
											default:{
												return 1;
											} break;
										}
										pc += 4;


									}break;
									case 0x6D:{ // MOV.l @ERs+, ERd --- MOV.l ERs, @-ERd
										char incOrDec = (dH & 0b1000) ? '-' : '+';

										if (incOrDec == '+'){
											struct RegRef32 Rs = getRegRef32(dH);
											struct RegRef32 Rd = getRegRef32(dL);

											uint32_t value = getMemory32(*Rs.ptr);

											*Rs.ptr += 4;

											setFlagsMOV(value, 32);
											*Rd.ptr = value;

											printInstruction("%04x - MOV.l @ER%d+, ER%d\n", pc, Rs.idx, Rd.idx); 

										} else{
											struct RegRef32 Rs = getRegRef32(dL);
											struct RegRef32 Rd = getRegRef32(dH);

											*Rd.ptr -= 4;

											uint32_t value = *Rs.ptr;
											setMemory32(*Rd.ptr, value);
											setFlagsMOV(value, 32);

											printInstruction("%04x - MOV.l ER%d, @-ER%d, \n", pc, Rs.idx, Rd.idx); 
											printMemory(*Rd.ptr, 4);

										}
										printRegistersState();
										pc +=2;


									} break;
									case 0x6F:{ 
										uint16_t disp = ef;
										bool msbDisp = disp & 0x8000;
										uint32_t signExtendedDisp = msbDisp ? (0xFFFF0000 | disp) : disp;

										if (!(dH & 0b1000)){ // From memory @(d:16, ERs), ERd
											struct RegRef32 Rs = getRegRef32(dH);
											struct RegRef32 Rd = getRegRef32(dL);

											uint32_t value = getMemory32(*Rs.ptr + signExtendedDisp); 
											*Rd.ptr = value;
											setFlagsMOV(value, 32);

											printInstruction("%04x - MOV.l @(%d:16, ER%d), ER%d\n", pc, disp, Rs.idx, Rd.idx); 

										} else{ // To memory  ERs, @(d:16,ERd) 
											struct RegRef32 Rs = getRegRef32(dL);
											struct RegRef32 Rd = getRegRef32(dH);

											uint32_t value = *Rs.ptr;
											setFlagsMOV(value, 32);


											setMemory32(*Rd.ptr + signExtendedDisp, value);
											printInstruction("%04x - MOV.l ER%d,@(%d:16, ER%d)\n", pc, Rs.idx, disp, Rd.idx); 
											printMemory(*Rd.ptr + signExtendedDisp, 4);
										}
										printRegistersState();
										pc+=4;
									} break;
									case 0x69:{ 
										if (!(dH & 0x8)){ // MOV.L @ERs, ERd
											struct RegRef32 Rs = getRegRef32(dH);
											struct RegRef32 Rd = getRegRef32(dL);

											uint32_t value = getMemory32(*Rs.ptr);

											setFlagsMOV(value, 32);
											*Rd.ptr = value;

											printInstruction("%04x - MOV.l @ER%d, ER%d\n", pc, Rs.idx, Rd.idx ); 
											printRegistersState();
										} else{ // MOV.l ERs, @ERd 
											struct RegRef32 Rs = getRegRef32(dL);
											struct RegRef32 Rd = getRegRef32(dH);
											uint32_t value = *Rs.ptr;
											setFlagsMOV(value, 32);
											setMemory32(*Rd.ptr, value);
											printInstruction("%04x - MOV.l ER%d, @ER%d, \n", pc, Rs.idx, Rd.idx);
											printMemory(*Rd.ptr, 4);
										}
										pc += 2;
									}break;
									case 0x66:{ // AND.L Rs, ERd
										struct RegRef32 Rs = getRegRef32(dH);
										struct RegRef32 Rd = getRegRef32(dL);

										uint32_t value = *Rs.ptr;
										uint32_t newValue = value & *Rd.ptr;

										setFlagsMOV(newValue, 32);
										*Rd.ptr = newValue;

										printInstruction("%04x - AND.l R%d, ER%d\n", pc, Rs.idx, Rd.idx ); 
										printRegistersState();

										pc += 2;
									}break;
									case 0x64:{ // OR.L Rs, ERd
										struct RegRef32 Rs = getRegRef32(dH);
										struct RegRef32 Rd = getRegRef32(dL);

										uint32_t value = *Rs.ptr;
										uint32_t newValue = value | *Rd.ptr;

										setFlagsMOV(newValue, 32);
										*Rd.ptr = newValue;

									printInstruction("%04x - OR.l R%d, ER%d\n", pc, Rs.idx, Rd.idx ); 
										printRegistersState();

										pc += 2;
									}break;
									case 0x65:{ // XOR.L Rs, ERd
										struct RegRef32 Rs = getRegRef32(dH);
										struct RegRef32 Rd = getRegRef32(dL);

										uint32_t value = *Rs.ptr;
										uint32_t newValue = value ^ *Rd.ptr;

										setFlagsMOV(newValue, 32);
										*Rd.ptr = newValue;

										printInstruction("%04x - XOR.l R%d, ER%d\n", pc, Rs.idx, Rd.idx ); 
										printRegistersState();

										pc += 2;
									}break;
									default:{
										return 1;
									} break;


								}

							}break;
							case 0x4:{
								pc+=2;
								if (bL == 0x0 && cH == 0x6){
									switch(cL){
										case 0x9:
										case 0xB:
										case 0xD:
										case 0xF:{
											uint8_t mostSignificantBit = dH >> 7;
											if (mostSignificantBit == 0x1){
												printInstruction("%04x - STC\n", pc);// Unused in the ROM
											}else{
												printInstruction("%04x - LDC\n", pc); // Unused in the ROM
											}
										}break;
										default:{
											return 1;
										} break;

									}
								}
							}break;
							case 0x8:{
								sleep = true;
								printInstruction("%04x - SLEEP\n", pc);
							}break;
							case 0xC:{
								if (bL == 0x0 && cH == 0x5){
									switch(cL){
										case 0x0:{ // MULXS B Rs, Rd
											struct RegRef8 Rs = getRegRef8(dH);
											struct RegRef16 Rd = getRegRef16(dL);
											int8_t lowerBitsRd = *Rd.ptr & 0x00FF;
											*Rd.ptr = (int16_t)*Rs.ptr * (int16_t)lowerBitsRd; 
											flags.Z = (*Rd.ptr == 0) ? 1 : 0;
											flags.N = (*Rd.ptr & 0x8000) ? 1 : 0;

											printInstruction("%04x - MULXS B r%d%c, %c%d\n", pc, Rs.idx, Rs.loOrHiReg, Rd.loOrHiReg, Rd.idx);
											printRegistersState();
											pc += 2;
										} break;
										case 0x2:{
											// MULXS W Rs, Rd
											struct RegRef16 Rs = getRegRef16(dH);
											struct RegRef32 Rd = getRegRef32(dL);
											int16_t lowerBitsRd = *Rd.ptr & 0x0000FFFF;
											*Rd.ptr = (int32_t)*Rs.ptr * (int32_t)lowerBitsRd;
											flags.Z = (*Rd.ptr == 0) ? 1 : 0;
											flags.N = (*Rd.ptr & 0x80000000) ? 1 : 0;

											printInstruction("%04x - MULXS W %c%d, er%d\n", pc, Rs.loOrHiReg, Rs.idx, Rd.idx);
											printRegistersState();
											pc += 2;
										}break;
										default:{
											return 1;
										} break;

									}
								};
							}break;
							case 0xD:{
								if (bL == 0x0 && cH == 0x5){
									switch(cL){ // TODO replace with if, and see if merging it with C & F makes it more readable
										case 0x1:{ // DIVXS B Rs, Rd
											struct RegRef8 Rs = getRegRef8(dH);
											struct RegRef16 Rd = getRegRef16(dL);
											int8_t quotient = (int16_t)*Rd.ptr / (int8_t)*Rs.ptr;
											int8_t remainder = (int16_t)*Rd.ptr % (int8_t)*Rs.ptr; // Following C99 rules for the sign of quotient and remainder, the actual behaviour isnt really documented in the H800 manual
											*Rd.ptr = (remainder << 8) | quotient;

											flags.Z = (*Rs.ptr == 0) ? 1 : 0;
											flags.N = (((int16_t)quotient) > 0) ? 0 : 1;

											printInstruction("%04x - DIVXS B r%d%c, %c%d\n", pc, Rs.idx, Rs.loOrHiReg, Rd.loOrHiReg, Rd.idx);
											printRegistersState();
											pc += 2;
										} break;
										case 0x3:{ // DIVXS W Rs, Rd
											struct RegRef16 Rs = getRegRef16(dH);
											struct RegRef32 Rd = getRegRef32(dL);
											int16_t quotient = (int32_t)*Rd.ptr / (int16_t)*Rs.ptr;
											int16_t remainder = (int32_t)*Rd.ptr % (int16_t)*Rs.ptr; 
											*Rd.ptr = (remainder << 16) | quotient;

											flags.Z = (*Rs.ptr == 0) ? 1 : 0;
											flags.N = (quotient & 0x80000000) ? 1 : 0;

											printInstruction("%04x - DIVXU W %c%d, er%d\n", pc, Rs.loOrHiReg, Rs.idx, Rd.idx);
											printRegistersState();
											pc += 2;
										}break;
										default:{
											return 1;
										} break;

									}
								};
							}break;
							case 0xF:{ // 01 F0 prefix — 32-bit OR.L / XOR.L / AND.L
								/* 4-byte instructions. The outer switch adds pc+=2
								 * after this case returns; one inner pc+=2 below
								 * (end of the if-block) makes total +4. */
								if (bL == 0x0 && cH == 0x6){
									struct RegRef32 Rs = getRegRef32(dH);
									struct RegRef32 Rd = getRegRef32(dL);
									switch(cL){
										case 0x4:{ // OR.L ERs, ERd
											uint32_t newValue = *Rs.ptr | *Rd.ptr;
											setFlagsMOV(newValue, 32);
											*Rd.ptr = newValue;
											printInstruction("%04x - OR.l ER%d, ER%d\n", pc, Rs.idx, Rd.idx);
											printRegistersState();
										}break;
										case 0x5:{ // XOR.L ERs, ERd
											uint32_t newValue = *Rs.ptr ^ *Rd.ptr;
											setFlagsMOV(newValue, 32);
											*Rd.ptr = newValue;
											printInstruction("%04x - XOR.l ER%d, ER%d\n", pc, Rs.idx, Rd.idx);
											printRegistersState();
										}break;
										case 0x6:{ // AND.L ERs, ERd
											uint32_t newValue = *Rs.ptr & *Rd.ptr;
											setFlagsMOV(newValue, 32);
											*Rd.ptr = newValue;
											printInstruction("%04x - AND.l ER%d, ER%d\n", pc, Rs.idx, Rd.idx);
											printRegistersState();
										}break;
										default:{
											return 1;
										} break;
									}
									pc += 2;
								} else {
									return 1;
								}
							}break;

							default:{
								printInstruction("???\n");
								return 1; // UNIMPLEMENTED
							}break;

						}
					}break;
					case 0x2:{ // STC.B CCR, Rd   (02 0N)
						struct RegRef8 Rd = getRegRef8(bL);
						*Rd.ptr = packFlags();
						printInstruction("%04x - STC.B CCR, r%d%c\n", pc, Rd.idx, Rd.loOrHiReg);
						printRegistersState();
					}break;
					case 0x3:{ // LDC.B Rs, CCR
						struct RegRef8 Rs = getRegRef8(bL);
						uint8_t value = *Rs.ptr;
						setFlags(value);
						printInstruction("%04x - LDC.B r%d%c, CCR\n", pc, Rs.idx, Rs.loOrHiReg);
						printRegistersState();
					}break;
					case 0x4:{ // ORC #xx:8, CCR   (04 ii)
						setFlags(packFlags() | b);
						printInstruction("%04x - ORC #%x:8, CCR\n", pc, b);
						printRegistersState();
					}break;
					case 0x5:{ // XORC #xx:8, CCR  (05 ii)
						setFlags(packFlags() ^ b);
						printInstruction("%04x - XORC #%x:8, CCR\n", pc, b);
						printRegistersState();
					}break;
					case 0x6:{ // ANDC #xx:8, CCR  (06 ii)
						setFlags(packFlags() & b);
						printInstruction("%04x - ANDC #%x:8, CCR\n", pc, b);
						printRegistersState();
					}break;
					case 0x7:{ // LDC.B #xx:8, CCR
						uint8_t value = b;
						setFlags(value);
						printInstruction("%04x - LDC.B #%x:8, CCR\n", pc, value);
						printRegistersState();
					}break;
					case 0x8:{ // ADD.B Rs, Rd
						struct RegRef8 Rs = getRegRef8(bH);
						struct RegRef8 Rd = getRegRef8(bL);

						setFlagsADD(*Rd.ptr, *Rs.ptr, 8);
						*Rd.ptr += *Rs.ptr;

						printInstruction("%04x - ADD.b R%d%c,R%d%c\n", pc, Rs.idx, Rs.loOrHiReg, Rd.idx, Rd.loOrHiReg); 
						printRegistersState();
					}break;
					case 0x9:{ // ADD.W Rs, Rd
					struct RegRef16 Rs = getRegRef16(bH);
					struct RegRef16 Rd = getRegRef16(bL);

						setFlagsADD(*Rd.ptr, *Rs.ptr, 16);

						*Rd.ptr += *Rs.ptr;
						printInstruction("%04x - ADD.w %c%d,%c%d\n", pc, Rs.loOrHiReg, Rs.idx, Rd.loOrHiReg,  Rd.idx); 
						printRegistersState();

					}break;
					case 0xA:{
						switch(bH){
							case 0x0:{ // INC.b Rd
								struct RegRef8 Rd = getRegRef8(bL);
								setFlagsINC(*Rd.ptr, 1, 8);
								*Rd.ptr += 1;
								printInstruction("%04x - INC.b r%d%c\n", pc, Rd.idx, Rd.loOrHiReg);
								printRegistersState();
							}break;
							case 0x8:
							case 0x9:
							case 0xA:
							case 0xB:
							case 0xC:
							case 0xD:
							case 0xE:
							case 0xF:{ // ADD.l ERs, ERd
								struct RegRef32 Rs = getRegRef32(bH);
								struct RegRef32 Rd = getRegRef32(bL);

								setFlagsADD(*Rd.ptr, *Rs.ptr, 32);

								*Rd.ptr += *Rs.ptr;
								printInstruction("%04x - ADD.l ER%d, ER%d\n", pc, Rs.idx,  Rd.idx); 
								printRegistersState();
							}break;
							default:{
								return 1;
							} break;
						}
					}break;
					case 0xB:{ // ADDS and INC
						switch(bH){
							case 0x0:{ // ADDS.l #1, ERd
								struct RegRef32 Rd = getRegRef32(bL);
								*Rd.ptr += 1;
								printInstruction("%04x - ADDS.l #1, ER%d\n", pc, Rd.idx);
							}break;
							case 0x8:{ // ADDS.l #2, ERd
								struct RegRef32 Rd = getRegRef32(bL);
								*Rd.ptr += 2;
								printInstruction("%04x - ADDS.l #2, ER%d\n", pc, Rd.idx);
							}break;
							case 0x9:{ // ADDS.l #4, ERd
								struct RegRef32 Rd = getRegRef32(bL);
								*Rd.ptr += 4;
								printInstruction("%04x - ADDS.l #4, ER%d\n", pc, Rd.idx);
							}break;
							case 0x5:{ // INC.w #1, Rd
								struct RegRef16 Rd = getRegRef16(bL);
								setFlagsINC(*Rd.ptr, 1, 16); 
								*Rd.ptr += 1;
								printInstruction("%04x - INC.w #1, %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
							}break;
							case 0x7:{ // INC.l #1, ERd
								struct RegRef32 Rd = getRegRef32(bL);
								setFlagsINC(*Rd.ptr, 1, 32);
								*Rd.ptr += 1;
								printInstruction("%04x - INC.l #1, ER%d\n", pc, Rd.idx);
							} break;
							case 0xD:{ // INC.w #2, Rd
								struct RegRef16 Rd = getRegRef16(bL);
								setFlagsINC(*Rd.ptr, 2, 16);
								*Rd.ptr += 2;
								printInstruction("%04x - INC.w #2, %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
							}break;
							case 0xF:{ // INC.l #2, ERd
								struct RegRef32 Rd = getRegRef32(bL);
								setFlagsINC(*Rd.ptr, 2, 32);
								*Rd.ptr += 2;
								printInstruction("%04x - INC.l #2, ER%d\n", pc, Rd.idx);
							}break;
							default:{
								return 1;
							} break;
						}
						printRegistersState();
					}break;
					case 0xC:{ // MOV.B Rs, Rd
						struct RegRef8 Rs = getRegRef8(bH);
						struct RegRef8 Rd = getRegRef8(bL);

						setFlagsMOV(*Rs.ptr, 8);
						*Rd.ptr = *Rs.ptr;

						printInstruction("%04x - MOV.b R%d%c,R%d%c\n", pc, Rs.idx, Rs.loOrHiReg, Rd.idx, Rd.loOrHiReg); 
						printRegistersState();

					}break;
					case 0xD:{ // MOV.W Rs, Rd
					struct RegRef16 Rs = getRegRef16(bH);
					struct RegRef16 Rd = getRegRef16(bL);

						setFlagsMOV(*Rs.ptr, 16);

						*Rd.ptr = *Rs.ptr;
						printInstruction("%04x - MOV.w %c%d,%c%d\n", pc, Rs.loOrHiReg, Rs.idx, Rd.loOrHiReg,  Rd.idx); 
						printRegistersState();

					}break;
					case 0xE:{
						printInstruction("%04x - ADDX\n", pc);
						return 1; // UNIMPLEMENTED
					}break;
					case 0xF:{
						switch(bH){
							case 0x0:{
								printInstruction("%04x - DAA\n", pc);
								return 1; // UNIMPLEMENTED
							}break;
							case 0x8:
							case 0x9:
							case 0xA:
							case 0xB:
							case 0xC:
							case 0xD:
							case 0xE:
							case 0xF:{// MOV.l ERs, ERd
								struct RegRef32 Rs = getRegRef32(bH);
								struct RegRef32 Rd = getRegRef32(bL);

								setFlagsMOV(*Rs.ptr, 32);

								*Rd.ptr = *Rs.ptr;
								printInstruction("%04x - MOV.l ER%d, ER%d\n", pc, Rs.idx,  Rd.idx); 
								printRegistersState();
							}break;
							default:{
								return 1;
							} break;
						}
					}break;
					default:{
						return 1;
					} break;

				}
			}break;
			case 0x10 ... 0x1F:{
				switch(aL){
					case 0x0:{
						switch(bH){
							case 0x0:{ // SHLL.b Rd
								struct RegRef8 Rd = getRegRef8(bL);
								flags.C = *Rd.ptr & 0x80;
								*Rd.ptr = (*Rd.ptr << 1);
								setFlagsMOV(*Rd.ptr, 8);
								printInstruction("%04x - SHLL.b r%d%c\n", pc, Rd.idx, Rd.loOrHiReg);
							}break;
							case 0x1:{ // SHLL.w Rd
								struct RegRef16 Rd = getRegRef16(bL);
								flags.C = *Rd.ptr & 0x8000;
								*Rd.ptr = (*Rd.ptr << 1);
								setFlagsMOV(*Rd.ptr, 16);
								printInstruction("%04x - SHLL.w %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
							} break;
							case 0x3:{ // SHLL.l Rd
								struct RegRef32 Rd = getRegRef32(bL);
								flags.C = *Rd.ptr & 0x80000000;
								*Rd.ptr = (*Rd.ptr << 1);
								setFlagsMOV(*Rd.ptr, 32);
								printInstruction("%04x - SHLL.l er%d\n", pc, Rd.idx);
							}break;
							case 0x8:{ // SHAL.b Rd -- These differ in their treatment of the V flag
								struct RegRef8 Rd = getRegRef8(bL);
								flags.C = *Rd.ptr & 0x80;
								*Rd.ptr = (*Rd.ptr << 1);
								setFlagsMOV(*Rd.ptr, 8);
								flags.V = flags.C && !(*Rd.ptr & 0x80);
								printInstruction("%04x - SHAL.b r%d%c\n", pc, Rd.idx, Rd.loOrHiReg);
							}break;
							case 0x9:{ // SHAL.w Rd
								struct RegRef16 Rd = getRegRef16(bL);
								flags.C = *Rd.ptr & 0x8000;
								*Rd.ptr = (*Rd.ptr << 1);
								setFlagsMOV(*Rd.ptr, 16);
								flags.V = flags.C && !(*Rd.ptr & 0x8000);
								printInstruction("%04x - SHAL.w %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
							}break;
							case 0xB:{ // SHAL.l Rd
								struct RegRef32 Rd = getRegRef32(bL);
								flags.C = *Rd.ptr & 0x80000000;
								*Rd.ptr = (*Rd.ptr << 1);
								setFlagsMOV(*Rd.ptr, 32);
								flags.V = flags.C && !(*Rd.ptr & 0x80000000);
								printInstruction("%04x - SHAL.l er%d\n", pc, Rd.idx);
							}break;
							default:{
								return 1;
							} break;
						}
						printRegistersState();
					}break;
					case 0x1:{
						switch(bH){
							case 0x0:{ // SHLR.b Rd
								struct RegRef8 Rd = getRegRef8(bL);
								flags.C = *Rd.ptr & 0x1;
								*Rd.ptr = (*Rd.ptr >> 1);
								setFlagsMOV(*Rd.ptr, 8);
								printInstruction("%04x - SHLR.b r%d%c\n", pc, Rd.idx, Rd.loOrHiReg);
							}break;
							case 0x1:{ // SHLR.w Rd
								struct RegRef16 Rd = getRegRef16(bL);
								flags.C = *Rd.ptr & 0x1;
								*Rd.ptr = (*Rd.ptr >> 1);
								setFlagsMOV(*Rd.ptr, 16);
								printInstruction("%04x - SHLR.w %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
							} break;
							case 0x3:{ // SHLR.l Rd
								struct RegRef32 Rd = getRegRef32(bL);
								flags.C = *Rd.ptr & 0x1;
								*Rd.ptr = (*Rd.ptr >> 1);
								setFlagsMOV(*Rd.ptr, 32);
								printInstruction("%04x - SHLR.l er%d\n", pc, Rd.idx);
							}break;
							case 0x8:{ // SHAR.b Rd - Unused in the ROM
								return 1;
							}break;
							case 0x9:{ // SHAR.w Rd
								struct RegRef16 Rd = getRegRef16(bL);
								flags.C = *Rd.ptr & 0x1;
								*Rd.ptr = (*Rd.ptr >> 1) | (*Rd.ptr & 0x8000);
								setFlagsMOV(*Rd.ptr, 16);
								printInstruction("%04x - SHAR.w %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
							}break;
							case 0xB:{ // SHAR.l Rd
								struct RegRef32 Rd = getRegRef32(bL);
								flags.C = *Rd.ptr & 0x1;
								*Rd.ptr = (*Rd.ptr >> 1) | (*Rd.ptr & 0x80000000);
								setFlagsMOV(*Rd.ptr, 32);
								printInstruction("%04x - SHAR.l er%d\n", pc, Rd.idx);
							}break;
							default:{
								return 1;
							} break;
						}
						printRegistersState();
					}break;
					case 0x2:{
						switch(bH){
							case 0x0:{ // ROTXL.b Rd
								struct RegRef8 Rd = getRegRef8(bL);
								bool oldCarry = flags.C;
								flags.C = *Rd.ptr & 0x80;
								*Rd.ptr = (*Rd.ptr << 1) | oldCarry;
								setFlagsMOV(*Rd.ptr, 8);
								printInstruction("%04x - ROTXL.b r%d%c\n", pc, Rd.idx, Rd.loOrHiReg);
							} break;
							case 0x1:{ // ROTXL.w Rd
								struct RegRef16 Rd = getRegRef16(bL);
								bool oldCarry = flags.C;
								flags.C = *Rd.ptr & 0x8000;
								*Rd.ptr = (*Rd.ptr << 1) | oldCarry;
								setFlagsMOV(*Rd.ptr, 16);
								printInstruction("%04x - ROTXL.w %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
							} break;
							case 0x3:{ // ROTXL.l Rd
								struct RegRef32 Rd = getRegRef32(bL);
								bool oldCarry = flags.C;
								flags.C = *Rd.ptr & 0x80000000;
								*Rd.ptr = (*Rd.ptr << 1) | oldCarry;
								setFlagsMOV(*Rd.ptr, 32);
								printInstruction("%04x - ROTXL.l er%d\n", pc, Rd.idx);
							}break;
							case 0x8:{ // ROTL.b Rd
								struct RegRef8 Rd = getRegRef8(bL);
								flags.C = *Rd.ptr & 0x80;
								*Rd.ptr = (*Rd.ptr << 1) | flags.C;
								setFlagsMOV(*Rd.ptr, 8);
								printInstruction("%04x - ROTL.b r%d%c\n", pc, Rd.idx, Rd.loOrHiReg);
							} break;
							case 0x9:{ // ROTL.w Rd
								struct RegRef16 Rd = getRegRef16(bL);
								flags.C = *Rd.ptr & 0x8000;
								*Rd.ptr = (*Rd.ptr << 1) | flags.C;
								setFlagsMOV(*Rd.ptr, 16);
								printInstruction("%04x - ROTL.w %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
							} break;
							case 0xB:{ // ROTL.l Rd
								struct RegRef32 Rd = getRegRef32(bL);
								flags.C = *Rd.ptr & 0x80000000;
								*Rd.ptr = (*Rd.ptr << 1) | flags.C;
								setFlagsMOV(*Rd.ptr, 32);
								printInstruction("%04x - ROTL.l er%d\n", pc, Rd.idx);
							}break;
							default:{
								return 1;
							} break;
						}
						printRegistersState();
					}break;
					case 0x3:{
						switch(bH){
							case 0x0:{ // ROTXR.b Rd (rotate right through carry)
								struct RegRef8 Rd = getRegRef8(bL);
								uint8_t oldCarry = flags.C ? 1 : 0;
								uint8_t bit0 = *Rd.ptr & 0x1;
								flags.C = bit0;
								*Rd.ptr = (uint8_t)((*Rd.ptr >> 1) | (oldCarry << 7));
								setFlagsMOV(*Rd.ptr, 8);
								printInstruction("%04x - ROTXR.b r%d%c\n", pc, Rd.idx, Rd.loOrHiReg);
							} break;
							case 0x1:{ // ROTXR.w Rd
								struct RegRef16 Rd = getRegRef16(bL);
								uint16_t oldCarry = flags.C ? 1 : 0;
								uint16_t bit0 = *Rd.ptr & 0x1;
								flags.C = bit0;
								*Rd.ptr = (uint16_t)((*Rd.ptr >> 1) | (oldCarry << 15));
								setFlagsMOV(*Rd.ptr, 16);
								printInstruction("%04x - ROTXR.w %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
							} break;
							case 0x3:{ // ROTXR.l Rd
								struct RegRef32 Rd = getRegRef32(bL);
								uint32_t oldCarry = flags.C ? 1u : 0u;
								uint32_t bit0 = *Rd.ptr & 0x1u;
								flags.C = bit0;
								*Rd.ptr = (*Rd.ptr >> 1) | (oldCarry << 31);
								setFlagsMOV(*Rd.ptr, 32);
								printInstruction("%04x - ROTXR.l er%d\n", pc, Rd.idx);
							} break;
							case 0x8:{ // ROTR.b Rd (rotate right, bit0 -> bit7 & C)
								struct RegRef8 Rd = getRegRef8(bL);
								uint8_t bit0 = *Rd.ptr & 0x1;
								flags.C = bit0;
								*Rd.ptr = (uint8_t)((*Rd.ptr >> 1) | (bit0 << 7));
								setFlagsMOV(*Rd.ptr, 8);
								printInstruction("%04x - ROTR.b r%d%c\n", pc, Rd.idx, Rd.loOrHiReg);
							} break;
							case 0x9:{ // ROTR.w Rd
								struct RegRef16 Rd = getRegRef16(bL);
								uint16_t bit0 = *Rd.ptr & 0x1;
								flags.C = bit0;
								*Rd.ptr = (uint16_t)((*Rd.ptr >> 1) | (bit0 << 15));
								setFlagsMOV(*Rd.ptr, 16);
								printInstruction("%04x - ROTR.w %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
							} break;
							case 0xB:{ // ROTR.l Rd
								struct RegRef32 Rd = getRegRef32(bL);
								uint32_t bit0 = *Rd.ptr & 0x1u;
								flags.C = bit0;
								*Rd.ptr = (*Rd.ptr >> 1) | (bit0 << 31);
								setFlagsMOV(*Rd.ptr, 32);
								printInstruction("%04x - ROTR.l er%d\n", pc, Rd.idx);
							} break;
							default:{
								return 1;
							} break;
						}
						printRegistersState();
					}break;
					case 0x4:{ // OR.B Rs, Rd
						struct RegRef8 Rs = getRegRef8(bH);
						struct RegRef8 Rd = getRegRef8(bL);

						uint8_t newValue = *Rs.ptr | *Rd.ptr;

						setFlagsMOV(newValue, 8);
						*Rd.ptr = newValue;

						printInstruction("%04x - OR.b R%d%c,R%d%c\n", pc, Rs.idx, Rs.loOrHiReg, Rd.idx, Rd.loOrHiReg); 
						printRegistersState();
					}break;
					case 0x5:{ // XOR.B Rs, Rd
						struct RegRef8 Rs = getRegRef8(bH);
						struct RegRef8 Rd = getRegRef8(bL);

						uint8_t newValue = *Rs.ptr ^ *Rd.ptr;

						setFlagsMOV(newValue, 8);
						*Rd.ptr = newValue;

						printInstruction("%04x - XOR.b R%d%c,R%d%c\n", pc, Rs.idx, Rs.loOrHiReg, Rd.idx, Rd.loOrHiReg); 
						printRegistersState();
					}break;
					case 0x6:{ // AND.B Rs, Rd
						struct RegRef8 Rs = getRegRef8(bH);
						struct RegRef8 Rd = getRegRef8(bL);

						uint8_t newValue = *Rs.ptr & *Rd.ptr;

						setFlagsMOV(newValue, 8);
						*Rd.ptr = newValue;

						printInstruction("%04x - AND.b R%d%c,R%d%c\n", pc, Rs.idx, Rs.loOrHiReg, Rd.idx, Rd.loOrHiReg); 
						printRegistersState();
					}break;
					case 0x7:{
						switch(bH){
							case 0x0:{ // NOT.b Rd
								struct RegRef8 Rd = getRegRef8(bL);
								*Rd.ptr = ~*Rd.ptr;
								setFlagsMOV(*Rd.ptr, 8);
								printInstruction("%04x - NOT.b r%d%c\n", pc, Rd.idx, Rd.loOrHiReg); 
								printRegistersState();
							} break;
							case 0x1:{
								// NOT.w Rd
								struct RegRef16 Rd = getRegRef16(bL);
								*Rd.ptr = ~*Rd.ptr;
								setFlagsMOV(*Rd.ptr, 16);
								printInstruction("%04x - NOT.w %c%d\n", pc, Rd.loOrHiReg, Rd.idx); 
								printRegistersState();
							} break;
							case 0x3:{ // NOT.l Rd
								printInstruction("%04x - NOT.l\n", pc);
								return 1; // Unused in the ROM
							}break;
							case 0x5:{ // EXTU.w Rd
								struct RegRef16 Rd = getRegRef16(bL);
								*Rd.ptr = *Rd.ptr & 0x00FF;
								setFlagsMOV(*Rd.ptr, 16);
								printInstruction("%04x - EXTU.w %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
								printRegistersState();
							} break;
							case 0x7:{ // EXTU.l Rd
								struct RegRef32 Rd = getRegRef32(bL);
								*Rd.ptr = *Rd.ptr & 0x0000FFFF;
								setFlagsMOV(*Rd.ptr, 32);
								printInstruction("%04x - EXTU.l er%d\n", pc, Rd.idx);
								printRegistersState();
							}break;
							case 0x8:{ // NEG.b Rd -- TODO: Untested
								struct RegRef8 Rd = getRegRef8(bL);

								setFlagsSUB(0, *Rd.ptr, 8);
								if (*Rd.ptr != 0x80){
									*Rd.ptr = (int8_t)0 - (int8_t)*Rd.ptr;
								}
								printInstruction("%04x - NEG.b r%d%c\n", pc, Rd.idx, Rd.loOrHiReg); 
								printRegistersState();

							} break;
							case 0x9:{ // NEG.w Rd
								struct RegRef16 Rd = getRegRef16(bL);

								setFlagsSUB(0, *Rd.ptr, 16);
								if (*Rd.ptr != 0x8000){
									*Rd.ptr = (int16_t)0 - (int16_t)*Rd.ptr;
								}
								printInstruction("%04x - NEG.w %c%d\n", pc, Rd.loOrHiReg, Rd.idx); 
								printRegistersState();

							} break;
							case 0xB:{ // NEG.l Rd
								printInstruction("%04x - NEG.l\n", pc);
								return 1; // Unused in the ROM
							}break;
							case 0xD:{ // EXTS.w Rd
								struct RegRef16 Rd = getRegRef16(bL);
								bool sign = *Rd.ptr & 0x80;
								if (sign == 0){
									*Rd.ptr = *Rd.ptr & 0x00FF;
								} else{
									*Rd.ptr = (*Rd.ptr & 0x00FF) | 0xFF00;
								}
								setFlagsMOV(*Rd.ptr, 16);
								printInstruction("%04x - EXTS.w %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
								printRegistersState();
							} break;
							case 0xF:{ // EXTS.l Rd
								struct RegRef32 Rd = getRegRef32(bL);
								bool sign = *Rd.ptr & 0x8000;
								if (sign == 0){
									*Rd.ptr = *Rd.ptr & 0x0000FFFF;
								} else{
									*Rd.ptr = (*Rd.ptr & 0x0000FFFF) | 0xFFFF0000;
								}
								setFlagsMOV(*Rd.ptr, 32);
								printInstruction("%04x - EXTS.l er%d\n", pc, Rd.idx);
								printRegistersState();
							}break;
							default:{
								return 1;
							} break;

						}
					}break;
					case 0x8:{ // SUB.b Rs, Rd
						struct RegRef8 Rs = getRegRef8(bH);
						struct RegRef8 Rd = getRegRef8(bL);

						setFlagsSUB(*Rd.ptr, *Rs.ptr, 8);
						*Rd.ptr -= *Rs.ptr;

						printInstruction("%04x - SUB.b R%d%c,R%d%c\n", pc, Rs.idx, Rs.loOrHiReg, Rd.idx, Rd.loOrHiReg); 
						printRegistersState();

					}break;
					case 0x9:{ // SUB.W Rs, Rd

					struct RegRef16 Rs = getRegRef16(bH);
					struct RegRef16 Rd = getRegRef16(bL);

						setFlagsSUB(*Rd.ptr, *Rs.ptr, 16);

						*Rd.ptr -= *Rs.ptr;
						printInstruction("%04x - SUB.w %c%d,%c%d\n", pc, Rs.loOrHiReg, Rs.idx, Rd.loOrHiReg,  Rd.idx); 
						printRegistersState();

					}break;
					case 0xA:{
						switch(bH){
							case 0x0:{ // DEC.b Rd
								struct RegRef8 Rd = getRegRef8(bL);
								setFlagsINC(*Rd.ptr, -1, 8);
								*Rd.ptr -= 1;
								printInstruction("%04x - DEC.b r%d%c\n", pc, Rd.idx, Rd.loOrHiReg);
								printRegistersState();
							}break;
							case 0x8:
							case 0x9:
							case 0xA:
							case 0xB:
							case 0xC:
							case 0xD:
							case 0xE:
							case 0xF:{ // SUB.l ERs, ERd
								struct RegRef32 Rs = getRegRef32(bH);
								struct RegRef32 Rd = getRegRef32(bL);

								setFlagsSUB(*Rd.ptr, *Rs.ptr, 32);

								*Rd.ptr -= *Rs.ptr;
								printInstruction("%04x - SUB.l ER%d, ER%d\n", pc, Rs.idx,  Rd.idx); 
								printRegistersState();
							}break;
							default:{
								return 1;
							} break;

						}
					}break;
					case 0xB:{
						switch(bH){
							case 0x0:{ // SUBS #1, ERd
								struct RegRef32 Rd = getRegRef32(bL);

								*Rd.ptr -= 1;
								printInstruction("%04x - SUBS #1, ER%d\n", pc, Rd.idx); 
								printRegistersState();
							}break;
							case 0x8:{ // SUBS #2, ERd
								struct RegRef32 Rd = getRegRef32(bL);

								*Rd.ptr -= 2;
								printInstruction("%04x - SUBS #2, ER%d\n", pc, Rd.idx); 
								printRegistersState();
							}break;
							case 0x9:{ // SUBS #4, ERd
								struct RegRef32 Rd = getRegRef32(bL);

								*Rd.ptr -= 4;
								printInstruction("%04x - SUBS #4, ER%d\n", pc, Rd.idx); 
								printRegistersState();
							}break;
							case 0x5:{ // DEC.w #1, Rd
								struct RegRef16 Rd = getRegRef16(bL);
								setFlagsINC(*Rd.ptr, -1, 16);
								*Rd.ptr -= 1;
								printInstruction("%04x - DEC.w #1, %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
								printRegistersState();
							}break;
							case 0x7:{ // DEC.l #1, ERd
								struct RegRef32 Rd = getRegRef32(bL);
								setFlagsINC(*Rd.ptr, -1, 32);
								*Rd.ptr -= 1;
								printInstruction("%04x - DEC.l #1, ER%d\n", pc, Rd.idx);
								printRegistersState();
							}break;
							case 0xD:{ // DEC.w #2, Rd
								struct RegRef16 Rd = getRegRef16(bL);
								setFlagsINC(*Rd.ptr, -2, 16);
								*Rd.ptr -= 2;
								printInstruction("%04x - DEC.w #2, %c%d\n", pc, Rd.loOrHiReg, Rd.idx);
								printRegistersState();
							}break;
							case 0xF:{ // DEC.l #2, ERd
								struct RegRef32 Rd = getRegRef32(bL);
								setFlagsINC(*Rd.ptr, -2, 32);
								*Rd.ptr -= 2;
								printInstruction("%04x - DEC.l #2, ER%d\n", pc, Rd.idx);
								printRegistersState();
							}break;
							default:{
								return 1;
							} break;
						}
					}break;
					case 0xC:{ // CMP.b Rs, Rd
						struct RegRef8 Rs = getRegRef8(bH);
						struct RegRef8 Rd = getRegRef8(bL);

						setFlagsSUB(*Rd.ptr, *Rs.ptr, 8);

						printInstruction("%04x - SUB.b R%d%c,R%d%c\n", pc, Rs.idx, Rs.loOrHiReg, Rd.idx, Rd.loOrHiReg); 
						printRegistersState();

					}break;
					case 0xD:{ // CMP.W Rs, Rd
					struct RegRef16 Rs = getRegRef16(bH);
					struct RegRef16 Rd = getRegRef16(bL);
						setFlagsSUB(*Rd.ptr, *Rs.ptr, 16);

						printInstruction("%04x - CMP.w %c%d,%c%d\n", pc, Rs.loOrHiReg, Rs.idx, Rd.loOrHiReg,  Rd.idx); 
						printRegistersState();
					}break;
					case 0xE:{ // SUBX Rs, Rd
						struct RegRef8 Rs = getRegRef8(bH);
						struct RegRef8 Rd = getRegRef8(bL);

						setFlagsSUB(*Rd.ptr, *Rs.ptr + flags.C, 8); // NOTE: didn't check edge cases (Rs + flag OV)
						*Rd.ptr -= *Rs.ptr;
						*Rd.ptr -= flags.C;

						printInstruction("%04x - SUBX R%d%c,R%d%c\n", pc, Rs.idx, Rs.loOrHiReg, Rd.idx, Rd.loOrHiReg); 
						printRegistersState();
					}break;
					case 0xF:{
						switch(bH){
							case 0x0:{
								printInstruction("%04x - DAS\n", pc);
								return 1; // UNIMPLEMENTED
							}break;
							case 0x8:
							case 0x9:
							case 0xA:
							case 0xB:
							case 0xC:
							case 0xD:
							case 0xE:
							case 0xF:{ // CMP.l ERs, ERd
								struct RegRef32 Rs = getRegRef32(bH);
								struct RegRef32 Rd = getRegRef32(bL);

								setFlagsSUB(*Rd.ptr, *Rs.ptr, 32);

								printInstruction("%04x - CMP.l ER%d, ER%d\n", pc, Rs.idx,  Rd.idx); 
								printRegistersState();
							}break;
							default:{
								return 1;
							} break;
						}
					}break;
					default:{
						return 1;
					} break;
				}
			}break;

			case 0x20 ... 0x2F:{ // MOV.B @aa:8, Rd
				uint32_t address = (b & 0x000000FF) | 0x00FFFF00; // Upper 16 bits assumed to be 1
				uint8_t value = getMemory8(address);

				struct RegRef8 Rd = getRegRef8(aL);
				setFlagsMOV(value, 8);
				*Rd.ptr = value;

				printInstruction("%04x - MOV.b @%x:8, R%d%c\n", pc, address, Rd.idx, Rd.loOrHiReg);
				printRegistersState();


			}break;
			case 0x30 ... 0x3F:{ // MOV.B Rs, @aa:8
				uint32_t address = (b & 0x000000FF) | 0x00FFFF00; // Upper 16 bits assumed to be 1

				struct RegRef8 Rs = getRegRef8(aL);
				uint8_t value = *Rs.ptr;
				setFlagsMOV(value, 8);
				setMemory8(address, value);

				printInstruction("%04x - MOV.b R%d%c,@%x:8 \n", pc, Rs.idx, Rs.loOrHiReg, address);
				printMemory(address, 1);
				printRegistersState();



			}break;
			case 0x40:{ // BRA d:8
				printInstruction("%04x - BRA %d:8\n", pc, (int8_t)b);
				pc += (int8_t)b;
			}break;
			case 0x41:{ // BRN - Unused in the ROM
				printInstruction("%04x - BRN %d:8\n", pc, (int8_t)b);
				return 1; // UNIMPLEMENTED
			}break;
			case 0x42:{ // BHI d:8
				printInstruction("%04x - BHI %d:8\n", pc, (int8_t)b);
				if(!(flags.C | flags.Z)){ pc += (int8_t)b; }
			}break;
			case 0x43:{ // BLS d:8
				printInstruction("%04x - BLS %d:8\n", pc, (int8_t)b);
				if(flags.C | flags.Z){ pc += (int8_t)b; }
			}break;
			case 0x44:{ // BCC d:8
				printInstruction("%04x - BCC %d:8\n", pc, (int8_t)b);
				if(!flags.C){ pc += (int8_t)b; }
			}break;
			case 0x45:{ // BCS d:8
				printInstruction("%04x - BCS %d:8\n", pc, (int8_t)b);
				if(flags.C){ pc += (int8_t)b; }
			}break;
			case 0x46:{ // BNE d:8
				printInstruction("%04x - BNE %d:8\n", pc, (int8_t)b);
				if(!flags.Z){ pc += (int8_t)b; }
			}break;
			case 0x47:{ // BEQ d:8
				printInstruction("%04x - BEQ %d:8\n", pc, (int8_t)b);
				if(flags.Z){ pc += (int8_t)b; }
			}break;
			case 0x48:{ // BVC d:8
				printInstruction("%04x - BVC %d:8\n", pc, (int8_t)b);
				if(!flags.V){ pc += (int8_t)b; }
			}break;
			case 0x49:{ // BVS d:8
				printInstruction("%04x - BVS %d:8\n", pc, (int8_t)b);
				if(flags.V){ pc += (int8_t)b; }
			}break;
			case 0x4A:{ // BPL d:8
				printInstruction("%04x - BPL %d:8\n", pc, (int8_t)b);
				if(!flags.N){ pc += (int8_t)b; }
			}break;
			case 0x4B:{ // BMI d:8
				printInstruction("%04x - BMI %d:8\n", pc, (int8_t)b);
				if(flags.N){ pc += (int8_t)b; }
			}break;
			case 0x4C:{ // BGE d:8
				printInstruction("%04x - BGE %d:8\n", pc, (int8_t)b);
				if(!(flags.N ^ flags.V)){ pc += (int8_t)b; }
			}break;
			case 0x4D:{ // BLT d:8
				printInstruction("%04x - BLT %d:8\n", pc, (int8_t)b);
				if(flags.N ^ flags.V){ pc += (int8_t)b; }
			}break;
			case 0x4E:{ // BGT d:8
				printInstruction("%04x - BGT %d:8\n", pc, (int8_t)b);
				if(!(flags.Z | (flags.N ^ flags.V))){ pc += (int8_t)b; }
			}break;
			case 0x4F:{ // BLE d:8
				printInstruction("%04x - BLE %d:8\n", pc, (int8_t)b);
				if(flags.Z | (flags.N ^ flags.V)){ pc += (int8_t)b; }
			}break;
			case 0x50:{ // MULXU B Rs, Rd
				struct RegRef8 Rs = getRegRef8(bH);
				struct RegRef16 Rd = getRegRef16(bL);
				uint8_t lowerBitsRd = *Rd.ptr & 0x00FF;
				*Rd.ptr = *Rs.ptr * lowerBitsRd;
				printInstruction("%04x - MULXU B r%d%c, %c%d\n", pc, Rs.idx, Rs.loOrHiReg, Rd.loOrHiReg, Rd.idx);
				printRegistersState();
			}break;
			case 0x51:{ // DIVXU B Rs, Rd
				struct RegRef8 Rs = getRegRef8(bH);
				struct RegRef16 Rd = getRegRef16(bL);
				uint8_t quotient = *Rd.ptr / *Rs.ptr;
				uint8_t remainder = *Rd.ptr % *Rs.ptr;
				*Rd.ptr = (remainder << 8) | quotient;
				flags.Z = (*Rs.ptr == 0) ? 1 : 0;
				flags.N = (*Rs.ptr & 0x8000) ? 1 : 0;
				printInstruction("%04x - DIVXU B r%d%c, %c%d\n", pc, Rs.idx, Rs.loOrHiReg, Rd.loOrHiReg, Rd.idx);
				printRegistersState();
			}break;
			case 0x52:{ // MULXU W Rs, Rd
				struct RegRef16 Rs = getRegRef16(bH);
				struct RegRef32 Rd = getRegRef32(bL);
				uint16_t lowerBitsRd = *Rd.ptr & 0x0000FFFF;
				*Rd.ptr = *Rs.ptr * lowerBitsRd;
				printInstruction("%04x - MULXU W %c%d, er%d\n", pc, Rs.loOrHiReg, Rs.idx, Rd.idx);
				printRegistersState();
			}break;
			case 0x53:{ // DIVXU W Rs, Rd
				struct RegRef16 Rs = getRegRef16(bH);
				struct RegRef32 Rd = getRegRef32(bL);
				uint16_t quotient = *Rd.ptr / *Rs.ptr;
				uint16_t remainder = *Rd.ptr % *Rs.ptr;
				*Rd.ptr = (remainder << 16) | quotient;
				flags.Z = (*Rs.ptr == 0) ? 1 : 0;
				flags.N = (*Rs.ptr & 0x80000000) ? 1 : 0;
				printInstruction("%04x - DIVXU W %c%d, er%d\n", pc, Rs.loOrHiReg, Rs.idx, Rd.idx);
				printRegistersState();
			}break;
			case 0x54:{ // RTS
				printInstruction("%04x - RTS\n", pc);
				pc = getMemory16(*SP) - 2;
				*SP += 2;
				printRegistersState();
			}break;
			case 0x55:{ // BSR d:8
				int8_t disp = b;
				printInstruction("%04x - BSR @%d:8\n", pc, disp);
				*SP -= 2;
				setMemory16(*SP, pc + 2);
				pc = pc + disp;
				printMemory(*SP, 2);
				printRegistersState();
			}break;
			case 0x56:{ // RTE
				interruptPopContext();
				printInstruction("%04x - RTE\n", pc);
			}break;
			case 0x57:{ // TRAPA
				printInstruction("%04x - TRAPA\n", pc);
				return 1; // UNIMPLEMENTED
			}break;
			case 0x58:{ // Bcc d:16 group
				int16_t disp = cd;
				switch(bH){
					case 0x0:{
						printInstruction("%04x - BRA %d:16\n", pc, disp);
						pc += 2 + disp;
					}break;
					case 0x1:{ // Unused in the ROM
						printInstruction("%04x - BRN %d:16\n", pc, disp);
					}break;
					case 0x2:{
						printInstruction("%04x - BHI %d:16\n", pc, disp);
						if(!(flags.C | flags.Z)){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0x3:{
						printInstruction("%04x - BLS %d:16\n", pc, disp);
						if(flags.C | flags.Z){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0x4:{
						printInstruction("%04x - BCC %d:16\n", pc, disp);
						if(!flags.C){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0x5:{
						printInstruction("%04x - BCS %d:16\n", pc, disp);
						if(flags.C){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0x6:{
						printInstruction("%04x - BNE %d:16\n", pc, disp);
						if(!flags.Z){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0x7:{
						printInstruction("%04x - BEQ %d:16\n", pc, disp);
						if(flags.Z){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0x8:{
						printInstruction("%04x - BVC %d:16\n", pc, disp);
						if(!flags.V){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0x9:{
						printInstruction("%04x - BVS %d:16\n", pc, disp);
						if(flags.V){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0xA:{
						printInstruction("%04x - BPL %d:16\n", pc, disp);
						if(!flags.N){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0xB:{
						printInstruction("%04x - BMI %d:16\n", pc, disp);
						if(flags.N){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0xC:{
						printInstruction("%04x - BGE %d:16\n", pc, disp);
						if(!(flags.N ^ flags.V)){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0xD:{
						printInstruction("%04x - BLT %d:16\n", pc, disp);
						if(flags.N ^ flags.V){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0xE:{
						printInstruction("%04x - BGT %d:16\n", pc, disp);
						if(!(flags.Z | (flags.N ^ flags.V))){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					case 0xF:{
						printInstruction("%04x - BLE %d:16\n", pc, disp);
						if(flags.Z | (flags.N ^ flags.V)){ pc += 2 + disp; }else{ pc += 2; }
					}break;
					default:{ return 1; } break;
				}
			}break;
			case 0x59:{ // JMP @ERn
				struct RegRef32 Er = getRegRef32(bH);
				printInstruction("%04x - JMP @ER%d\n", pc, Er.idx);
				pc = (*Er.ptr & 0x0000FFFF) - 2;
			}break;
			case 0x5A:{ // JMP @aa:24
				uint32_t address = (b << 16) | cd;
				printInstruction("%04x - JMP @0x%04x:24\n", pc, address);
				pc = address - 2;
			}break;
			case 0x5B:{ // JMP @@aa:8 - UNUSED IN THE ROM
				printInstruction("%04x - ????\n", pc);
			}break;
			case 0x5C:{ // BSR d:16
				int16_t disp = cd;
				printInstruction("%04x - BSR @%d:16\n", pc, disp);
				*SP -= 2;
				setMemory16(*SP, pc + 4);
				pc = pc + 2 + disp;
				printMemory(*SP, 2);
				printRegistersState();
			}break;
			case 0x5D:{ // JSR @ERn
				struct RegRef32 Er = getRegRef32(bH);
				*SP -= 2;
				setMemory16(*SP, pc + 2);
				printInstruction("%04x - JSR @ER%d\n", pc, Er.idx);
				pc = (*Er.ptr & 0x0000FFFF) - 2;
				printMemory(*SP, 2);
				printRegistersState();
			}break;
			case 0x5E:{ // JSR @aa:24
				uint32_t address = (b << 16) | cd;
				*SP -= 2;
				setMemory16(*SP, pc + 4);
				printInstruction("%04x - JSR @0x%04x:24\n", pc, address);
				pc = address - 2;
				printMemory(*SP, 2);
				printRegistersState();
			}break;
			case 0x5F:{ // JSR @@aa:8 - UNUSED IN THE ROM
				printInstruction("%04x - ????\n", pc);
			}break;
			case 0x60 ... 0x6F:{
				switch(aL){
					case 0x0:{ // BSET Rn, Rd

						struct RegRef8 Rd = getRegRef8(bL);		
						struct RegRef8 Rn = getRegRef8(bH);		
						int bitToSet = *Rn.ptr;

						*Rd.ptr = *Rd.ptr | (1 << bitToSet);

						printInstruction("%04x - BSET r%d%c, r%d%c\n", pc, Rn.idx, Rn.loOrHiReg, Rd.idx, Rd.loOrHiReg);
						printRegistersState();
					}break;
					case 0x1:{
						printInstruction("%04x - BNOT\n", pc);
						return 1; // UNUSED IN THE ROM
					}break;
					case 0x2:{ // BCLR Rn, Rd
						struct RegRef8 Rd = getRegRef8(bL);		
						struct RegRef8 Rn = getRegRef8(bH);		
						int bitToClear = *Rn.ptr;

						*Rd.ptr = *Rd.ptr & ~(1 << bitToClear);

						printInstruction("%04x - BCLR r%d%c, r%d%c\n", pc, Rn.idx, Rn.loOrHiReg, Rd.idx, Rd.loOrHiReg);
						printRegistersState();
					}break;
					case 0x3:{
						printInstruction("%04x - BTST\n", pc);
						return 1; // UNIMPLEMENTED
					}break;
					case 0x4:{ // OR.w Rs, Rd
					struct RegRef16 Rd = getRegRef16(bL);
					struct RegRef16 Rs = getRegRef16(bH);
						uint16_t newValue = *Rs.ptr | *Rd.ptr;
						setFlagsMOV(newValue, 16);
						*Rd.ptr = newValue;

						printInstruction("%04x - OR.w %c%d,%c%d\n", pc, Rs.loOrHiReg, Rs.idx, Rd.loOrHiReg,  Rd.idx); 
						printRegistersState();
					}break;
					case 0x5:{ // XOR.w Rs, Rd
					struct RegRef16 Rd = getRegRef16(bL);
					struct RegRef16 Rs = getRegRef16(bH);
						uint16_t newValue = *Rs.ptr ^ *Rd.ptr;
						setFlagsMOV(newValue, 16);
						*Rd.ptr = newValue;

						printInstruction("%04x - XOR.w %c%d,%c%d\n", pc, Rs.loOrHiReg, Rs.idx, Rd.loOrHiReg,  Rd.idx); 
						printRegistersState();
					}break;
					case 0x6:{ // AND.w Rs, Rd
					struct RegRef16 Rd = getRegRef16(bL);
					struct RegRef16 Rs = getRegRef16(bH);
						uint16_t newValue = *Rs.ptr & *Rd.ptr;
						setFlagsMOV(newValue, 16);
						*Rd.ptr = newValue;

						printInstruction("%04x - AND.w %c%d,%c%d\n", pc, Rs.loOrHiReg, Rs.idx, Rd.loOrHiReg,  Rd.idx); 
						printRegistersState();
					}break;
					case 0x7:{ // BST / BIST #xx:3, Rd — Rd.bit = (~)C
						/* Register-direct form: the destination is the 8-bit
						 * register itself. bH bit3 selects the BIST form. */
						struct RegRef8 Rd = getRegRef8(bL);
						int b3 = bH & 0x7;
						bool v = flags.C ? true : false;
						if (bH & 0x8) v = !v;        /* BIST: store ~C */
						if (v) *Rd.ptr |=  (uint8_t)(1 << b3);
						else   *Rd.ptr &= (uint8_t)~(1 << b3);
						printInstruction("%04x - B%sST #%d, r%d%c\n", pc,
						                 (bH & 0x8) ? "I" : "", b3, Rd.idx, Rd.loOrHiReg);
						printRegistersState();
					}break;
					case 0x8:{ 
						if(!(b & 0x80)){ // MOV.B @ERs, Rd
							struct RegRef32 Rs = getRegRef32(bH);
							struct RegRef8 Rd = getRegRef8(bL);

							uint8_t value = getMemory8(*Rs.ptr);

							setFlagsMOV(value, 8);
							*Rd.ptr = value;

							printInstruction("%04x - MOV.b @ER%d, R%d%c\n", pc, Rs.idx, Rd.idx, Rd.loOrHiReg); 
						} else{// MOV.B Rs, @ERd 
							struct RegRef8 Rs = getRegRef8(bL);
							struct RegRef32 Rd = getRegRef32(bH);

							uint8_t value = *Rs.ptr;

							setFlagsMOV(value, 8);
							setMemory8(*Rd.ptr, value);
							printInstruction("%04x - MOV.b R%d%c, @ER%d, \n", pc, Rs.idx, Rs.loOrHiReg, Rd.idx);
							printMemory(*Rd.ptr, 1);
						}
						printRegistersState();
					}break;
					case 0x9:{ 
						if(!(b & 0x80)){ // MOV.w @ERs, Rd
							struct RegRef32 Rs = getRegRef32(bH);
							struct RegRef16 Rd = getRegRef16(bL);
							uint16_t value = getMemory16(*Rs.ptr);
							setFlagsMOV(value, 16);
							*Rd.ptr = value;
							printInstruction("%04x - MOV.w @ER%d, %c%d\n", pc, Rs.idx, Rd.loOrHiReg, Rd.idx ); 
						} else{ // MOV.w Rs, @ERd 
							struct RegRef16 Rs = getRegRef16(bL);
							struct RegRef32 Rd = getRegRef32(bH);
							uint16_t value = *Rs.ptr;
							setFlagsMOV(value, 16);
							setMemory16(*Rd.ptr, value);
							printInstruction("%04x - MOV.w R%d%c, @ER%d, \n", pc, Rs.idx, Rs.loOrHiReg, Rd.idx);
							printMemory(*Rd.ptr, 2);
						}
						printRegistersState();

					} break;
					case 0xA:{ 
						switch(bH){
							case 0x0:{ // MOV.B @aa:16, Rd
								uint32_t address = (cd & 0x0000FFFF) | 0x00FF0000; // Upper 16 bits assumed to be 1
								uint8_t value = getMemory8(address);

								struct RegRef8 Rd = getRegRef8(bL);

								setFlagsMOV(value, 8);
								*Rd.ptr = value;


								if(address == 0xfff0e9){ // SSSRDR
									*SSU.SSSR = clearBit8(*SSU.SSSR, 1);
								}

								printInstruction("%04x - MOV.b @%x:16, R%d%c\n", pc, address, Rd.idx, Rd.loOrHiReg); 
								printRegistersState();

							}break;

							case 0x8:{ // MOV.B Rs, @aa:16 
								uint32_t address = (cd & 0x0000FFFF) | 0x00FF0000; // Upper 16 bits assumed to be 1

								struct RegRef8 Rs = getRegRef8(bL);

								uint8_t value = *Rs.ptr;
								setFlagsMOV(value, 8);
								setMemory8(address, value);

								if(address == 0xfff0eb){ // SSSTDR
									*SSU.SSSR = clearBit8(*SSU.SSSR, 2); // TDRE
									*SSU.SSSR = clearBit8(*SSU.SSSR, 3); // TEND
								} else if(address == 0xfff0d1){ // TMRB_TCB1_TLB1
									TimerB.TLBvalue = value; // TODO (if handling custom ROMs) add these checks in the other MOVs
								}

								printInstruction("%04x - MOV.b R%d%c,@%x:16 \n", pc, Rs.idx, Rs.loOrHiReg, address); 
								printMemory(address, 1);
								printRegistersState();

							}break;
							default:{
								return 1;
							} break;
						}
						pc+=2;

					}break;
					case 0xB:{
						switch(bH){
							case 0x0:{ // MOV.w @aa:16, Rd
								uint32_t address = (cd & 0x0000FFFF) | 0x00FF0000; // Upper 16 bits assumed to be 1
								uint16_t value = getMemory16(address);

								struct RegRef16 Rd = getRegRef16(bL);

								setFlagsMOV(value, 16);
								*Rd.ptr = value;

								printInstruction("%04x - MOV.w @%x:16, %c%d\n", pc, address, Rd.loOrHiReg, Rd.idx); 
								printRegistersState();

							}break;

							case 0x8:{ // MOV.w Rs, @aa:16 
								uint32_t address = (cd & 0x0000FFFF) | 0x00FF0000; // Upper 16 bits assumed to be 1

								struct RegRef16 Rs = getRegRef16(bL);

								uint16_t value = *Rs.ptr;
								setFlagsMOV(value, 16);
								setMemory16(address, value);

								printInstruction("%04x - MOV.w %c%d,@%x:16 \n", pc, Rs.loOrHiReg, Rs.idx, address); 
								printMemory(address, 2);
								printRegistersState();

							}break;
							default:{
								return 1;
							} break;
						}
						pc+=2;

					}break;
					case 0xC:{ // MOV.B @ERs+, Rd --- MOV.B Rs, @-ERd
						char incOrDec = (bH & 0b1000) ? '-' : '+';

						if (incOrDec == '+'){
							struct RegRef32 Rs = getRegRef32(bH);
							struct RegRef8 Rd = getRegRef8(bL);

							uint8_t value = getMemory8(*Rs.ptr);

							*Rs.ptr += 1;

							setFlagsMOV(value, 8);
							*Rd.ptr = value;

							printInstruction("%04x - MOV.b @ER%d+, R%d%c\n", pc, Rs.idx, Rd.idx, Rd.loOrHiReg); 

						} else{
							struct RegRef32 Rd = getRegRef32(bH);
							struct RegRef8 Rs = getRegRef8(bL);

							*Rd.ptr -= 1;


							uint8_t value = *Rs.ptr;
							setMemory8(*Rs.ptr, value);
							setFlagsMOV(value, 8);

							printInstruction("%04x - MOV.b R%d%c, @-ER%d, \n", pc, Rs.idx, Rs.loOrHiReg, Rd.idx); 
							printMemory(*Rd.ptr, 1);

						}
						printRegistersState();



					}break;
					case 0xD:{ // MOV.w @ERs+, Rd --- MOV.w Rs, @-ERd
						char incOrDec = (bH & 0b1000) ? '-' : '+';

						if (incOrDec == '+'){
							struct RegRef32 Rs = getRegRef32(bH);
							struct RegRef16 Rd = getRegRef16(bL);

							uint16_t value = getMemory16(*Rs.ptr);

							*Rs.ptr += 2;

							setFlagsMOV(value, 16);
							*Rd.ptr = value;

							printInstruction("%04x - MOV.w @ER%d+, %c%d\n", pc, Rs.idx, Rd.loOrHiReg, Rd.idx); 

						} else{
							struct RegRef32 Rd = getRegRef32(bH);
							struct RegRef16 Rs = getRegRef16(bL);

							*Rd.ptr -= 2;

							uint16_t value = *Rs.ptr;
							setMemory16(*Rd.ptr, value);
							setFlagsMOV(value, 16);

							printInstruction("%04x - MOV.w %c%d, @-ER%d, \n", pc, Rs.loOrHiReg, Rs.idx, Rd.idx); 
							printMemory(*Rd.ptr, 2);
						}
						printRegistersState();
					} break;
					case 0xE:{ 
						struct RegRef8 Rd = getRegRef8(bL);
					struct RegRef32 Rs = getRegRef32(bH);

						uint16_t disp = cd;
						bool msbDisp = disp & 0x8000;
						uint32_t signExtendedDisp = msbDisp ? (0xFFFF0000 | disp) : disp;

						if (!(bH & 0b1000)){ // From memory MOV.B @(d:16, ERs), Rd
							uint8_t value = getMemory8(*Rs.ptr + signExtendedDisp);
							*Rd.ptr = value;
							setFlagsMOV(value, 8);

							printInstruction("%04x - MOV.b @(%d:16, ER%d), R%d%c\n", pc, disp, Rs.idx, Rd.idx, Rd.loOrHiReg); 

						} else{ // To memory MOV.B Rs, @(d:16, ERd)
							uint8_t value = *Rd.ptr;
							setFlagsMOV(value, 8);
							setMemory8(*Rs.ptr + signExtendedDisp, value);
							printInstruction("%04x - MOV.b R%d%c, @(%d:16, ER%d), \n", pc, Rd.idx, Rd.loOrHiReg, disp, Rs.idx); 
							printMemory(*Rs.ptr + signExtendedDisp, 1);
						}
						printRegistersState();
						pc+=2;
					}break;
					case 0xF:{ 
						struct RegRef16 Rd = getRegRef16(bL);
					struct RegRef32 Rs = getRegRef32(bH);
						uint16_t disp = cd;
						bool msbDisp = disp & 0x8000;
						uint32_t signExtendedDisp = msbDisp ? (0xFFFF0000 | disp) : disp;

						if (!(bH & 0b1000)){ // From memory MOV.W @(d:16, ERs), Rd
							uint16_t value = getMemory16(*Rs.ptr + signExtendedDisp);
							*Rd.ptr = value;
							setFlagsMOV(value, 16);

							printInstruction("%04x - MOV.w @(%d:16, ER%d), %c%d\n", pc, disp, Rs.idx, Rd.loOrHiReg, Rd.idx); 

						} else{ // To memory MOV.W Rs, @(d:16, ERd)
							uint16_t value = *Rd.ptr;
							setFlagsMOV(value, 16);
							setMemory16(*Rs.ptr + signExtendedDisp, value);
							printInstruction("%04x - MOV.w %c%d, @(%d:16, ER%d), \n", pc, Rd.loOrHiReg, Rd.idx, disp, Rs.idx); 
							printMemory(*Rs.ptr + signExtendedDisp, 2);
						}
						printRegistersState();
						pc+=2;


					}break;
					default:{
						return 1;
					} break;
				}
			}break;
			case 0x70 ... 0x7F:{
				uint8_t mostSignificantBit = bH >> 7;
				switch(aL){
					case 0x0:{ // BSET #xx:3, Rd

						struct RegRef8 Rd = getRegRef8(bL);		

						int bitToSet = bH;

						*Rd.ptr = *Rd.ptr | (1 << bitToSet);

						printInstruction("%04x - BSET #%d, r%d%c\n", pc, bitToSet, Rd.idx, Rd.loOrHiReg);
						printRegistersState();
					}break;
					case 0x1:{ // BNOT #xx:3, Rd  (71 N0) — toggle bit n
						struct RegRef8 Rd = getRegRef8(bL);
						int bitToInvert = bH;
						*Rd.ptr = (uint8_t)(*Rd.ptr ^ (1 << bitToInvert));
						printInstruction("%04x - BNOT #%d, r%d%c\n", pc, bitToInvert, Rd.idx, Rd.loOrHiReg);
						printRegistersState();
					}break;
					case 0x2:{ // BCLR #xx:3, Rd

						struct RegRef8 Rd = getRegRef8(bL);		

						int bitToClear = bH;

						*Rd.ptr = *Rd.ptr & ~(1 << bitToClear);

						printInstruction("%04x - BCLR #%d, r%d%c\n", pc, bitToClear, Rd.idx, Rd.loOrHiReg);
						printRegistersState();
					}break;
					case 0x3:{
						struct RegRef8 Rd = getRegRef8(bL);
						int bitToTest = bH;
					flags.Z = !(*Rd.ptr & (1<<bitToTest));
						printInstruction("%04x - BTST #%d, r%d%c\n", pc, bitToTest, Rd.idx, Rd.loOrHiReg);
					}break;
					/* C-flag bit ops, register direct (7N bH bL): bit number is
					 * bH&7; bH bit3 selects the inverted (BIxx) form. gcc emits
					 * these when fusing single-bit merges (bld/bor/bst...). */
					case 0x4:{ // BOR / BIOR #xx:3, Rd — C |= (~)bit
						struct RegRef8 Rd = getRegRef8(bL);
						int b3 = bH & 0x7;
						bool bit = (*Rd.ptr >> b3) & 1;
						if (bH & 0x8) bit = !bit;
						flags.C = flags.C || bit;
						printInstruction("%04x - B%sOR #%d, r%d%c\n", pc,
						                 (bH & 0x8) ? "I" : "", b3, Rd.idx, Rd.loOrHiReg);
						printRegistersState();
					}break;
					case 0x5:{ // BXOR / BIXOR #xx:3, Rd — C ^= (~)bit
						struct RegRef8 Rd = getRegRef8(bL);
						int b3 = bH & 0x7;
						bool bit = (*Rd.ptr >> b3) & 1;
						if (bH & 0x8) bit = !bit;
						flags.C = (flags.C ? true : false) != bit;
						printInstruction("%04x - B%sXOR #%d, r%d%c\n", pc,
						                 (bH & 0x8) ? "I" : "", b3, Rd.idx, Rd.loOrHiReg);
						printRegistersState();
					}break;
					case 0x6:{ // BAND / BIAND #xx:3, Rd — C &= (~)bit
						struct RegRef8 Rd = getRegRef8(bL);
						int b3 = bH & 0x7;
						bool bit = (*Rd.ptr >> b3) & 1;
						if (bH & 0x8) bit = !bit;
						flags.C = flags.C && bit;
						printInstruction("%04x - B%sAND #%d, r%d%c\n", pc,
						                 (bH & 0x8) ? "I" : "", b3, Rd.idx, Rd.loOrHiReg);
						printRegistersState();
					}break;
					case 0x7:{ // BLD / BILD #xx:3, Rd — C = (~)bit
						struct RegRef8 Rd = getRegRef8(bL);
						int b3 = bH & 0x7;
						bool bit = (*Rd.ptr >> b3) & 1;
						if (bH & 0x8) bit = !bit;
						flags.C = bit;
						printInstruction("%04x - B%sLD #%d, r%d%c\n", pc,
						                 (bH & 0x8) ? "I" : "", b3, Rd.idx, Rd.loOrHiReg);
						printRegistersState();
					}break;
					case 0x8:{
					printInstruction("%04x - MOV\n", pc);
						return 1; // UNIMPLEMENTED
					}break;
					case 0x9:{ // XXX.w #xx:16, Rd
					struct RegRef16 Rd = getRegRef16(bL);
						switch(bH){ 
							case 0x0:{ // MOV.w #xx:16, Rd
								setFlagsMOV(cd, 16);
								*Rd.ptr = cd;
								printInstruction("%04x - MOV.w 0x%x,%c%d\n", pc, cd, Rd.loOrHiReg,  Rd.idx); 
							}break;
							case 0x1:{ // ADD.w #xx:16, Rd
								setFlagsADD(*Rd.ptr, cd, 16);
								*Rd.ptr += cd;
								printInstruction("%04x - ADD.w 0x%x,%c%d\n", pc, cd, Rd.loOrHiReg,  Rd.idx); 
							}break;
							case 0x2:{ // CMP.w #xx:16, Rd
								setFlagsSUB(*Rd.ptr, cd, 16);
								printInstruction("%04x - CMP.w 0x%x,%c%d\n", pc, cd, Rd.loOrHiReg,  Rd.idx); 
							}break;
							case 0x3:{ // SUB.w #xx:16, Rd
								setFlagsSUB(*Rd.ptr, cd, 16);
								*Rd.ptr -= cd;
								printInstruction("%04x - SUB.w 0x%x,%c%d\n", pc, cd, Rd.loOrHiReg,  Rd.idx); 
							}break;
							case 0x4:{ // OR.w #xx:16, Rd
								uint16_t value = cd;
								uint16_t newValue = cd | *Rd.ptr;
								setFlagsMOV(newValue, 16);
								*Rd.ptr = newValue;
								printInstruction("%04x - OR.w 0x%x,%c%d\n", pc, cd, Rd.loOrHiReg,  Rd.idx); 
							}break;
							case 0x5:{ // XOR.w #xx:16, Rd
								uint16_t value = cd;
								uint16_t newValue = cd ^ *Rd.ptr;
								setFlagsMOV(newValue, 16);
								*Rd.ptr = newValue;
								printInstruction("%04x - XOR.w 0x%x,%c%d\n", pc, cd, Rd.loOrHiReg,  Rd.idx); 
							}break;
							case 0x6:{ // AND.w #xx:16, Rd
								uint16_t value = cd;
								uint16_t newValue = cd & *Rd.ptr;
								setFlagsMOV(newValue, 16);
								*Rd.ptr = newValue;
								printInstruction("%04x - AND.w 0x%x,%c%d\n", pc, cd, Rd.loOrHiReg,  Rd.idx); 
							}break;
							default:{
								return 1;
							} break;
						}
						printRegistersState();
						pc+=2;
					}break;
					case 0xA:{ // XXX.l #xx:32, ERd
					struct RegRef32 Rd = getRegRef32(bL);
						switch(bH){ 
							case 0x0:{ // MOV.l #xx:32, ERd
								setFlagsMOV(cdef, 32);
								*Rd.ptr = cdef;
								printInstruction("%04x - MOV.l 0x%04x, ER%d\n", pc, cdef,  Rd.idx); 
							}break;
							case 0x1:{ // ADD.l #xx:32, ERd
								setFlagsADD(*Rd.ptr, cdef, 32);
								*Rd.ptr += cdef;
								printInstruction("%04x - ADD.l 0x%04x, ER%d\n", pc, cdef,  Rd.idx); 
							}break;
							case 0x2:{
								// CMP.l #xx:32, ERd
								setFlagsSUB(*Rd.ptr, cdef, 32);
								printInstruction("%04x - CMP.l 0x%04x, ER%d\n", pc, cdef,  Rd.idx); 
							}break;
							case 0x3:{ // SUB.l #xx:32, ERd
								setFlagsSUB(*Rd.ptr, cdef, 32);
								*Rd.ptr -= cdef;
								printInstruction("%04x - SUB.l 0x%04x, ER%d\n", pc, cdef,  Rd.idx); 
							}break;
							case 0x4:{ // OR.l #xx:32, ERd
								uint32_t newValue = cdef | *Rd.ptr;
								setFlagsMOV(newValue, 32);
								*Rd.ptr = newValue;
								printInstruction("%04x - OR.l 0x%04x, ER%d\n", pc, cdef,  Rd.idx); 
							}break;
							case 0x5:{ // XOR.l #xx:32, ERd
								uint32_t newValue = cdef ^ *Rd.ptr;
								setFlagsMOV(newValue, 32);
								*Rd.ptr = newValue;
								printInstruction("%04x - XOR.l 0x%04x, ER%d\n", pc, cdef,  Rd.idx); 
							}break;
							case 0x6:{ // AND.l #xx:32, ERd
								uint32_t newValue = cdef & *Rd.ptr;
								setFlagsMOV(newValue, 32);
								*Rd.ptr = newValue;
								printInstruction("%04x - AND.l 0x%04x, ER%d\n", pc, cdef,  Rd.idx); 
							}break;
							default:{
								return 1;
							} break;
						}
						printRegistersState();
						pc+=4;
					}break;
					case 0xB:{
						printInstruction("%04x - EEPMOV\n", pc);
						return 1; // UNIMPLEMENTED
					}break;
					case 0xC:{
						uint8_t mostSignificantBit = dH >> 7;
						switch(c){
							case 0x77:{
								// BLD #xx:3, @ERd
								struct RegRef32 Rd = getRegRef32(bH);		
								int bitToLoad = dH;
								printInstruction("%04x - BLD #%d, @ER%d\n", pc, bitToLoad, Rd.idx);
								flags.C = getMemory8(*Rd.ptr) & (1 << bitToLoad);
								printRegistersState();
								pc+=2;

							}break;
							default:{
								return 1;
							} break;

						}
					} break;
					case 0xE:{
						if (cH == 0x6){
							switch(cL){
								case 0x3:{
									printInstruction("%04x - BTST\n", pc);
									return 1; // UNIMPLEMENTED
								}break;
								default:{
									return 1;
								} break;
							}
						}else if (cH == 0x7){
							uint8_t mostSignificantBit = dH >> 7;
							switch(cL){
								case 0x3:{
									printInstruction("%04x - BTST\n", pc);
									return 1; // UNIMPLEMENTED
								}break;
								case 0x4:{
									if (mostSignificantBit == 0x1){
										printInstruction("%04x - BIOR\n", pc);
										return 1; // UNIMPLEMENTED
									}else{
										printInstruction("%04x - BOR\n", pc);
										return 1; // UNIMPLEMENTED
									}
								}break;
								case 0x5:{
									if (mostSignificantBit == 0x1){
										printInstruction("%04x - BXOR\n", pc);
										return 1; // UNIMPLEMENTED
									}else{
										printInstruction("%04x - BIXOR\n", pc);
										return 1; // UNIMPLEMENTED
									}
								}break;
								case 0x6:{
									if (mostSignificantBit == 0x1){
										printInstruction("%04x - BIAND\n", pc);
										return 1; // UNIMPLEMENTED
									}else{
										printInstruction("%04x - BAND\n", pc);
										return 1; // UNIMPLEMENTED
									}
								}break;
								case 0x7:{
									if (mostSignificantBit == 0x1){
										printInstruction("%04x - BILD\n", pc);
									}else{ // BLD #xx:3, @ERd
										int bitToLoad = dH;
										uint32_t address = (0x0000FF00) | b;
										printInstruction("%04x - BLD #%d, @0x%x:8\n", pc, bitToLoad, address);
										flags.C =  getMemory8(address) & (1 << bitToLoad);
									}
								}break;
								default:{
									return 1;
								} break;
							}


						}
						pc+=2; 
					}break;
					case 0xD:{
					struct RegRef32 Rd = getRegRef32(bH);		
						switch(c){
							case 0x70:{ // BSET #xx:3, @ERd
								int bitToSet = dH;
								printInstruction("%04x - BSET #%d, @ER%d\n", pc, bitToSet, Rd.idx);
								setMemory8(*Rd.ptr, getMemory8(*Rd.ptr) | (1 << bitToSet));
							}break;
							case 0x60:{ // BSET Rn, @ERd
								struct RegRef8 Rn = getRegRef8(dH);		
								int bitToSet = *Rn.ptr;
								printInstruction("%04x - BSET r%d%c, @ER%d\n", pc, Rn.idx, Rn.loOrHiReg, Rd.idx);
								setMemory8(*Rd.ptr, getMemory8(*Rd.ptr) | (1 << bitToSet));
							}break;
							case 0x71:{ // BNOT #xx:3, @ERd — toggle bit n
								int bitToInvert = dH;
								setMemory8(*Rd.ptr, getMemory8(*Rd.ptr) ^ (1 << bitToInvert));
								printInstruction("%04x - BNOT #%d, @ER%d\n", pc, bitToInvert, Rd.idx);
							}break;
							case 0x72:{ // BCLR #xx:3, @ERd
								int bitToClear = dH;
								printInstruction("%04x - BCLR #%d, @ER%d\n", pc, bitToClear, Rd.idx);
								setMemory8(*Rd.ptr, getMemory8(*Rd.ptr) & ~(1 << bitToClear));
							}break;
							case 0x62:{ // BCLR Rn, @ERd
								struct RegRef8 Rn = getRegRef8(dH);		
								int bitToClear = *Rn.ptr;
								printInstruction("%04x - BCLR r%d%c, @ER%d\n", pc, Rn.idx, Rn.loOrHiReg, Rd.idx);
								setMemory8(*Rd.ptr, getMemory8(*Rd.ptr) & ~(1 << bitToClear));
							}break;
							case 0x67:{ // BST ##xx:3, @ERd
								int bitToSet = dH;
								if (flags.C == 0){
									setMemory8(*Rd.ptr, getMemory8(*Rd.ptr) & ~(1 << bitToSet));
								} else{
								setMemory8(*Rd.ptr, getMemory8(*Rd.ptr) | (1 << bitToSet));
								}
								printInstruction("%04x - BST #%d, @ER%d\n", pc, bitToSet, Rd.idx);
							}break;
							default:{
								return 1;
							} break;
						}
						printMemory(*Rd.ptr, 1);
						printRegistersState();
						pc+=2;
					}break;
					case 0xF:{
						uint32_t address = (0x0000FF00) | b;
						switch(c){
							case 0x70:{ // BSET #xx:3, @aa:8
								int bitToSet = dH;
								printInstruction("%04x - BSET #%d, @0x%x:8\n", pc, bitToSet, address);
								setMemory8(address, getMemory8(address) | (1 << bitToSet));
							}break;
							case 0x60:{ // BSET Rn, @aa:8
								struct RegRef8 Rn = getRegRef8(dH);		
								int bitToSet = *Rn.ptr;
								printInstruction("%04x - BSET r%d%c, @0x%x:8\n", pc, Rn.idx, Rn.loOrHiReg, address);
								setMemory8(address, getMemory8(address) | (1 << bitToSet));
							}break;
							case 0x72:{ // BCLR #xx:3, @aa:8
								int bitToClear = dH;
								printInstruction("%04x - BCLR #%d, @0x%x:8\n", pc, bitToClear, address);
								setMemory8(address, getMemory8(address) & ~(1 << bitToClear));
							}break;
							case 0x62:{ // BCLR Rn, @aa:8
								struct RegRef8 Rn = getRegRef8(dH);		
								int bitToClear = *Rn.ptr;
								printInstruction("%04x - BCLR r%d%c, @0x%x:8\n", pc, Rn.idx, Rn.loOrHiReg, address);
								setMemory8(address, getMemory8(address) & ~(1 << bitToClear));

							}break;
							case 0x67:{ // BST - Unused in the ROM
								return 1;
							} break;
							default:{
								return 1;
							} break;
						}
						printMemory(address, 1);
						pc+=2;
					} break;
					default:{
						return 1;
					} break;
				}
			}break;
			case 0x80 ... 0x8F:{ // ADD.B #xx:8, Rd
				struct RegRef8 Rd = getRegRef8(aL);

				uint8_t value = (bH << 4) | bL;

				setFlagsADD(*Rd.ptr, value, 8);
				*Rd.ptr += value;

				printInstruction("%04x - ADD.b 0x%x,R%d%c\n", pc, value, Rd.idx, Rd.loOrHiReg); //Note: Dmitry's dissasembler sometimes outputs address in decimal (0xdd) not sure why
				printRegistersState();
			}break;
			case 0x90 ... 0x9F:{
				printInstruction("%04x - ADDX\n", pc);
				return 1; // UNIMPLEMENTED
			}break;

			case 0xA0 ... 0xAF:{ // CMP.B #xx:8, Rd
				struct RegRef8 Rd = getRegRef8(aL);

				uint8_t value = (bH << 4) | bL;

				setFlagsSUB(*Rd.ptr, value, 8);

				printInstruction("%04x - CMP.b 0x%x,R%d%c\n", pc, value, Rd.idx, Rd.loOrHiReg); 
				printRegistersState();

			}break;

			case 0xB0 ... 0xBF:{
				printInstruction("%04x - SUBX\n", pc);
				return 1; // UNIMPLEMENTED
			}break;

			case 0xC0 ... 0xCF:{ // OR.b #xx:8, Rd
				struct RegRef8 Rd = getRegRef8(aL);

				uint8_t value = b;
				uint8_t newValue = value | *Rd.ptr;
				setFlagsMOV(newValue, 8);
				*Rd.ptr = newValue;

				printInstruction("%04x - OR.b 0x%x,R%d%c\n", pc, value, Rd.idx, Rd.loOrHiReg); 
				printRegistersState();
			}break;

			case 0xD0 ... 0xDF:{ // XOR.b #xx:8, Rd
				struct RegRef8 Rd = getRegRef8(aL);

				uint8_t value = b;
				uint8_t newValue = value ^ *Rd.ptr;
				setFlagsMOV(newValue, 8);
				*Rd.ptr = newValue;

				printInstruction("%04x - XOR.b 0x%x,R%d%c\n", pc, value, Rd.idx, Rd.loOrHiReg); 
				printRegistersState();
			}break;

			case 0xE0 ... 0xEF:{ // AND #xx:8, Rd
				struct RegRef8 Rd = getRegRef8(aL);

				uint8_t value = b;
				uint8_t newValue = value & *Rd.ptr;
				setFlagsMOV(newValue, 8);
				*Rd.ptr = newValue;

				printInstruction("%04x - AND.b 0x%x,R%d%c\n", pc, value, Rd.idx, Rd.loOrHiReg); 
				printRegistersState();
			}break;

			case 0xF0 ... 0xFF:{ // MOV.B #xx:8, Rd
				struct RegRef8 Rd = getRegRef8(aL);

				uint8_t value = b;

				setFlagsMOV(value, 8);
				*Rd.ptr = value;

				printInstruction("%04x - MOV.b 0x%x,R%d%c\n", pc, value, Rd.idx, Rd.loOrHiReg); 
				printRegistersState();
			}break;

			default:{
				printInstruction("???\n");
				return 1; // UNIMPLEMENTED
			} break;
		}

		// SSU cleanup
		if((getMemory8(PORT9)) & ACCEL_PIN){ 
			accel.buffer.state = ACCEL_GETTING_ADDRESS;
			accel.buffer.offset = 0x0;
		}

		if((getMemory8(PORT1)) & EEPROM_PIN){ // TODO: can be optimized by checking when the pin gets sets instead of all the time
			eeprom.buffer.state = EEPROM_EMPTY;
			eeprom.buffer.offset = 0x0;
		}
		pc+=2;

	}
	// Interrupt handling
	// Note: Remember to check priorities when adding interrupt types here
	// TODO: this doesnt follow this rule: 3.8.4 Conflict between Interrupt Generation and Disabling
	if (!flags.I){
		if ((*IRQ_IRR1 & IRRI0) && (*IRQ_IENR1 & IEN0)){
			interruptPushContext();
			flags.I = true;
			pc = VECTOR_IRQ0;
			addElement(&inputQueue, ENTER);
			addElement(&inputQueue, 0);
			sleep = false;
		}
		else if (*IRQ_IENR1 & IENRTC){
			if (*RTCFLG & _025SEIFG){
				interruptPushContext();
				flags.I = true;
				pc = VECTOR_RTC_QUARTER_SEC;
				sleep = false;
			}
			else if (*RTCFLG & _05SEIFG){
				interruptPushContext();
				flags.I = true;
				pc = VECTOR_RTC_HALF_SEC;
				sleep = false;
			}
			else if (*RTCFLG & _1SEIFG){
				interruptPushContext();
				flags.I = true;
				pc = VECTOR_RTC_EVERY_SEC;
				sleep = false;
			}
		}
		else if ((*IRQ_IRR2 & IRRTB1) && (*IRQ_IENR2 & IENTB1)){
			interruptPushContext();
			flags.I = true;
			pc = VECTOR_TIMER_B1;
			sleep = false;
		}

	}

	// Clock handling
	static uint32_t subClockCountdown = SYSTEM_CLOCK_CYCLES_PER_SECOND / SUB_CLOCK_CYCLES_PER_SECOND;
	for(uint32_t i = 0; i < cycles; i++){
		*cycleCount += 1;
		// SSU
		if ((*cycleCount & 3) == 0){ // TODO(Custom ROMs): Parameterize
			if (~*SSU.SSER & TE){ // TE == 0
				*SSU.SSSR |= TDRE; // Set TDRE
			}

			if ((*SSU.SSER & (TE | RE)) == (TE | RE)){ // Transmission and recieve enabled
				if(~*SSU.SSSR & TDRE){ 
					// Here we'll start the transmission that'll take 8 cycles. But for now it happens instantly.
					// Accelerometer
					// TODO: check all the RDRF | TDRE stuff once we begin sampling the accel, it's proablby wrong the way its coded now
					if(~(getMemory8(PORT9)) & ACCEL_PIN){ // TODO: find more readable way to deal with pins
						switch(accel.buffer.state){
							case ACCEL_GETTING_ADDRESS:{
								accel.buffer.address = *SSU.SSTDR & 0x7F; // Remove bit 7 (R/W flag), keep 7-bit register address
								accel.buffer.offset = 0;
								accel.buffer.state = ACCEL_GETTING_BYTES;
								*SSU.SSSR = *SSU.SSSR | RDRF; 
							}break;
							case ACCEL_GETTING_BYTES:{
								*SSU.SSRDR = accel.memory[(accel.buffer.address) + accel.buffer.offset]; 
								accel.buffer.offset += 1;
								*SSU.SSSR = *SSU.SSSR | RDRF; 
								*SSU.SSSR = *SSU.SSSR | TDRE; 
								*SSU.SSSR = *SSU.SSSR | TEND; 
							}break;
						}
					}
					// EEPROM
					else if(~(getMemory8(PORT1)) & EEPROM_PIN){ 
						bool ssuOpFinished = false;
						SSU.progress += 1;
						if (SSU.progress == 7){
							SSU.progress = 0;
							ssuOpFinished = true;
						}
						if (ssuOpFinished){
							switch(eeprom.buffer.state){
								case EEPROM_EMPTY:{
									switch(*SSU.SSTDR){
										case 0x3:{ // READ - read from memory
											eeprom.buffer.state = EEPROM_GETTING_ADDRESS_HI;
											eeprom.buffer.isWrite = 0;
										} break;
										case 0x5:{ // RDSR - read status register
											eeprom.buffer.state = EEPROM_GETTING_STATUS_REGISTER;
										} break;
										case 0x6:{ // WREN - write enable. v2 drives EEP writes
										            // full-duplex (SSER=0xC0), so WREN/WRITE land
										            // on this read path; honour them here too.
											eeprom.status |= 0x2; // WEL
											*SSU.SSSR = *SSU.SSSR | TEND;
										} break;
										case 0x2:{ // WRITE - same addr state machine, then store
											eeprom.buffer.state = EEPROM_GETTING_ADDRESS_HI;
											eeprom.buffer.isWrite = 1;
										} break;
									}
								} break;
								case EEPROM_GETTING_STATUS_REGISTER:{
									*SSU.SSRDR = eeprom.status; 
									*SSU.SSSR = *SSU.SSSR | TEND;
								} break;
								case EEPROM_GETTING_ADDRESS_HI:{
									eeprom.buffer.hiAddress = *SSU.SSTDR;
									eeprom.buffer.state = EEPROM_GETTING_ADDRESS_LO;
								} break;

								case EEPROM_GETTING_ADDRESS_LO:{
									eeprom.buffer.loAddress = *SSU.SSTDR;
									eeprom.buffer.state = EEPROM_GETTING_BYTES;
								} break;

								case EEPROM_GETTING_BYTES:{
									if (eeprom.buffer.isWrite){
										// WRITE clocked on the full-duplex path: store the
										// byte (page-wrapped offset, identical to the
										// TE-only write path used by the original ROM).
										eeprom.memory[((eeprom.buffer.hiAddress << 8) | eeprom.buffer.loAddress) + eeprom.buffer.offset] = *SSU.SSTDR;
										eeprom.buffer.offset = (eeprom.buffer.offset + 1) % EEPROM_PAGE_SIZE;
									} else {
										*SSU.SSRDR = eeprom.memory[(((eeprom.buffer.hiAddress << 8) | eeprom.buffer.loAddress) + eeprom.buffer.offset) & 0xFFFF];
										eeprom.buffer.offset  = (eeprom.buffer.offset + 1);
									}
									*SSU.SSSR = *SSU.SSSR | TEND;

								} break;
							}
							*SSU.SSSR = *SSU.SSSR | RDRF; // Wait for rx
							*SSU.SSSR = *SSU.SSSR | TDRE;
					}
				}
			}
			}
			else if (*SSU.SSER & TE){ 
				if(~*SSU.SSSR & TDRE){ 
					// Accelerometer
					if(~(getMemory8(PORT9)) & ACCEL_PIN){ 
						switch(accel.buffer.state){
							case ACCEL_GETTING_ADDRESS:{
								accel.buffer.address = *SSU.SSTDR;
								accel.buffer.state = ACCEL_GETTING_BYTES;
								*SSU.SSSR = *SSU.SSSR | RDRF; 
							}break;
							case ACCEL_GETTING_BYTES:{
								accel.memory[accel.buffer.address] = *SSU.SSTDR;
								*SSU.SSSR = *SSU.SSSR | RDRF; 
								*SSU.SSSR = *SSU.SSSR | TDRE; 
								*SSU.SSSR = *SSU.SSSR | TEND; 
							}break;
						}
					}
					// EEPROM
					if(~(getMemory8(PORT1)) & EEPROM_PIN){ 
						bool ssuOpFinished = false;
						SSU.progress += 1;
						if (SSU.progress == 7){
							SSU.progress = 0;
							ssuOpFinished = true;
						}
						if (ssuOpFinished){
							switch (eeprom.buffer.state) {
								case EEPROM_EMPTY:{
									switch(*SSU.SSTDR){
										case 0x6:{ // WREN - write enable
											eeprom.status |= 0x2; // WEL - write enable latch. No WRDI/WRSR handling, so the latch is never cleared.
											*SSU.SSSR = *SSU.SSSR | TEND;
										}break;
										case 0x2: { // WRITE
											eeprom.buffer.state = EEPROM_GETTING_ADDRESS_HI;
										}break;
									}

								} break;
								case EEPROM_GETTING_ADDRESS_HI:{
									eeprom.buffer.hiAddress = *SSU.SSTDR;
									eeprom.buffer.state = EEPROM_GETTING_ADDRESS_LO;
								} break;

								case EEPROM_GETTING_ADDRESS_LO:{
									eeprom.buffer.loAddress = *SSU.SSTDR;
									eeprom.buffer.state = EEPROM_GETTING_BYTES;
								} break;

								case EEPROM_GETTING_BYTES:{
									eeprom.memory[((eeprom.buffer.hiAddress << 8) | eeprom.buffer.loAddress) + eeprom.buffer.offset] = *SSU.SSTDR;
									eeprom.buffer.offset = (eeprom.buffer.offset + 1) % EEPROM_PAGE_SIZE;
									*SSU.SSSR = *SSU.SSSR | TEND;
								} break;

								default:{
									return 1; // Invalid state
								}
							}
							*SSU.SSSR = *SSU.SSSR | TDRE; 
						}
					}

					// LCD
					if((getMemory8(PORT1)) & LCD_DATA_PIN){ 
						bool ssuOpFinished = false;
						SSU.progress += 1;
						if (SSU.progress == 7){
							SSU.progress = 0;
							ssuOpFinished = true;
						}
						if(ssuOpFinished){
							size_t lcdMemIndex = (lcd.currentPage * LCD_WIDTH * LCD_BYTES_PER_STRIPE) + lcd.currentColumn*LCD_BYTES_PER_STRIPE + lcd.currentByte;
							assert(lcdMemIndex < LCD_MEM_SIZE);
							/* Draw trace: report raw pixel writes that land in the
							 * bottom band of either GDDRAM half (pages 6-7 = rows
							 * 48-63, pages 14-15 = the other half) at the right
							 * edge x>=80 — i.e. the footer-right fragment that no
							 * lcd_draw_image covers. Names the writer instruction. */
							if (drawTraceEnabled && *SSU.SSTDR != 0 &&
							    (lcd.currentPage==6||lcd.currentPage==7||lcd.currentPage==14||lcd.currentPage==15) &&
							    lcd.currentColumn>=80) {
								fprintf(stderr,
									"[SSU] page=%u col=%u byte=%u val=%02X  writer=0x%04X\n",
									lcd.currentPage, lcd.currentColumn, lcd.currentByte,
									*SSU.SSTDR, lcdSstdrWriterPC);
							}
							lcd.memory[lcdMemIndex] = *SSU.SSTDR;
							if (lcd.currentByte == 1){
							lcd.currentColumn = (lcd.currentColumn + 1);
							}
							lcd.currentByte = (lcd.currentByte + 1) % 2;
							*SSU.SSSR = *SSU.SSSR | TDRE; 
							*SSU.SSSR = *SSU.SSSR | TEND;
						}
					}
					else if(~(getMemory8(PORT1)) & LCD_PIN){
						switch(lcd.state){
							case LCD_EMPTY:{
								switch(*SSU.SSTDR){
									case 0x00:
									case 0x01:
									case 0x02:
									case 0x03:
									case 0x04:
									case 0x05:
									case 0x06:
									case 0x07:
									case 0x08:
									case 0x09:
									case 0x0A:
									case 0x0B:
									case 0x0C:
									case 0x0D:
									case 0x0E:
									case 0x0F:{
										lcd.currentColumn = (*SSU.SSTDR & 0xF) | (lcd.currentColumn & 0xF0); // Set lower column address
										lcd.currentByte = 0;
									}break;
									case 0x10:
									case 0x11:
									case 0x12:
									case 0x13:
									case 0x14:
									case 0x15:
									case 0x16:
									case 0x17:{
										lcd.currentColumn = ((*SSU.SSTDR & 0b111) << 4) | (lcd.currentColumn & 0xF); // Set upper column address
										lcd.currentByte = 0;
									} break;
									case 0xB0:
									case 0xB1:
									case 0xB2:
									case 0xB3:
									case 0xB4:
									case 0xB5:
									case 0xB6:
									case 0xB7:
									case 0xB8:
									case 0xB9:
									case 0xBA:
									case 0xBB:
									case 0xBC:
									case 0xBD:
									case 0xBE:
									case 0xBF:{
										lcd.currentPage = *SSU.SSTDR & 0xF;
										#ifdef STARTLINE_TRACE
										{ static int n=0; if(n++<40) fprintf(stderr,"[PG] page=%u (cmd=0x%02X) pc=0x%04X\n", lcd.currentPage, *SSU.SSTDR, pc); }
										#endif
									}break;
									case 0x40:{
										/* SSD1854 set-display-start-line: the
										 * next command byte (0x00 or 0x40) is
										 * the scan-out origin row. ROM 0x7cac:
										 * send 0x40, then F7E4*0x40. */
										lcd.state = LCD_READING_STARTLINE;
										#ifdef STARTLINE_TRACE
										{ static int n=0; if(n++<40) fprintf(stderr,"[40] set-start-line cmd pc=0x%04X\n", pc); }
										#endif
									} break;
									case 0x81:{
										lcd.state = LCD_READING_CONTRAST;
									} break;
									default:{
										// We'll ignore most commands
									} break;
								}
							} break;
							case LCD_READING_CONTRAST:{
								lcd.contrast = *SSU.SSTDR;
								lcd.state = LCD_EMPTY;
							}break;
							case LCD_READING_STARTLINE:{
								lcd.displayStartLine = *SSU.SSTDR & 0x7F; /* 0..127 */
								lcd.startLineSet = true;
								/* "Active" = the ROM has selected the UPPER half
								 * (start-line >= panel height). Only firmware that
								 * page-flips for double-buffering ever does this.
								 * Firmware that only sets line 0 (or an init value
								 * <64) while drawing into the upper half keeps the
								 * free-running fallback instead. */
								if (lcd.displayStartLine >= LCD_HEIGHT)
									lcd.startLineActive = true;
								lcd.state = LCD_EMPTY;
								#ifdef STARTLINE_TRACE
								{ static int n=0; if(n++<40) fprintf(stderr,"[SL] line=%u pc=0x%04X page=%u active=%d\n", lcd.displayStartLine, pc, lcd.currentPage, lcd.startLineActive); }
								#endif
							}break;

						}
					*SSU.SSSR = *SSU.SSSR | TDRE; 
					*SSU.SSSR = *SSU.SSSR | TEND;
					}
				}
			}
			else if (*SSU.SSER & RE){ 
				return 1; // TODO: Check if this mode is used in the ROM
			}
		}

		// SCI3 TX timing — count down to TDRE after TDR3 write.
		if (SCI3.txCountdown > 0) {
			if (--SCI3.txCountdown == 0) {
				*SCI3.SSR3 |= SCI3_TDRE | SCI3_TEND;
#ifdef __3DS__
				if (SCI3.txHasPending) {
					ir_tx_byte(SCI3.txPending);
					SCI3.txHasPending = false;
					SCI3.txIdleCountdown = 320; /* detect end of packet burst */
				} else {
					SCI3.txIdleCountdown = 320;
				}
#endif
			}
		}
#ifdef __3DS__
		// Detect end of TX burst: if 320 cycles pass after TDRE
		// without a new TDR3 write, the ROM's packet is done.
		// Switch SC16IS750 from TX-only to RX-only.
		if (SCI3.txIdleCountdown > 0) {
			if (--SCI3.txIdleCountdown == 0) {
				ir_tx_flush_to_rx();
			}
		}
#endif

		// SCI3 RX timing — deliver next byte from rxBuf at baud rate.
		// Delivery is read-paced: the next byte lands after the firmware
		// reads RDR3 (a perfectly-buffered model — more forgiving than real
		// overrun semantics, but it works for firmware that sleeps or paints
		// mid-listen).
		if (SCI3.rxCountdown > 0) {
			if (--SCI3.rxCountdown == 0) {
				if (SCI3.rxPos < SCI3.rxLen) {
					uint8_t rxByte = SCI3.rxBuf[SCI3.rxPos++];
					*SCI3.RDR3 = rxByte;
					*SCI3.SSR3 |= SCI3_RDRF;
					{
						static int sciTrace2 = -1;
						if (sciTrace2 < 0) { const char *e = getenv("POKESTRIDE_SCI_TRACE"); if (!e) e = getenv("PWDBG_SCI_TRACE"); sciTrace2 = (e && *e && *e != '0') ? 1 : 0; }
						if (sciTrace2) fprintf(stderr, "[DL] %u/%u %02X\n", SCI3.rxPos, SCI3.rxLen, rxByte);
					}
#ifdef __3DS__
					irTracePush(IR_TRACE_RX_BYTE, rxByte, pc, 0);
#endif
				}
			}
		}

		// SCI3 IR polling — periodically check for incoming RX data.
		// Poll interval must be long enough that the I2C overhead of reading
		// RXLVL (~100-160 µs per transaction) doesn't dominate the frame.
		// At 115200 baud ~11.5 bytes/ms arrive; the SC16IS750 FIFO holds 64
		// bytes (~5.5 ms).  4096 H8 cycles ≈ 1.1 ms → ~13 bytes between
		// polls, well within the FIFO.  (512 was too frequent — 1800 I2C
		// reads per frame consumed ~270 ms, exceeding the 250 ms budget.)
#ifdef __3DS__
		{
			static uint32_t irPollCountdown = 4096;
			if (--irPollCountdown == 0) {
				/* Fixed 4096-cycle poll (~1.1ms). An adaptive tighter poll
				 * during RE was tried and regressed the handshake (more I2C
				 * per frame added jitter exactly when timing matters); the
				 * cleanest handshake runs used this fixed rate, so keep it. */
				irPollCountdown = 4096;
				// Drain the SC16IS750 RX FIFO as soon as the previous burst has
				// been fully delivered to the ROM (rxPos >= rxLen), NOT gated on
				// RDRF. The drain must be eager: while RDRF stays set (ROM
				// mid-read) the peer keeps clocking bytes into the 64-byte
				// SC16IS750 FIFO, which would OVERFLOW and silently drop packet
				// bytes. The read-paced delivery + SSR3 read-1-then-write-0 below
				// still pace bytes to the ROM one at a time; only the hardware-FIFO
				// drain is eager.
				if (*SCI3.SCR3 & SCI3_RE) {
					if (SCI3.rxPos >= SCI3.rxLen) {
						size_t n = ir_recv_poll(SCI3.rxBuf, sizeof(SCI3.rxBuf));
						if (n > 0) {
							SCI3.rxLen = n;
							SCI3.rxPos = 0;
							irTracePush(IR_TRACE_RX_POLL, (uint8_t)n, 0, 0);
							// Start countdown for first byte delivery (only if
							// RDR3 is free and no delivery already pending).
							if (!(*SCI3.SSR3 & SCI3_RDRF) && SCI3.rxCountdown == 0) {
								SCI3.rxCountdown = 320;
							}
						}
					}
				}
			}
		}
#endif

		if (--subClockCountdown == 0) {
			subClockCountdown = SYSTEM_CLOCK_CYCLES_PER_SECOND / SUB_CLOCK_CYCLES_PER_SECOND;
			subClockCyclesEllapsed += 1;
			runSubClock();
		}
	}

	return 0;
}

void initWalker(){
	memset(&inputQueue, 0 , sizeof(inputQueue));
	/* read from reset vector after ROM loads */

	sleep = false;
	uint64_t subClockCyclesEllapsed = 0;
	
	memory = malloc(MEM_SIZE);
	memset(memory, 0, MEM_SIZE);
	
	memset(&eeprom, 0, sizeof(eeprom));
	eeprom.memory = malloc(EEPROM_SIZE);
	memset(eeprom.memory, 0xFF, EEPROM_SIZE);

#ifndef INIT_EEPROM
	FILE *eepromFile = fopen("pweep.rom", "rb");
	fread(eeprom.memory, 1, EEPROM_SIZE, eepromFile);
	fclose(eepromFile);
#endif

	memset(&accel, 0, sizeof(accel));
	accel.memory = malloc(64);  /* BMA150 has registers up to 0x3F */
	memset(accel.memory, 0, 64);
	accel.memory[0] = 0x2; // Chip id

	memset(&lcd, 0, sizeof(lcd));
	lcd.contrast = 20;
	lcd.state = LCD_EMPTY;
	lcd.memory = malloc(LCD_MEM_SIZE);

	/* Opt-in draw trace (debug only). POKESTRIDE_DRAW_TRACE=1 (or PWDBG_DRAW_TRACE) → trace lcd_draw_image
	 * at the default PC (0x80ac); =0xNNNN → trace that PC instead. */
	{
		const char *dt = getenv("POKESTRIDE_DRAW_TRACE"); if (!dt) dt = getenv("PWDBG_DRAW_TRACE");
		if (dt && *dt) {
			drawTraceEnabled = true;
			unsigned long v = strtoul(dt, NULL, 0);
			if (v > 1) drawTracePC = (uint16_t)v;
		}
	}

	FILE* romFile = fopen("pwflash.rom","rb");
	if(!romFile){
		printf("Can't find rom");
	}

	fseek (romFile , 0 , SEEK_END);
	int romSize = ftell (romFile);
	rewind (romFile);

	fread(memory,1,romSize ,romFile);
	fclose(romFile);

	// Read entry point from reset vector (first 2 bytes of ROM, big-endian)
	entry = (memory[0] << 8) | memory[1];

	// Init SSU registers
	SSU.SSCRH = &memory[0xF0E0]; 
	SSU.SSCRL = &memory[0xF0E1]; 
	SSU.SSMR = &memory[0xF0E2]; 
	SSU.SSER = &memory[0xF0E3]; 
	SSU.SSSR = &memory[0xF0E4]; 
	SSU.SSRDR = &memory[0xF0E9]; 
	SSU.SSTDR = &memory[0xF0EB]; 
	SSU.SSTRSR = 0x0; 

	*SSU.SSRDR = 0x0; 
	*SSU.SSTDR = 0x0;
	*SSU.SSER = 0x0; 
	*SSU.SSSR = 0x4; // TDRE = 1 (Transmit data empty)

	// Init SCI3 registers (IrDA serial)
	SCI3.SMR3 = &memory[0xFF98];
	SCI3.BRR3 = &memory[0xFF99];
	SCI3.SCR3 = &memory[0xFF9A];
	SCI3.TDR3 = &memory[0xFF9B];
	SCI3.SSR3 = &memory[0xFF9C];
	SCI3.RDR3 = &memory[0xFF9D];
	SCI3.IrCR = &memory[0xFFA7];
	*SCI3.SSR3 = SCI3_TDRE | SCI3_TEND;
	*SCI3.SCR3 = 0;
	SCI3.txCountdown = 0;
	SCI3.txPending = 0;
	SCI3.txHasPending = false;
	SCI3.rxLen = 0;
	SCI3.rxPos = 0;
	SCI3.rxCountdown = 0;

	// Init general purpose registers
	for(int i=0; i < 8;i++){
		ER[i] = malloc(4);
		*ER[i] = 0;
		R[i] = (uint16_t*) ER[i];
		E[i] = (uint16_t*) ER[i] + 1;
		RL[i] = (uint8_t*) R[i];
		RH[i] = (uint8_t*) R[i] + 1;
	}
	SP = ER[7];
	flags = (struct Flags_t){0};

	// Populate debug anchor for external inspection
	debugAnchor.magic1 = 0x504B5354; // "PKST"
	debugAnchor.magic2 = 0x524C4421; // "RLD!"
	debugAnchor.memory = memory;
	debugAnchor.pc = &pc;
	debugAnchor.sci3 = (struct SCI3_t*)&SCI3;
	debugAnchor.flags = &flags;
	debugAnchor.ER = ER;

	printRegistersState();

	// Init Timers
	TimerB.on = false;
	TimerB.TMB1 = &memory[0xF0D0];
	setMemory8(0xf0d0, 0b00111000); 
	TimerB.TCB1 = &memory[0xF0D1];
	setMemory8(0xf0d1, 0); 
	TimerB.TLBvalue = 0;
	memset(&TimerW, 0, sizeof(TimerW));
	TimerW.TMRW = &memory[0xf0f0];
	setMemory8(0xf0f0, 0b01001000); 
	TimerW.TCRW = &memory[0xf0f1];
	setMemory8(0xf0f1, 0);
	TimerW.TIERW = &memory[0xf0f2];
	setMemory8(0xf0f2, 0b01110000);
	TimerW.TSRW = &memory[0xf0f3];
	setMemory8(0xf0f3, 0b01110000);
	TimerW.TIOR0 = &memory[0xf0f4];
	setMemory8(0xf0f4, 0b10001000);
	TimerW.TIOR1 = &memory[0xf0f5];
	setMemory8(0xf0f5, 0b10001000);
	TimerW.TCNT = (uint16_t*)&memory[TCNT_ADDRESS];
	setMemory16(TCNT_ADDRESS, 0);
	TimerW.GRA = (uint16_t*)&memory[0xf0f8];
	setMemory16(0xf0f8, 0xffff);
	TimerW.GRB = (uint16_t*)&memory[0xf0fa];
	setMemory16(0xf0fa, 0xffff);
	TimerW.GRC = (uint16_t*)&memory[0xf0fc];
	setMemory16(0xf0fc, 0xffff);
	/*
	TimerW.GRD = (uint16_t*)&memory[0xf0fe];
	setMemory8(0xf0fe, 0);
	*/ // Unused in the ROM

	// Init Clock halt registers
	CKSTPR1 = &memory[0xfffa];	
	setMemory8(0xFFFA, 0b00000011); 
	CKSTPR2 = &memory[0xfffb];	
	setMemory8(0xFFFB, 0b00000100);

	// Init Interrupt stuff
	IRQ_IENR1 = &memory[0xfff3];
	*IRQ_IENR1 = 0;
	IRQ_IENR2 = &memory[0xfff4];
	*IRQ_IENR2 = 0;
	IRQ_IRR1 = &memory[0xfff6];
	*IRQ_IRR1 = 0;
	IRQ_IRR2 = &memory[0xfff7];
	*IRQ_IRR2 = 0;
	RTCFLG = &memory[0xf067];
	*RTCFLG = 0;
	interruptSavedAddress = 0;
	interruptSaveDepth = 0;

	quartersEllapsed = 0;
	pc = entry;

}

void halfRTCInterrupt(){
	*RTCFLG |= _05SEIFG ;
}

void secondRTCInterrupt(){
	*RTCFLG |= _1SEIFG ;
}

void quarterRTCInterrupt(){
	*RTCFLG |= _025SEIFG ;
	quartersEllapsed += 1;
	if((quartersEllapsed % 2) == 0){
		halfRTCInterrupt();
	}
	if((quartersEllapsed % 4) == 0){
		secondRTCInterrupt();
	}
}

void injectSteps(uint32_t steps){
	if (steps == 0) return;

	/* RAM addresses:
	 *   0xF780: RamCache_totalSteps       (u32 BE, max 9999999)
	 *   0xF784: RamCache_STEP_COUNT       (u32 BE, identity step count)
	 *   0xF78E: RamCache_curWatts         (u16 BE, max 9999)
	 *   0xF792: stepToWattDividerState    (u8, counts 0-19, resets at 20 → +1 watt)
	 *   0xF79C: stepCountTodaySoFar       (u32 BE, max 99999)
	 *
	 * Watts conversion: every 20 steps = 1 watt (ROM function at 0x1F3E).
	 */

	/* Read current values */
	uint32_t lifetime  = (uint32_t)getMemory16(0xF780) << 16 | getMemory16(0xF782);
	uint32_t stepCount = (uint32_t)getMemory16(0xF784) << 16 | getMemory16(0xF786);
	uint16_t watts     = getMemory16(0xF78E);
	uint8_t  divider   = (uint8_t)getMemory8(0xF792);
	uint32_t today     = (uint32_t)getMemory16(0xF79C) << 16 | getMemory16(0xF79E);

	/* Add steps */
	lifetime  += steps;
	stepCount += steps;
	today     += steps;

	/* Cap at ROM maximums */
	if (lifetime > 9999999) lifetime = 9999999;
	if (today > 99999) today = 99999;

	/* Calculate watts: advance the divider for each step */
	uint32_t newWatts = (divider + steps) / 20;
	divider = (divider + steps) % 20;
	watts += newWatts;
	if (watts > 9999) watts = 9999;

	/* Write back to RAM */
	setMemory32(0xF780, lifetime);
	setMemory32(0xF784, stepCount);
	setMemory16(0xF78E, watts);
	setMemory8(0xF792, divider);
	setMemory32(0xF79C, today);
}

uint32_t getWalkerSteps(void){
	return (uint32_t)getMemory16(0xF79C) << 16 | getMemory16(0xF79E);
}

uint32_t getWalkerLifetimeSteps(void){
	/* RamCache_totalSteps @ 0xF780, u32 BE (max 9999999). Mirrors the
	 * "lifetime" counter the firmware also persists to EEPROM HealthData. */
	return (uint32_t)getMemory16(0xF780) << 16 | getMemory16(0xF782);
}

uint16_t getWalkerWatts(void){
	return getMemory16(0xF78E);
}

/* ---- Currently walked Pokemon (read from EEPROM PHCCourseData) ---------- *
 * PHCCourseData lives at EEPROM 0x8F00. Within it the walked Pokemon's
 * PHCPokemonBase fields are laid out as:
 *   +0x00  u16 LE  species ID
 *   +0x0C  u8      level
 *   +0x0D  u8      bits 0-4 form, 5-6 sex, 7 isEgg
 *   +0x0E  u8      bit 0 shiny
 *   +0x10  22 B    nickname (gen4 wide chars terminated by 0xFFFF)
 */
#define WALKED_BASE          0x8F00
#define WALKED_OFF_SPECIES   0x00
#define WALKED_OFF_LEVEL     0x0C
#define WALKED_OFF_FOLM_SEX  0x0D
#define WALKED_OFF_FLAGS     0x0E
#define WALKED_OFF_NICKNAME  0x10
#define WALKED_NICKNAME_SIZE 22

uint16_t getWalkedSpecies(void){
	const uint8_t *p = &eeprom.memory[WALKED_BASE + WALKED_OFF_SPECIES];
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint8_t getWalkedLevel(void){
	return eeprom.memory[WALKED_BASE + WALKED_OFF_LEVEL];
}

uint8_t getWalkedForm(void){
	return eeprom.memory[WALKED_BASE + WALKED_OFF_FOLM_SEX] & 0x1F;
}

uint8_t getWalkedSex(void){
	return (eeprom.memory[WALKED_BASE + WALKED_OFF_FOLM_SEX] >> 5) & 0x03;
}

bool getWalkedShiny(void){
	return (eeprom.memory[WALKED_BASE + WALKED_OFF_FLAGS] & 0x01) != 0;
}

void getWalkedNickname(uint8_t out[WALKED_NICKNAME_SIZE]){
	memcpy(out, &eeprom.memory[WALKED_BASE + WALKED_OFF_NICKNAME],
	       WALKED_NICKNAME_SIZE);
}

uint16_t getTimerWGRA(void){
	return getMemory16(0xF0F8);
}

uint8_t getWalkerVolume(void){
	return (uint8_t)getMemory8(0xF7C6);
}

bool isTimerWActive(void){
	return TimerW.on;
}

uint16_t getPC(void){
	return lastExecPC;
}

uint16_t getEntry(void){
	return (uint16_t)entry;
}

bool isSleeping(void){
	return sleep;
}

bool lcdStartLineSet(void){
	return lcd.startLineSet;
}

bool lcdStartLineActive(void){
	return lcd.startLineActive;
}

uint8_t lcdDisplayStartLine(void){
	return lcd.displayStartLine;
}

uint8_t readMem(uint16_t addr){
	return memory[addr];
}

uint16_t getTimerWReg(uint16_t addr){
	return getMemory16(addr);
}

uint8_t getAudioReg(int idx){
	static const uint16_t addrs[] = { 0xFF8C, 0xFF8E, 0xFF91 };
	if (idx < 0 || idx > 2) return 0;
	return (uint8_t)getMemory8(addrs[idx]);
}

bool consumeAudioEvent(uint16_t *graOut){
	if (!audioEventPending) return false;
	audioEventPending = false;
	if (graOut) *graOut = audioEventGRA;
	return true;
}

int saveEeprom(void){
	/* Sync RAM step/watts counters to EEPROM before saving.
	 * HealthData: 0x18 bytes at EEPROM 0x0156 (A) / 0x0256 (B).
	 * Checksum at offset +0x18 = 1 + sum(data[0..0x17]).       */
	static const uint16_t healthBase[2] = { 0x0156, 0x0256 };
	for (int copy = 0; copy < 2; copy++) {
		uint16_t b = healthBase[copy];
		/* lifetimeTotalSteps: +0x00 (4B) from RAM 0xF780 */
		for (int i = 0; i < 4; i++) eeprom.memory[b + i]      = memory[0xF780 + i];
		/* stepCount:          +0x04 (4B) from RAM 0xF784 */
		for (int i = 0; i < 4; i++) eeprom.memory[b + 4 + i]  = memory[0xF784 + i];
		/* curWatts:            +0x0E (2B) from RAM 0xF78E */
		for (int i = 0; i < 2; i++) eeprom.memory[b + 0xE + i] = memory[0xF78E + i];
		/* Checksum = 1 + sum of 0x18 data bytes */
		uint8_t cksum = 1;
		for (int i = 0; i < 0x18; i++) cksum += eeprom.memory[b + i];
		eeprom.memory[b + 0x18] = cksum;
	}

	FILE *f = fopen("pweep.rom", "wb");
	if (!f) return -1;
	size_t written = fwrite(eeprom.memory, 1, EEPROM_SIZE, f);
	fflush(f);
	fclose(f);

	return (written == EEPROM_SIZE) ? 0 : -2;
}

