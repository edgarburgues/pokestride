#pragma once
#include <stdint.h>

#define STARTING_WATTS 50

// Memory
	// 0x0000 - 0xBFFF - ROM 
	// 0xF020 - 0xF0FF - MMIO
	// 0xF780 - 0xFF7F - RAM 
	// 0xFF80 - 0xFFFF - MMIO
#define MEM_SIZE (64 * 1024)

// Flags / CCR Condition Code Register
struct Flags_t{
	bool I; // Interrupt mask bit
	bool UI; // User bit
	bool H; // Half carry flag
	bool U; // User bit
	bool N; // Negative flag
	bool Z; // Zero flag
	bool V; // Overflow flag
	bool C; // Carry flag
};	

// Vector Table — read from ROM at runtime (vector# × 2 bytes, big-endian)
#define VECTOR_AT(n) ((memory[(n)*2] << 8) | memory[(n)*2 + 1])
#define VECTOR_TIMER_B1        VECTOR_AT(33)
#define VECTOR_TIMER_W         VECTOR_AT(35)
#define VECTOR_IRQ0            VECTOR_AT(16)
#define VECTOR_RTC_QUARTER_SEC VECTOR_AT(23)
#define VECTOR_RTC_HALF_SEC    VECTOR_AT(24)
#define VECTOR_RTC_EVERY_SEC   VECTOR_AT(25)

// Port Addresses
#define PORT1 0xFFD4
#define PORT9 0xFFDC

// Colors
#define GRAY_0 0x00333333
#define GRAY_1 0x00666666
#define GRAY_2 0x00999999
#define GRAY_3 0x00CCCCCC
const static uint32_t palette[4] = {GRAY_3, GRAY_2, GRAY_1, GRAY_0};

// SSU
struct SSU_t{
	uint8_t* SSCRH; // Control register H
	uint8_t* SSCRL; // Control register L
	uint8_t* SSMR; // Mode register
	uint8_t* SSER; // Enable register
	uint8_t* SSSR; // Status register
	uint8_t* SSRDR; // Recieve data register
	uint8_t* SSTDR; // Transmit data register.
	uint8_t SSTRSR; // Shift register.
	uint8_t progress; // goes from 0 to 7
};
#define RDRF (1 << 1) /* Recieve Data Register Full */
#define TDRE (1 << 2) /* Transmit Data Register Empty */
#define TEND (1 << 3) /* Transmit End */
#define TE 0x80 /* Transmission Enabled */
#define RE 0x40 /* Reception Enabled */

// SCI3 (IrDA serial)
struct SCI3_t {
	uint8_t *SMR3;   // 0xFF98 - Serial Mode Register
	uint8_t *BRR3;   // 0xFF99 - Bit Rate Register
	uint8_t *SCR3;   // 0xFF9A - Serial Control Register
	uint8_t *TDR3;   // 0xFF9B - Transmit Data Register
	uint8_t *SSR3;   // 0xFF9C - Serial Status Register
	uint8_t *RDR3;   // 0xFF9D - Receive Data Register
	uint8_t *IrCR;   // 0xFFA7 - IrDA Control Register
	// TX timing: TDRE delay countdown (cycles until TDRE goes high after TDR write)
	// At 115200 baud with 3.6864 MHz clock: 1 byte = 10 bits / 115200 ≈ 86.8 µs ≈ 320 cycles
	uint32_t txCountdown;
	uint8_t  txPending;    // byte waiting in TDR to be "shifted out"
	bool     txHasPending; // true if txPending holds a byte
	// RX buffer (filled from real IR; served to ROM byte-by-byte)
	uint8_t rxBuf[256];
	uint16_t rxLen;
	uint16_t rxPos;
	// RX timing: RDRF delay countdown (cycles until next byte is available)
	// Matches real SCI3 baud rate so the ROM's gap detection works correctly.
	uint32_t rxCountdown;
	// TX idle detector: counts down after TDRE fires. If it reaches 0
	// without a new TDR3 write, the ROM's TX burst is done → switch to RX.
	uint32_t txIdleCountdown;
};
#define SCI3_TE   (1 << 5)  /* Transmit Enable */
#define SCI3_RE   (1 << 4)  /* Receive Enable */
#define SCI3_RIE  (1 << 6)  /* Receive Interrupt Enable (SCR3 bit 6) */
#define SCI3_TDRE (1 << 7)  /* Transmit Data Register Empty */
#define SCI3_RDRF (1 << 6)  /* Receive Data Register Full */
#define SCI3_TEND (1 << 2)  /* Transmit End */

