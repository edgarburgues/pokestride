# PokéWalker ROM & RAM Reference Map

Compiled from Dmitry.gr's Palm OS app, ROM disassembly, and pokewalker-rom-dumper.
Source files: `palmos-app/DISASSEMBLY/`, `palmos-app/Palm App/Src/`, `pokewalker-rom-dumper/pw/inc/`.

## RAM Variables (0xF780–0xFF7F)

### HealthData Cache (0xF780)

Mirrors EEPROM HealthData (0x0156/0x0256). All multi-byte values are big-endian.

| Address | Size | Name | Description |
|---------|------|------|-------------|
| 0xF780 | 4 | RamCache_totalSteps | Lifetime total steps (max 9,999,999) |
| 0xF784 | 4 | RamCache_STEP_COUNT | Identity step count |
| 0xF788 | 4 | RamCache_lastSyncTime | Last sync timestamp |
| 0xF78C | 2 | RamCache_totalDays | Total days |
| 0xF78E | 2 | RamCache_curWatts | Current watts (max 9,999) |

### Step Counting & Accelerometer

| Address | Size | Name | Description |
|---------|------|------|-------------|
| 0xF792 | 1 | stepToWattDividerState | Counts 0–19; at 20 → +1 watt, resets to 0 |
| 0xF79C | 4 | stepCountTodaySoFar | Daily step count (max 99,999) |
| 0xF7A0 | 2 | someWalkingRelatedState | Walking-related (clamped to 9,999) |
| 0xF7AE | 1 | ACCEL_SAMPLE_NEXT_WRITE_IDX | Accel sample ring buffer write index |
| 0xF7B2 | 1 | step_detection_related_A | Step detection state A |
| 0xF7B3 | 1 | step_detection_related_B | Step detection state B |
| 0xF7B4 | 1 | step_detection_related_C | Step detection state C |
| 0xF826 | ? | ACCEL_SAMPLES_X | X-axis sample buffer |
| 0xF866 | ? | ACCEL_SAMPLES_Y | Y-axis sample buffer |
| 0xF8A6 | ? | ACCEL_SAMPLES_Z | Z-axis sample buffer |

### UI & System State

| Address | Size | Name | Description |
|---------|------|------|-------------|
| 0xF793 | 1 | curEventLogPointer | Event log write pointer |
| 0xF797 | 1 | RamCache_settingsByte | bit0=special route, bits1-2=volume, bits3-6=contrast |
| 0xF79A | 1 | CUR_KEYS | Current key state (0x02=enter, 0x04=left, 0x08=right) |
| 0xF7A4 | 1 | currentSecond | RTC seconds |
| 0xF7A5 | 1 | currentMinute | RTC minutes |
| 0xF7A6 | 1 | currentHour | RTC hours |
| 0xF7A9 | 1 | base_contrast_value | LCD base contrast |
| 0xF7AA | 1 | menuCurItem | Current menu item index |
| 0xF7AC | 1 | curUiFlags | UI flags |
| 0xF7AD | 1 | commsErrorMessageId | Comms error message (1=no trainer, 2/8=cannot connect, 3=cannot complete, 4=no pokemon, 5=cannot again, 6=already received, 7=could not receive) |
| 0xF7B1 | 1 | currentlyActiveView | Active view/screen ID |
| 0xF7B5 | 1 | common_bit_flags | Common flags |
| 0xF7B6 | 1 | walker_status_flags | Walker status flags |
| 0xF7BE | 2 | CUR_HEAP_PTR | Current heap pointer |
| 0xF7C0 | 4 | RandomSeed | PRNG seed |
| 0xF7C6 | 1 | volume | Current volume setting |

### UI Substates

| Address | Size | Name |
|---------|------|------|
| 0xF7CE | 1 | gCurSubstateY |
| 0xF7CF | 1 | gCurSubstateZ |
| 0xF7D0 | 2 | gCurSubstateA |
| 0xF7D2 | 2 | gCurSubstateB |
| 0xF7D4 | 1 | gCurSubstateC |
| 0xF7D5 | 1 | gCurSubstateD |
| 0xF7D6 | 2 | gCurSubstateE |
| 0xF7D8 | 2 | gCurSubstateF |

