#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SYSTEM_CLOCK_CYCLES_PER_SECOND 3686400 /* 3.6864 MHz */
#define SUB_CLOCK_CYCLES_PER_SECOND 32768 /* 32.768 KHz */

#define ENTER (1<<0)
#define LEFT (1<<2)
#define RIGHT (1<<4)

#define LCD_WIDTH 96
#define LCD_HEIGHT 64

/* PC of ui_keypoll_main for the ROM with entry==0x0080. Set from the ELF symbol
 * table so the button-inject hook survives firmware rebuilds. 0 = disabled. */
extern uint32_t walkerV2KeypollPC;

void initWalker(); // Must be called once before the main loop
int runNextInstruction(uint64_t* cycleCount); // Must be called once every main loop iteration and given a cycleCount variable defined globally
void fillVideoBuffer(uint32_t* videoBuffer);
bool lcdStartLineSet(void);       // true once the ROM issued a 0x40 set-display-start-line
bool lcdStartLineActive(void);    // true once start-line set non-zero (ROM actually page-flips)
uint8_t lcdDisplayStartLine(void);// current scan-out origin row (0..127)
void walkerSetDrawTrace(int on, unsigned pcOpt); // debug: log lcd_draw_image calls (pcOpt=0 keeps current PC)
void setKeys(uint8_t input); // Must be called every time a key is pressed down. 'input' should be one of ENTER, LEFT or RIGHT
void quarterRTCInterrupt();// Must be called once every quarter second
void injectSteps(uint32_t steps); // Add steps to the PokéWalker's RAM counters (todaySteps + lifetimeSteps + watts)
uint32_t getWalkerSteps(void);    // Read todaySteps from PW RAM (0xF79C)
uint32_t getWalkerLifetimeSteps(void); // Read lifetime totalSteps from PW RAM (0xF780)
uint16_t getWalkerWatts(void);    // Read curWatts from PW RAM (0xF78E)

/* ---- Currently walked Pokemon (read from EEPROM PHCCourseData @ 0x8F00) -*/
#define WALKER_NICKNAME_SIZE 22
uint16_t getWalkedSpecies(void);
uint8_t  getWalkedLevel(void);
uint8_t  getWalkedForm(void);  /* 0..31 - alt form index */
uint8_t  getWalkedSex(void);   /* 0=male 1=female 2=genderless (best-effort) */
bool     getWalkedShiny(void);
void     getWalkedNickname(uint8_t out[WALKER_NICKNAME_SIZE]); /* gen4 wide */
uint16_t getTimerWGRA(void);      // Read Timer W GRA register (audio frequency)
uint8_t  getWalkerVolume(void);   // Read volume setting from PW RAM (0xF7C6)
bool     isTimerWActive(void);    // Check if Timer W is running
uint16_t getPC(void);             // Read H8 PC at last instruction (for error logging)
uint16_t getEntry(void);          // ROM entry point (reset vector)
bool isSleeping(void);            // true while the CPU is halted in SLEEP (waiting for an IRQ)
uint8_t  readMem(uint16_t addr);  // Read a byte from H8 memory
uint8_t  getAudioReg(int idx);   // Read AEC registers: 0=ECPWCR(0xFF8C), 1=ECPWDR(0xFF8E), 2=SPCR(0xFF91)
uint16_t getTimerWReg(uint16_t addr); // Read any Timer W 16-bit register
bool     consumeAudioEvent(uint16_t *graOut); // Returns true if Timer W was active since last call, with last GRA value
int saveEeprom(void); // Write eeprom.memory back to pweep.rom. Returns 0 on success.
void savePedometerState(uint32_t totalInjected); // Save injected step count to file
uint32_t loadPedometerState(void); // Load previously saved injected step count