// EEPROM
static const size_t EEPROM_SIZE = 64 * 1024;
const static int EEPROM_PAGE_SIZE = 128;
#define EEPROM_PIN 0x4
enum EEPROM_STATES{
	EEPROM_EMPTY,
	EEPROM_GETTING_STATUS_REGISTER,
	EEPROM_GETTING_ADDRESS_HI,
	EEPROM_GETTING_ADDRESS_LO,
	EEPROM_GETTING_BYTES
};
struct Eeprom_t{
	uint8_t* memory;
	uint8_t status;
	struct buffer_t{
		uint8_t hiAddress;
		uint8_t loAddress;
		enum EEPROM_STATES state;
		uint16_t offset;
		uint8_t isWrite;   /* current transaction is a WRITE (cmd 0x02), not a READ (0x03).
		                    * Some firmware drives writes with SSER=0xC0 (TE+RE), so they
		                    * arrive on the read path; the command byte, not the SSER mode,
		                    * decides read vs write. */
	} buffer;
};

// Accelerometer
#define ACCEL_PIN 0x1
enum ACCEL_STATES{
	ACCEL_GETTING_ADDRESS,
	ACCEL_GETTING_BYTES,
};
struct Accelerometer_t{
	uint8_t* memory;
	struct accelBuffer_t{
		uint8_t address;
		uint8_t offset;
		enum ACCEL_STATES state;
	} buffer;
};

// LCD
#define LCD_PIN 0x1
#define LCD_DATA_PIN 0x2
#define LCD_BUFFER_SEPARATION 16
#define LCD_BYTES_PER_STRIPE 2
#define LCD_MEM_WIDTH 128 // TODO: figure out way to traverse the memory without needing to simulate the bigger RAM size
#define LCD_MEM_HEIGHT 176
static const int LCD_MEM_SIZE = LCD_MEM_WIDTH * LCD_MEM_HEIGHT / 4; // /4 cuz in 2B we get 8px ( 4px per byte)
enum LCD_STATES{
	LCD_EMPTY,
	LCD_READING_CONTRAST,
	LCD_READING_STARTLINE,   /* got 0x40, next cmd byte is the start-line value */
};
struct Lcd_t{
	uint8_t* memory;
	uint8_t contrast;
	enum LCD_STATES state;
	uint8_t currentColumn;
	uint8_t currentPage;
	uint8_t currentByte;
	bool currentBuffer;       /* legacy free-running half toggle (fallback only) */
	uint8_t displayStartLine; /* SSD1854 scan-out origin row, 0..127 (set via 0x40) */
	bool startLineSet;        /* true once the ROM issues a 0x40 set-start-line */
	bool startLineActive;     /* true once start-line selects the UPPER half
	                           * (>= panel height), i.e. the ROM actually page-flips.
	                           * A ROM that only ever sets the lower half is NOT
	                           * "active" — we fall back to pick-inkier so its render
	                           * is unchanged. */
};

// Timers
#define TMB_AUTORELOAD (1<<7)
#define TMB_COUNTING (1<<6)
struct TimerB_t{
	bool on;
	uint8_t TLBvalue;
	uint8_t* TMB1;
	uint8_t* TCB1;
};
#define CTS (1<<7)
#define CCLR (1<<7)
struct TimerW_t{
	bool on;
	uint8_t* TMRW;
	uint8_t* TCRW;
	uint8_t* TIERW;
	uint8_t* TSRW;
	uint8_t* TIOR0;
	uint8_t* TIOR1;
	uint16_t* TCNT;
	uint16_t* GRA;
	uint16_t* GRB;
	uint16_t* GRC;
	uint16_t* GRD;
};
#define TCNT_ADDRESS 0xf0f6

// Interrupts
// IRQ_IENR1; // Interrupt enable register 1
#define IEN0 (1<<0)
#define IENRTC (1<<7)
// IRQ_IENR2; // Interrupt enable register 2
#define IENTB1 (1<<2) /* Timer B1 Interrupt Request Enable */
// IRQ_IRR1; // Interrupt flag register 1
#define IRRI0 (1<<0) /* Timer B1 Interrupt Request Flag */
// IRQ_IRR2; // Interrupt flag register 2
#define IRRTB1 (1<<2) /* Timer B1 Interrupt Request Flag */
// RTCFLG; // RTC Interrupt Flag Register
#define _025SEIFG (1<<0) /* When a 0.25-second periodic interrupt occurs */
#define _05SEIFG (1<<1) /* When a 0.5-second periodic interrupt occurs */
#define _1SEIFG (1<<2) /* When a 1-second periodic interrupt occurs */

// Clocks
// CKSTPR1; // Clock halt register 1
#define TB1CKSTP (1<<2) /* Timer B1 Module Standby */

// CKSTPR2; // Clock halt register 2
#define TWCKSTP (1<<6) /* Timer W Module Standby */

