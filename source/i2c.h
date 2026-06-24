/*
 * I2C bus 3 driver — direct MMIO access to SC16IS750 IR chip.
 * Adapted from https://github.com/francesco265/pwalkerHax
 * which in turn was adapted from https://github.com/profi200/libn3ds
 */
#pragma once

#include <3ds/types.h>
#include <3ds/os.h>

/* SC16IS750 sits on I2C bus 3, 7-bit address 0x4D (write byte: 0x9A) */
#define I2C_IR_ADDR     (0x4Du << 1)          /* 0x9A */
#define I2C3_REGS_BASE  (OS_MMIO_VADDR + 0x48000u)

/* REG_I2C_CNT bit-fields */
#define I2C_STOP    BIT(0)
#define I2C_START   BIT(1)
#define I2C_ERROR   BIT(2)
#define I2C_ACK     BIT(4)
#define I2C_DIR_S   (0u)       /* direction: send  */
#define I2C_DIR_R   BIT(5)    /* direction: recv  */
#define I2C_IRQ_EN  BIT(6)
#define I2C_EN      BIT(7)

/* REG_I2C_CNTEX bit-fields */
#define I2C_CLK_STRETCH_EN  BIT(1)

#define I2C_DELAYS(high, low)  (((high) << 8) | (low))

typedef struct {
    vu8  data;   /* offset 0x0 */
    vu8  cnt;    /* offset 0x1 */
    vu16 cntex;  /* offset 0x2 */
    vu16 scl;    /* offset 0x4 */
} I2cBus;

void I2C_init(void);
u8   I2C_read(u32 regAddr);
bool I2C_write(u32 regAddr, u8 data);
bool I2C_read_array(u32 regAddr, void *out, u32 size);
bool I2C_write_array(u32 regAddr, const void *in, u32 size);