### Event Loop & RCE

| Address | Size | Name | Description |
|---------|------|------|-------------|
| 0xF7E0 | 2 | currentEventLoopFunc | Main loop dispatch pointer. **RCE target**: write a function address here to execute arbitrary code. |
| 0xF7E2 | 2 | prevEventLoopFunc | Previous event loop function |

### IR Communication

| Address | Size | Name | Description |
|---------|------|------|-------------|
| 0xF7B8 | 2 | TimeWhenLastByteRxed | Time of last RX byte |
| 0xF7BA | 2 | NumBytesRxedsoFar | RX byte counter |
| 0xF8B6 | 4 | SuggestedOurSideSessionId | Our proposed session ID |
| 0xF8BA | 4 | CurrentSessionID | Active session ID |
| 0xF8BE | 1 | ourPeerRole | Peer role (master/slave) |
| 0xF8BF | 1 | numTimesWeSentIrAdv | Beacon send counter |
| 0xF8C2 | 1 | NumPingsSent | Ping counter |
| 0xF8C3 | 1 | bool_got_at_least_one_packet | RX packet received flag |
| 0xF8C4 | 1 | REQUESTED_POKEMON_ACTION_TYPE | Requested action type |
| 0xF8C5 | 1 | PeerPlayStage | Peer play state machine stage |
| 0xF8C6 | 2 | PeerPlayEepromXferSizeLeft | EEPROM transfer remaining |
| 0xF8C8 | 2 | PeerPlayEepromXferLocalAddr | Local EEPROM address |
| 0xF8CA | 2 | PeerPlayEepromXferRemoteAddr | Remote EEPROM address |
| 0xF8CC | 1 | PeerPlayEepromXfreNumPacketsSentSoFar | Transfer packet count |
| 0xF8CD | 1 | LastRxedUnusedByte | Last received extra byte |

### TX Packet Buffer (0xF8CE)

| Address | Size | Name |
|---------|------|------|
| 0xF8CE | 1 | TX_PACKET_cmd |
| 0xF8CF | 1 | TX_PACKET_extraByte |
| 0xF8D0 | 2 | TX_PACKET_crc |
| 0xF8D2 | 4 | TX_PACKET_session |
| 0xF8D6 | 128 | TX_PACKET_payload |

### Heap & Buffers

| Address | Size | Name |
|---------|------|------|
| 0xF8F0 | ? | HEAP_START |
| 0xF956 | 128+ | DECOMPRESSION_BUFFER — also **RCE code target** |

## ROM Function Addresses

### Core System

| Address | Name | Signature / Notes |
|---------|------|-------------------|
| 0x02C4 | ENTRY | Main entry point |
| 0x245E | wdt_off | Disable watchdog |
| 0x246C | wdt_on | Enable watchdog |
| 0x259E | wdt_pet | Pet watchdog |
| 0x2488 | static_malloc | `void* r0 static_malloc(r0 size)` |
| 0x247E | setHeapPointer | Uses space at 0xF8F0 |
| 0x25C8 | setRandomSeed | `void setRandomSeed(er0 seed)` |
| 0x25D0 | rand | Returns random value |
| 0xB94E | divmod32 | `er0 = er0 / er1, er1 = er0 % er1` |
| 0xB9AE | mul32 | `er0 = er0 * er1` |
| 0xB9CE | memcpy | `void memcpy(r0 src, r1 dst, r2 len)` |
| 0x25F6 | decompress | `void decompress(u8* e0 out, u8* r0 in)` |

### Step Counting & Watts

| Address | Name | Signature / Notes |
|---------|------|-------------------|
| 0x1F3E | addWatts | `void addWatts(r0l watts)` — 8-bit |
| 0x1F40 | addWatts_word | `void addWatts_word(r0 watts)` — 16-bit, skips extu.w |
| 0x24AC | accountForStepTakenLikely | Step increment handler |
| 0x9454 | processAccelSamplesIncrementStepCountersAndNotifyInterestedPartiesAsNeeded | Main step processing |
| 0x60DA | processAcceleromerSamplesAxis | Process single axis data |
| 0xA1A8 | accelProcessDataDetectStep | Step detection algorithm |
| 0xA32E | clampTotalStepToLimit | Clamp to 9,999,999 max |
| 0xA45E | dayEndedRecordailyStepadnshipHistoricStepCounts | Midnight step rollover |
| 0xB124 | wipeOutStepsAndsyncTime | Clear steps and sync time |

