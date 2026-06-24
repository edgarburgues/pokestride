/*
 * I2C bus 3 driver — direct MMIO access to SC16IS750 IR chip.
 * Adapted from https://github.com/francesco265/pwalkerHax
 * which in turn was adapted from https://github.com/profi200/libn3ds
 */
#include "i2c.h"
#include <stdbool.h>

static I2cBus *const g_i2cBus = (I2cBus*)I2C3_REGS_BASE;

static bool checkAck(I2cBus *const bus) {
    if ((bus->cnt & I2C_ACK) == 0) {
        bus->cnt = I2C_EN | I2C_IRQ_EN | I2C_ERROR | I2C_STOP;
        return false;
    }
    return true;
}

static void sendByte(I2cBus *const bus, u8 data, u8 params) {
    bus->data = data;
    bus->cnt  = I2C_EN | I2C_IRQ_EN | I2C_DIR_S | params;
    while (bus->cnt & I2C_EN);
}

static u8 recvByte(I2cBus *const bus, u8 params) {
    bus->cnt = I2C_EN | I2C_IRQ_EN | I2C_DIR_R | params;
    while (bus->cnt & I2C_EN);
    return bus->data;
}

static bool startTransfer(u32 devAddr, u32 regAddr, bool read) {
    u32 tries = 8;
    do {
        while (g_i2cBus->cnt & I2C_EN);

        sendByte(g_i2cBus, devAddr, I2C_START);
        if (!checkAck(g_i2cBus)) continue;

        sendByte(g_i2cBus, regAddr, 0);
        if (!checkAck(g_i2cBus)) continue;

        if (read) {
            sendByte(g_i2cBus, devAddr | BIT(0), I2C_START);
            if (!checkAck(g_i2cBus)) continue;
        }
        break;
    } while (--tries > 0);
    return tries > 0;
}

void I2C_init(void) {
    static bool inited = false;
    if (inited) return;
    inited = true;
    while (g_i2cBus->cnt & I2C_EN);
    g_i2cBus->cntex = I2C_CLK_STRETCH_EN;
    g_i2cBus->scl   = I2C_DELAYS(5u, 0u);
}

u8 I2C_read(u32 regAddr) {
    u8 data;
    if (!I2C_read_array(regAddr, &data, 1)) return 0xFF;
    return data;
}

bool I2C_write(u32 regAddr, u8 data) {
    return I2C_write_array(regAddr, &data, 1);
}

bool I2C_read_array(u32 regAddr, void *out, u32 size) {
    u8 *ptr = (u8*)out;
    if (!startTransfer(I2C_IR_ADDR, regAddr, true)) return false;
    while (--size) *ptr++ = recvByte(g_i2cBus, I2C_ACK);
    *ptr = recvByte(g_i2cBus, I2C_STOP);
    return true;
}

bool I2C_write_array(u32 regAddr, const void *in, u32 size) {
    const u8 *ptr = (const u8*)in;
    if (!startTransfer(I2C_IR_ADDR, regAddr, false)) return false;
    while (--size) {
        sendByte(g_i2cBus, *ptr++, 0);
        if (!checkAck(g_i2cBus)) return false;
    }
    sendByte(g_i2cBus, *ptr, I2C_STOP);
    if (!checkAck(g_i2cBus)) return false;
    return true;
}