### Accelerometer Hardware

| Address | Name | Signature / Notes |
|---------|------|-------------------|
| 0x26A0 | accelRegRead | `void accelRegRead(r0l regno, r0h nbytes, e0 dstRamPtr)` — OR's 0x80 into regno |
| 0x270A | accelRegWrite | `void accelRegWrite(r0l regno, r0h value)` |
| 0x273C | accelInit | `bool r0l accelInit()` |
| 0x76AA | accelReadSample | Read one accel sample |

### IR Communication

| Address | Name | Signature / Notes |
|---------|------|-------------------|
| 0x0714 | calcPacketCrc | `r0 = calcPacketCrc(r0 dataP, er1 len)` |
| 0x0772 | sendPacket | `void sendPacket(r0h cmd, r0l dataLen, r1l directionByte)` — payload at 0xF8D6 |
| 0x07F2 | sciConfigureForIrDA | Configure SCI3 for IrDA mode |
| 0x0822 | irTxByte | `void irTxByte(r0l byte)` — XORs with 0xAA before sending |
| 0x0832 | configureIrdaAndIrRxTimer | Set up IR RX with timer |
| 0x0880 | configureSciAndSendPingByte | Configure SCI + send beacon |
| 0x08D6 | irCommsEventLoop | IR communication event loop |
| 0x693A | setEventLoopFunc | Set main loop function pointer |

### Connection & Peer Play

| Address | Name | Signature / Notes |
|---------|------|-------------------|
| 0x03B4 | processSyncPacketFromGame | Handle SYN from DS game |
| 0x632C | goToPeerPlayMicroApp | Enter peer play mode |
| 0x6382 | calculateAndApplyPeerGift | Calculate peer play gift |
| 0x6784 | seeIfWeSawThisTrainerBefore | Check peer history |
| 0x67DE | pushSeenTrainersListLogDownOne | Shift trainer list |
| 0x6816 | logPeerPlay | Log peer play event |
| 0x694C | goToConnectionMicroApp | Enter CONNECT mode |
| 0x722C | connectionStartHandler | CONNECT initialization |
| 0x728A | showCommsErrorMessage | Show error (1-8) |

### Event Loop / Micro-Apps

| Address | Name | Description |
|---------|------|-------------|
| 0x7882 | sleepModeEventLoop | Sleep mode (screen off, counting steps) |
| 0x7998 | normalModeEventLoop | Normal mode handler |
| 0x6A3E | interactionHandlerHomeScreen | Home screen |
| 0x9756 | interactionHandlerMainMenu | Main menu |
| 0x47CE | interactionHandlerDowsing | Dowsing minigame |
| 0x9E72 | interactionHandlerPokeRadar | PokéRadar minigame |
| 0x2C62 | interactionHandlerPokeBattle | Battle handler |
| 0x6DFC | interactionHandlerSettings | Settings screen |
| 0x8D02 | interactionHandlerPokemonAndItemsScreen | Items screen |
| 0xB3CC | interactionHandlerTrainerCardAndStatsScreens | Trainer card |

### EEPROM Access

| Address | Name | Signature / Notes |
|---------|------|-------------------|
| 0x4FCA | eepromWriteSingleByte | `void eepromWriteSingleByte(r0 addr, r1l val)` |
| 0x524E | eepromWriteData | `void eepromWriteData(r0 eepaddr, e0 srcptr, r1 size)` |
| 0x5384 | eepromReadToRam | `void eepromReadToRam(r0 eepaddr, e0 dstptr, r1 size)` |
| 0x552E | eepromReadSingleByte | `u8 r0l eepromReadSingleByte(r0 eepaddr)` |
| 0x5634 | eepromFillPageWithByte | `void eepromFillPageWithByte(r0 addr, r1l val)` |
| 0x5742 | eepromFillWithByte | `void eepromFillWithByte(r0 addr, e0 size, r1l val)` |
| 0x5874 | eepromWrite128Bytes | `void eepromWrite128Bytes(r0 eepaddr, e0 srcptr)` |
| 0x50D8 | writeReliableDataToEeprom | `void writeReliableDataToEeprom(r0 addr0, e0 addr1, r1 src, e1 size)` |
| 0x5128 | readReliableDataFromEeprom | Reads dual-copy data with checksum verification |

### Display & LCD

| Address | Name | Signature / Notes |
|---------|------|-------------------|
| 0x7B72 | initLcdUsingRomOrEepromCommands | LCD init |
| 0x7C24 | lcdSetContrast | Add base_contrast_value (0xF7A9) |
| 0x7E58 | lcdFillRectangle | `void lcdFillRectangle(r0l x, r0h y, r1l w, r1h h, [sp] color)` |
| 0x80AC | drawImageToScreen | `void drawImageToScreen(r0l x, r0h y, e0 imgPtr, r1l imgW, r1h imgH)` |
| 0x858A | drawTinyChars | `void drawTinyChars(r0l x, r0h y, e0 char* text)` |
| 0x7FB8 | lcdExitPowerSaveMode | Wake LCD |
| 0x7FDA | lcdEnterPowerSaveMode | Sleep LCD |
| 0x1F6C | drawNumberAndWattsymbolOnscreen | Draw watts display |
| 0x1FEE | drawNumberOnscreen | Draw number at x position |

### Audio

| Address | Name | Signature / Notes |
|---------|------|-------------------|
| 0x3832 | setVolume | Set audio volume |
| 0x3838 | setBeeperFreq | `void setBeeperFreq(r0l period)` — period in 32 kHz ticks |
| 0x388C | timer_w_irq | Timer W IRQ handler (audio) |
| 0x3A4A | VECTOR_TIMER_W | Timer W vector |
| 0xBB4A | FREQTAB | Frequency table data |

### Battery & ADC

| Address | Name | Signature / Notes |
|---------|------|-------------------|
| 0x281E | adcSample | `u16 r0 adcSample()` |
| 0x289A | checkBatteryForBelowGivenLevel | `bool checkBatteryBelowGivenLevel(r0 level)` |
| 0x290A | measureBatteryAndSetLowFlag | Measure + update flag |
| 0x27C2 | checksumProtectAdcValue | 12-bit ADC checksum (T = (X+Y+Z) & 0x0F → 0xTXYZ) |

### Factory Test

| Address | Name |
|---------|------|
| 0x5990 | factoryTestPerformIfNeeded |
| 0xA72A | eepromFactoryTest |
| 0xA800 | rtcFactoryTest |
| 0xA830 | accelFactoryTest |
| 0xA8F8 | adcFactoryTestAndCalibrate |

### Walk Management

| Address | Name |
|---------|------|
| 0x048C | startWalkEepromActions |
| 0x0426 | copyTeamAndconfigDataFromStagingArea |
| 0x0636 | end_walk_action |
| 0x06DE | clearWattsAndErasePokemonStructure |
| 0x5C0A | pokemonJoinsEmptyWalkerWalk |
| 0x4F50 | checkIfPokeOrItemCanBeEncountered |
| 0xB1AE | some_sort_of_eeprom_init |
| 0xB2E2 | checkForAndInitEepromContentsIfNeeded |

## IO Port Registers

### SCI3 / IrDA (IO2)

| Address | Name | Notes |
|---------|------|-------|
| 0xFF98 | SMR3 | Serial Mode Register |
| 0xFF9B | TDR3 | Transmit Data Register |
| 0xFF9C | SSR3 | Serial Status Register |
| 0xFF9D | RDR3 | Receive Data Register |
| 0xFFA6 | SEMR | |
| 0xFFA7 | IrCR | IrDA Control Register |

### Audio Encoder / Compressor (IO2)

| Address | Name | Notes |
|---------|------|-------|
| 0xFF8C | ECPWCR | |
| 0xFF8E | ECPWDR | |
| 0xFF91 | SPCR | |
| 0xFF92 | AESGR | |
| 0xFF94 | ECCR | |
| 0xFF95 | ECCSR | |
| 0xFF96 | ECH/EC | |
| 0xFF97 | ECL | |

### RTC (IO1)

| Address | Name |
|---------|------|
| 0xF067 | RTCFLG |
| 0xF068 | RSECDR |
| 0xF069 | RMINDR |
| 0xF06A | RHRDR |
| 0xF06B | RWKDR |
| 0xF06C | RTCCR1 |
| 0xF06D | RTCCR2 |
| 0xF06F | RTCCSR |

### Timer B1 (IO1)

| Address | Name |
|---------|------|
| 0xF0D0 | TMB1 |
| 0xF0D1 | TCB1/TLB1 |

### Timer W (IO1) — used for audio PWM

| Address | Name |
|---------|------|
| 0xF0F0 | TMRW |
| 0xF0F1 | TCRW |
| 0xF0F2 | TIERW |
| 0xF0F3 | TSRW |
| 0xF0F4 | TIOR0 |
| 0xF0F5 | TIOR1 |
| 0xF0F6 | TCNT |
| 0xF0F8 | GRA |
| 0xF0FA | GRB |
| 0xF0FC | GRC |
| 0xF0FE | GRD |

### SSU / SPI (IO1)

| Address | Name |
|---------|------|
| 0xF0E0 | SSCRH/SSCR |
| 0xF0E1 | SSCRL |
| 0xF0E2 | SSMR |
| 0xF0E3 | SSER |
| 0xF0E4 | SSSR |
| 0xF0E9 | SSRDR |
| 0xF0EB | SSTDR |

### A/D Converter (IO2)

| Address | Name |
|---------|------|
| 0xFFBC | ADRR |
| 0xFFBE | AMR |
| 0xFFBF | ADSR |

### Watchdog Timer (IO2)

| Address | Name |
|---------|------|
| 0xFFB0 | TMWD |
| 0xFFB1 | TCSRWD1 |
| 0xFFB2 | TCSRWD2 |
| 0xFFB3 | TCDW |

### Interrupt Control (IO2)

| Address | Name |
|---------|------|
| 0xFFF0 | SYSCR1 |
| 0xFFF1 | SYSCR2 |
| 0xFFF2 | IEGR |
| 0xFFF3 | IENR1 |
| 0xFFF4 | IENR2 |
| 0xFFF5 | OSCCR |
| 0xFFF6 | IRR1 |
| 0xFFF7 | IRR2 |
| 0xFFFA | CKSTPR1 |
| 0xFFFB | CKSTPR2 |

## Interrupt Vectors

| Address | Name |
|---------|------|
| 0x005E | VECTOR_NMI |
| 0x0060 | VECTOR_TRAPA_0 |
| 0x0068 | VECTOR_CPU_SLEEP |
| 0x006A | VECTOR_COMP_0 |
| 0x006C | VECTOR_COMP_1 |
| 0x006E | VECTOR_RTC_WEEK |
| 0x0070 | VECTOR_RTC_ALARM |
| 0x0072 | VECTOR_WDT |
| 0x0076 | VECTOR_SSU_I2C |
| 0x06FA | VECTOR_TIMER_B1 |
| 0x075A | VECTOR_SCI3 |
| 0x3A4A | VECTOR_TIMER_W |
| 0xA300 | VECTOR_IRQ0 |
| 0xA31C | VECTOR_IRQ1 |
| 0xA322 | VECTOR_IRQ_AEC |
| 0xA328 | VECTOR_ADC |
| 0xA65E | VECTOR_RTC_QUARTER_SEC |
| 0xA674 | VECTOR_RTC_HALF_SEC |
| 0xA682 | VECTOR_RTC_EVERY_SEC |
| 0xA6E2 | VECTOR_RTC_EVERY_MIN |
| 0xA702 | VECTOR_RTC_EVERY_HOUR |

## IR Protocol

### Packet Format

8-byte header + 0–128 bytes payload. All bytes XOR'd with 0xAA on the wire.

```
Byte 0:    Command
Byte 1:    Extra/direction (0x01=g2w, 0x02=w2g)
Bytes 2-3: CRC-16 (LE)
Bytes 4-7: Session ID
Bytes 8+:  Payload
```

### CRC Algorithm

```c
uint16_t calcCrc(uint8_t* data, size_t len) {
    uint32_t crc = 0x0002;   // fixed start value
    for (size_t i = 0; i < len; i++)
        crc += data[i] << ((i & 1) ? 0 : 8);  // even bytes << 8, odd bytes << 0
    while (crc >> 16)
        crc = (crc & 0xffff) + (crc >> 16);    // fold carry
    return (uint16_t)crc;
}
```

### Command Set

| Cmd | Name | Direction | Notes |
|-----|------|-----------|-------|
| 0xFC | Beacon/ADV | W→* | Single byte, no header |
| 0xFA | SYN | Master→Slave | 8B header only, random session ID |
| 0xF8 | ACK | Slave→Master | 8B header only, slave's session ID |
| 0xF4 | RST | *→W | Reset connection |
| 0x24 | PING | G→W | |
| 0x26 | PONG | W→G | |
| 0x56 | KILL | *→W | Terminate |
| 0x66 | CLOSE | G→W | Close request |
| 0x68 | CLOSE_ACK | W→G | Close acknowledge |
| 0x20 | GENINFO_REQ | G→W | Request identity data |
| 0x22 | GENINFO_RSP | W→G | Identity response (0x68 bytes) |
| 0x02 | EEPWRITE_ALIGN | G→W | EEPROM write aligned 128B |
| 0x04 | EEPWRITE_RSP | W→G | EEPROM write ack |
| 0x06 | MEMWRITE | *→W | **Direct RAM/MMIO write** |
| 0x0A | EEPWRITE_REQ | G→W | EEPROM write request |
| 0x0C | EEPREAD_REQ | G→W | EEPROM read (addr16, len8) |
| 0x0E | EEPREAD_RSP | W→G | EEPROM read response |

### Connection Handshake

1. Slave sends 0xFC beacon every ~300ms
2. Master sends 0xFA SYN (random 32-bit session ID)
3. Slave responds 0xF8 ACK (its own random session ID)
4. Agreed session = master_session XOR slave_session

## Audio System

- **setBeeperFreq** (0x3838): Takes `r0l` = period in 32 kHz sub-clock ticks
- **setVolume** (0x3832): Set volume level
- Uses **Timer W** for PWM generation via output compare
- **FREQTAB** at 0xBB4A: Lookup table for note frequencies
- Timer W IRQ at 0x388C handles waveform generation

## Key Constants

| Name | Value | Notes |
|------|-------|-------|
| Steps per watt | 20 | stepToWattDividerState counts to 20 |
| Max watts | 9,999 | Hard-coded in addWatts |
| Max lifetime steps | 9,999,999 | Clamped at 0xA32E |
| Max daily steps | 99,999 | Checked at 0x24FA |
| ROM size | 0xC000 (49,152 B) | |
| RAM range | 0xF780–0xFF7F (2 KB) | |
| EEPROM size | 0x10000 (64 KB) | |

## Exploit / Code Injection Reference

### RCE via CMD 0x06 (MEMWRITE)

Write arbitrary bytes to RAM/MMIO. The ROM's packet handler writes payload to the address specified in the packet header.

### Injection Targets

1. **0xF7E0** (currentEventLoopFunc): Write a function pointer → ROM calls it in main loop
2. **0xF956** (DECOMPRESSION_BUFFER): Write executable code here, then point 0xF7E0 to 0xF956
3. **0xF79C** (stepCountTodaySoFar): Direct write to set step count

### Watts Exploit Payload (upload to 0xF956)

```asm
mov.w #9999, r0           ; 0x79 0x00 0x27 0x0F
jsr   addWatts_word       ; 0x5E 0x00 0x1F 0x40
mov.w #irCommsEventLoop, r0 ; 0x79 0x00 0x08 0xD6
jmp   setEventLoopFunc    ; 0x5A 0x00 0x69 0x3A
```
