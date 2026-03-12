// SPDX-License-Identifier: MIT

/*
  Original Copyright 2020 Claude Schwarz
  Code reorganized and rewritten by
  Niklas Ekström 2021 (https://github.com/niklasekstrom)
*/

#define PS_PROTOCOL_IMPL

#include <stdint.h>

#include "config.h"
#include "support.h"
#include "tlsf.h"
#include "ps_protocol.h"
#include "M68k.h"
#include "cache.h"

volatile unsigned int *gpio;
volatile unsigned int *gpclk;

unsigned int gpfsel0;
unsigned int gpfsel1;
unsigned int gpfsel2;

unsigned int gpfsel0_o;
unsigned int gpfsel1_o;
unsigned int gpfsel2_o;

uint32_t INPUT[3] = {
    0, 0, 0
};

uint32_t OUTPUT[3] = {
    0, 0, 0
};

uint32_t CLEAR_BITS = 0;

#define BITBANG_DELAY PISTORM_BITBANG_DELAY

#define CHIPSET_DELAY PISTORM_CHIPSET_DELAY
#define CIA_DELAY     PISTORM_CIA_DELAY

volatile uint8_t gpio_lock;

static void usleep(uint64_t delta)
{
    uint64_t hi = LE32(*(volatile uint32_t*)0xf2003008);
    uint64_t lo = LE32(*(volatile uint32_t*)0xf2003004);
    uint64_t t1, t2;

    if (hi != LE32(*(volatile uint32_t*)0xf2003008))
    {
        hi = LE32(*(volatile uint32_t*)0xf2003008);
        lo = LE32(*(volatile uint32_t*)0xf2003004);
    }

    t1 = (hi << 32) | lo;
    t1 += delta;
    t2 = 0;

    do {
        hi = LE32(*(volatile uint32_t*)0xf2003008);
        lo = LE32(*(volatile uint32_t*)0xf2003004);
        if (hi != LE32(*(volatile uint32_t*)0xf2003008))
        {
            hi = LE32(*(volatile uint32_t*)0xf2003008);
            lo = LE32(*(volatile uint32_t*)0xf2003004);
        }
        t2 = (hi << 32) | lo;
    } while (t2 < t1);
}

static inline void ticksleep(uint64_t ticks)
{
    uint64_t t0 = 0, t1 = 0;
    asm volatile("mrs %0, CNTPCT_EL0":"=r"(t0));
    t0 += ticks;
    do {
        asm volatile("mrs %0, CNTPCT_EL0":"=r"(t1));
    } while(t1 < t0);
}

static inline void ticksleep_wfe(uint64_t ticks)
{
    uint64_t t0 = 0, t1 = 0;
    asm volatile("mrs %0, CNTPCT_EL0":"=r"(t0));
    t0 += ticks;
    do {
        asm volatile("mrs %0, CNTPCT_EL0":"=r"(t1));
        asm volatile("wfe");
    } while(t1 < t0);
}

#define TXD_BIT (1 << 26)

uint32_t bitbang_delay;
 
void bitbang_putByte(uint8_t byte)
{
    if (!gpio)
        gpio = ((volatile unsigned *)BCM2708_PERI_BASE) + GPIO_ADDR / 4;

    uint64_t t0 = 0, t1 = 0;
    asm volatile("mrs %0, CNTPCT_EL0":"=r"(t0));

    *(gpio + 10) = LE32(TXD_BIT); // Start bit - 0
  
    do {
        asm volatile("mrs %0, CNTPCT_EL0":"=r"(t1));
    } while(t1 < (t0 + bitbang_delay));
  
    for (int i=0; i < 8; i++) {
        asm volatile("mrs %0, CNTPCT_EL0":"=r"(t0));

        if (byte & 1)
            *(gpio + 7) = LE32(TXD_BIT);
        else
            *(gpio + 10) = LE32(TXD_BIT);
        byte = byte >> 1;

        do {
            asm volatile("mrs %0, CNTPCT_EL0":"=r"(t1));
        } while(t1 < (t0 + bitbang_delay));
    }
    asm volatile("mrs %0, CNTPCT_EL0":"=r"(t0));

    *(gpio + 7) = LE32(TXD_BIT);  // Stop bit - 1

    do {
        asm volatile("mrs %0, CNTPCT_EL0":"=r"(t1));
    } while(t1 < (t0 + 3*bitbang_delay / 2));
}

#define FS_CLK  (1 << 26)
#define FS_DO   (1 << 27)
#define FS_CTS  (1 << 25)

void (*fs_putByte)(uint8_t);

void fastSerial_putByte_pi3(uint8_t byte)
{
    if (!gpio)
        gpio = ((volatile unsigned *)BCM2708_PERI_BASE) + GPIO_ADDR / 4;
  
    /* Wait for CTS to go high */
    //while (0 == (*(gpio + 13) & LE32(FS_CTS))) {}

    /* Start bit */
    *(gpio + 10) = LE32(FS_DO);

    /* Clock down */
    *(gpio + 10) = LE32(FS_CLK);
    //*(gpio + 10) = LE32(FS_CLK);
    /* Clock up */
    *(gpio + 7) = LE32(FS_CLK);
    //*(gpio + 7) = LE32(FS_CLK);

    for (int i=0; i < 8; i++) {
        if (byte & 1)
            *(gpio + 7) = LE32(FS_DO);
        else
            *(gpio + 10) = LE32(FS_DO);

        /* Clock down */
        *(gpio + 10) = LE32(FS_CLK);
        //*(gpio + 10) = LE32(FS_CLK);
        /* Clock up */
        *(gpio + 7) = LE32(FS_CLK);
        //*(gpio + 7) = LE32(FS_CLK);
        
        byte = byte >> 1;
    }

    /* DEST bit (0) */
    *(gpio + 10) = LE32(FS_DO);

    /* Clock down */
    *(gpio + 10) = LE32(FS_CLK);
    //*(gpio + 10) = LE32(FS_CLK);
    /* Clock up */
    *(gpio + 7) = LE32(FS_CLK);
    *(gpio + 7) = LE32(FS_CLK);

    /* Leave FS_CLK and FS_DO high */
    *(gpio + 7) = LE32(FS_CLK);
    *(gpio + 7) = LE32(FS_DO);
}

void fastSerial_putByte_pi4(uint8_t byte)
{
    if (!gpio)
        gpio = ((volatile unsigned *)BCM2708_PERI_BASE) + GPIO_ADDR / 4;
  
    /* Start bit */
    *(gpio + 10) = LE32(FS_DO);

    /* Clock down */
    *(gpio + 10) = LE32(FS_CLK);
    *(gpio + 10) = LE32(FS_CLK);
    /* Clock up */
    *(gpio + 7) = LE32(FS_CLK);
    *(gpio + 7) = LE32(FS_CLK);

    for (int i=0; i < 8; i++) {
        if (byte & 1)
            *(gpio + 7) = LE32(FS_DO);
        else
            *(gpio + 10) = LE32(FS_DO);

        /* Clock down */
        *(gpio + 10) = LE32(FS_CLK);
        *(gpio + 10) = LE32(FS_CLK);
        /* Clock up */
        *(gpio + 7) = LE32(FS_CLK);
        *(gpio + 7) = LE32(FS_CLK);
        
        byte = byte >> 1;
    }

    /* DEST bit (0) */
    *(gpio + 10) = LE32(FS_DO);

    /* Clock down */
    *(gpio + 10) = LE32(FS_CLK);
    *(gpio + 10) = LE32(FS_CLK);
    /* Clock up */
    *(gpio + 7) = LE32(FS_CLK);
    *(gpio + 7) = LE32(FS_CLK);

    /* Leave FS_CLK and FS_DO high */
    *(gpio + 7) = LE32(FS_CLK);
    *(gpio + 7) = LE32(FS_DO);
}

void fastSerial_reset()
{
    if (!gpio)
        gpio = ((volatile unsigned *)BCM2708_PERI_BASE) + GPIO_ADDR / 4;

    /* Leave FS_CLK and FS_DO high */
    *(gpio + 7) = LE32(FS_CLK);
    *(gpio + 7) = LE32(FS_DO);

    for (int i=0; i < 16; i++) {
        /* Clock down */
        *(gpio + 10) = LE32(FS_CLK);
        *(gpio + 10) = LE32(FS_CLK);
        /* Clock up */
        *(gpio + 7) = LE32(FS_CLK);
        *(gpio + 7) = LE32(FS_CLK);
    }
}

void fastSerial_putByte(uint8_t byte)
{
    static char reset_pending = 0;

    if (reset_pending)
    {
        fastSerial_reset();
        reset_pending = 0;
    }

    if (!gpio)
        gpio = ((volatile unsigned *)BCM2708_PERI_BASE) + GPIO_ADDR / 4;
  
    if (fs_putByte)
        fs_putByte(byte);
    
    if (byte == 10)
        reset_pending = 1;
}

void fastSerial_init()
{
    uint64_t tmp;

    if (!gpio)
        gpio = ((volatile unsigned *)BCM2708_PERI_BASE) + GPIO_ADDR / 4;
  
    asm volatile("mrs %0, CNTFRQ_EL0":"=r"(tmp));

    if (tmp > 20000000)
    {
        fs_putByte = fastSerial_putByte_pi4;
    }
    else
    {
        fs_putByte = fastSerial_putByte_pi3;
    }   

    fastSerial_reset();
}


static void pistorm_setup_io() {
    gpio = ((volatile unsigned *)BCM2708_PERI_BASE) + GPIO_ADDR / 4;
    gpclk = ((volatile unsigned *)BCM2708_PERI_BASE) + GPCLK_ADDR / 4;
}

static void setup_gpclk() {
    // Enable 200MHz CLK output on GPIO4, adjust divider and pll source depending
    // on pi model
    uint64_t tmp;
    asm volatile("mrs %0, CNTFRQ_EL0":"=r"(tmp));

    *(gpclk + (CLK_GP0_CTL / 4)) = LE32(CLK_PASSWD | (1 << 5));
    usleep(10);
    while ((*(gpclk + (CLK_GP0_CTL / 4))) & LE32(1 << 7));
    usleep(100);
    if (tmp > 20000000)
        *(gpclk + (CLK_GP0_DIV / 4)) =
            LE32(CLK_PASSWD | (5 << 12));  // divider , 3=200MHz on pi4
    else
        *(gpclk + (CLK_GP0_DIV / 4)) =
            LE32(CLK_PASSWD | (6 << 12));  // divider , 6=200MHz on pi3
    usleep(10);
    *(gpclk + (CLK_GP0_CTL / 4)) =
        LE32(CLK_PASSWD | 5 | (1 << 4));  // pll? 6=plld, 5=pllc
    usleep(10);
    while (((*(gpclk + (CLK_GP0_CTL / 4))) & LE32(1 << 7)) == 0);
    usleep(100);

    SET_GPIO_ALT(PIN_CLK, 0);  // gpclk0
}

static unsigned int ps_read_16_int_nowbwait(unsigned int address);

/* Ensure MMIO stores reach the peripheral before continuing */
#define GPIO_SETTLE() asm volatile("dsb sy" ::: "memory")

/* Mask/unmask IRQ+FIQ during bus cycles */
#define BUS_IRQ_OFF() asm volatile("msr DAIFSet, #3" ::: "memory")
#define BUS_IRQ_ON()  asm volatile("msr DAIFClr, #3" ::: "memory")

/* TXN timeout: if FPGA doesn't complete within this many spins, reset and retry */
#define TXN_TIMEOUT 10000
#define TXN_MAX_RETRIES 3

#define WR_STROBE() do { \
    *(gpio + 7)  = LE32(1 << PIN_WR); \
    *(gpio + 7)  = LE32(1 << PIN_WR); \
    *(gpio + 7)  = LE32(1 << PIN_WR); \
    *(gpio + 7)  = LE32(1 << PIN_WR); \
    *(gpio + 7)  = LE32(1 << PIN_WR); \
    *(gpio + 7)  = LE32(1 << PIN_WR); \
    *(gpio + 10) = LE32(1 << PIN_WR); \
    *(gpio + 10) = LE32(1 << PIN_WR); \
    *(gpio + 10) = LE32(1 << PIN_WR); \
} while(0)

#define RD_STROBE() do { \
    *(gpio + 7)  = LE32(1 << PIN_RD); \
    *(gpio + 7)  = LE32(1 << PIN_RD); \
    *(gpio + 7)  = LE32(1 << PIN_RD); \
    *(gpio + 7)  = LE32(1 << PIN_RD); \
    *(gpio + 7)  = LE32(1 << PIN_RD); \
    *(gpio + 7)  = LE32(1 << PIN_RD); \
} while(0)

static inline void ps_reset_bus(void)
{
    /* Clear all data/control pins */
    *(gpio + 10) = LE32(CLEAR_BITS);
    /* Set pins to input */
    *(gpio + 0) = LE32(INPUT[0]);
    *(gpio + 1) = LE32(INPUT[1]);
    *(gpio + 2) = LE32(INPUT[2]);
    /* Quick state machine reset via status register */
    ps_write_status_reg(STATUS_BIT_INIT);
    usleep(100);
    ps_write_status_reg(0);
    usleep(50);
    /* Dummy read to warm up FPGA after reset */
    (void)ps_read_16_int_nowbwait(0);
}

void ps_setup_protocol() {
    uint64_t clock;
    uint64_t delay;

    /* Setup bitbang RS232 delay based on the RS232 speed and CPU tick frequency */
    asm volatile("mrs %0, CNTFRQ_EL0":"=r"(clock));
    delay = (clock + PISTORM_BITBANG_SPEED / 2) / PISTORM_BITBANG_SPEED;
    bitbang_delay = delay;

    pistorm_setup_io();
    setup_gpclk();

    uint64_t tmp;
    asm volatile("mrs %0, CNTFRQ_EL0":"=r"(tmp));

    /* Pi4, CM4 */
    if (tmp > 20000000)
    {
        CLEAR_BITS = CLEAR_BITS_PI4;
        
        OUTPUT[0] = GPFSEL0_OUTPUT_PI4;
        OUTPUT[1] = GPFSEL1_OUTPUT_PI4;
        OUTPUT[2] = GPFSEL2_OUTPUT_PI4;
        
        INPUT[0] = GPFSEL0_INPUT_PI4;
        INPUT[1] = GPFSEL1_INPUT_PI4;
        INPUT[2] = GPFSEL2_INPUT_PI4;
    }
    else
    {
        CLEAR_BITS = CLEAR_BITS_PI3;
        
        OUTPUT[0] = GPFSEL0_OUTPUT_PI3;
        OUTPUT[1] = GPFSEL1_OUTPUT_PI3;
        OUTPUT[2] = GPFSEL2_OUTPUT_PI3;
        
        INPUT[0] = GPFSEL0_INPUT_PI3;
        INPUT[1] = GPFSEL1_INPUT_PI3;
        INPUT[2] = GPFSEL2_INPUT_PI3;
    }

    *(gpio + 10) = LE32(CLEAR_BITS);

    *(gpio + 0) = LE32(INPUT[0]);
    *(gpio + 1) = LE32(INPUT[1]);
    *(gpio + 2) = LE32(INPUT[2]);

    *(gpio + 7) = LE32(TXD_BIT);

#ifdef MAC68K
    /* Reset FPGA state machine and do warm-up reads.
       The first bus cycle after reset may not assert TXN. */
    ps_reset_state_machine();
    {
        unsigned int d;
        for (int i = 0; i < 3; i++) {
            d = ps_read_16_int_nowbwait(0);
            kprintf("[GPIO] Warm-up read %d: %04x\n", i, d);
        }
    }
#endif
}

static void ps_write_8_int(unsigned int address, unsigned int data);

static void ps_write_16_int(unsigned int address, unsigned int data)
{
    address &= 0xffffff;

    if (address & 1)
    {
        ps_write_8_int(address, data >> 8);
        ps_write_8_int(address + 1, data & 0xff);
    }
    else
    {
        int retries = TXN_MAX_RETRIES;
retry_w16:
        BUS_IRQ_OFF();
        *(gpio + 0) = LE32(OUTPUT[0]);
        *(gpio + 1) = LE32(OUTPUT[1]);
        *(gpio + 2) = LE32(OUTPUT[2]);

        *(gpio + 7) = LE32(((data & 0xffff) << 8) | (REG_DATA << PIN_A0));
        GPIO_SETTLE();
        WR_STROBE();
        *(gpio + 10) = LE32(CLEAR_BITS);

        *(gpio + 7) = LE32(((address & 0xffff) << 8) | (REG_ADDR_LO << PIN_A0));
        GPIO_SETTLE();
        WR_STROBE();
        *(gpio + 10) = LE32(CLEAR_BITS);

        *(gpio + 7) = LE32(((0x0000 | ((address >> 16) & 0x00ff)) << 8) | (REG_ADDR_HI << PIN_A0));
        GPIO_SETTLE();
        WR_STROBE();
        *(gpio + 10) = LE32(CLEAR_BITS);

        *(gpio + 0) = LE32(INPUT[0]);
        *(gpio + 1) = LE32(INPUT[1]);
        *(gpio + 2) = LE32(INPUT[2]);

        {
            int timeout = TXN_TIMEOUT;
            while (*(gpio + 13) & LE32((1 << PIN_TXN_IN_PROGRESS))) {
                if (--timeout == 0) {
                    BUS_IRQ_ON();
                    if (--retries > 0) { ps_reset_bus(); goto retry_w16; }
                    break;
                }
            }
        }
        BUS_IRQ_ON();
    }
}

static void ps_write_8_int(unsigned int address, unsigned int data)
{
    address &= 0xffffff;

    data = (data & 0xff) | (data << 8);

    int retries = TXN_MAX_RETRIES;
retry_w8:
    BUS_IRQ_OFF();
    *(gpio + 0) = LE32(OUTPUT[0]);
    *(gpio + 1) = LE32(OUTPUT[1]);
    *(gpio + 2) = LE32(OUTPUT[2]);

    *(gpio + 7) = LE32(((data & 0xffff) << 8) | (REG_DATA << PIN_A0));
    GPIO_SETTLE();
    WR_STROBE();
    *(gpio + 10) = LE32(CLEAR_BITS);

    *(gpio + 7) = LE32(((address & 0xffff) << 8) | (REG_ADDR_LO << PIN_A0));
    GPIO_SETTLE();
    WR_STROBE();
    *(gpio + 10) = LE32(CLEAR_BITS);

    *(gpio + 7) = LE32(((0x0100 | ((address >> 16) & 0x00ff)) << 8) | (REG_ADDR_HI << PIN_A0));
    GPIO_SETTLE();
    WR_STROBE();
    *(gpio + 10) = LE32(CLEAR_BITS);

    *(gpio + 0) = LE32(INPUT[0]);
    *(gpio + 1) = LE32(INPUT[1]);
    *(gpio + 2) = LE32(INPUT[2]);

    {
        int timeout = TXN_TIMEOUT;
        while (*(gpio + 13) & LE32((1 << PIN_TXN_IN_PROGRESS))) {
            if (--timeout == 0) {
                BUS_IRQ_ON();
                if (--retries > 0) { ps_reset_bus(); goto retry_w8; }
                break;
            }
        }
    }
    BUS_IRQ_ON();
}

static void ps_write_32_int(unsigned int address, unsigned int value)
{
    if (address & 1)
    {
        ps_write_8_int(address, value >> 24);
        ps_write_16_int(address + 1, value >> 8);
        ps_write_8_int(address + 3, value & 0xff);
    }
    else
    {
        ps_write_16_int(address, value >> 16);
        ps_write_16_int(address + 2, value);
    }
}

static unsigned int ps_read_16_int_nowbwait(unsigned int address)
{
    address &= 0xffffff;

    if (address & 1)
    {
        unsigned int value;

        value = ps_read_8(address) << 8;
        value |= ps_read_8(address + 1);

        return value;
    }
    else
    {
        int retries = TXN_MAX_RETRIES;
retry_r16:
        BUS_IRQ_OFF();
        *(gpio + 0) = LE32(OUTPUT[0]);
        *(gpio + 1) = LE32(OUTPUT[1]);
        *(gpio + 2) = LE32(OUTPUT[2]);

        *(gpio + 7) = LE32(((address & 0xffff) << 8) | (REG_ADDR_LO << PIN_A0));
        GPIO_SETTLE();
        WR_STROBE();
        *(gpio + 10) = LE32(CLEAR_BITS);

        *(gpio + 7) = LE32(((0x0200 | ((address >> 16) & 0x00ff)) << 8) | (REG_ADDR_HI << PIN_A0));
        GPIO_SETTLE();
        WR_STROBE();
        *(gpio + 10) = LE32(CLEAR_BITS);

        *(gpio + 0) = LE32(INPUT[0]);
        *(gpio + 1) = LE32(INPUT[1]);
        *(gpio + 2) = LE32(INPUT[2]);

        *(gpio + 7) = LE32(REG_DATA << PIN_A0);
        GPIO_SETTLE();
        RD_STROBE();

        {
            int timeout = TXN_TIMEOUT;
            while (*(gpio + 13) & LE32(1 << PIN_TXN_IN_PROGRESS)) {
                if (--timeout == 0) {
                    *(gpio + 10) = LE32(CLEAR_BITS);
                    BUS_IRQ_ON();
                    if (--retries > 0) { ps_reset_bus(); goto retry_r16; }
                    return 0xffff;
                }
            }
            unsigned int value = LE32(*(gpio + 13));

            *(gpio + 10) = LE32(CLEAR_BITS);
            BUS_IRQ_ON();
            return (value >> 8) & 0xffff;
        }
    }
}

unsigned int ps_read_16_int(unsigned int address)
{
#if PISTORM_WRITE_BUFFER
    wb_waitfree();
#endif
    return ps_read_16_int_nowbwait(address);
}

unsigned int ps_read_8_int(unsigned int address)
{
#if PISTORM_WRITE_BUFFER
    wb_waitfree();
#endif

    address &= 0xffffff;

    int retries = TXN_MAX_RETRIES;
retry_r8:
    BUS_IRQ_OFF();
    *(gpio + 0) = LE32(OUTPUT[0]);
    *(gpio + 1) = LE32(OUTPUT[1]);
    *(gpio + 2) = LE32(OUTPUT[2]);

    *(gpio + 7) = LE32(((address & 0xffff) << 8) | (REG_ADDR_LO << PIN_A0));
    GPIO_SETTLE();
    WR_STROBE();
    *(gpio + 10) = LE32(CLEAR_BITS);

    *(gpio + 7) = LE32(((0x0300 | ((address >> 16) & 0x00ff)) << 8) | (REG_ADDR_HI << PIN_A0));
    GPIO_SETTLE();
    WR_STROBE();
    *(gpio + 10) = LE32(CLEAR_BITS);

    *(gpio + 0) = LE32(INPUT[0]);
    *(gpio + 1) = LE32(INPUT[1]);
    *(gpio + 2) = LE32(INPUT[2]);

    *(gpio + 7) = LE32(REG_DATA << PIN_A0);
    GPIO_SETTLE();
    RD_STROBE();

    {
        int timeout = TXN_TIMEOUT;
        while (*(gpio + 13) & LE32(1 << PIN_TXN_IN_PROGRESS)) {
            if (--timeout == 0) {
                *(gpio + 10) = LE32(CLEAR_BITS);
                BUS_IRQ_ON();
                if (--retries > 0) { ps_reset_bus(); goto retry_r8; }
                return 0xff;
            }
        }
        unsigned int value = LE32(*(gpio + 13));

        *(gpio + 10) = LE32(CLEAR_BITS);
        BUS_IRQ_ON();

        value = (value >> 8) & 0xffff;

        if ((address & 1) == 0)
            return (value >> 8) & 0xff;  // EVEN, A0=0,UDS
        else
            return value & 0xff;  // ODD , A0=1,LDS
    }
}

unsigned int ps_read_32_int(unsigned int address)
{
#if PISTORM_WRITE_BUFFER
    wb_waitfree();
#endif

    if (address & 1)
    {
        unsigned int value;
        value = ps_read_8(address) << 24;
        value |= ps_read_16(address + 1) << 8;
        value |= ps_read_8(address + 3);
        return value;
    }
    else
    {
        unsigned int a = ps_read_16(address);
        unsigned int b = ps_read_16(address + 2);
        return (a << 16) | b;
    }
}

void ps_write_status_reg(unsigned int value)
{
    *(gpio + 0) = LE32(OUTPUT[0]);
    *(gpio + 1) = LE32(OUTPUT[1]);
    *(gpio + 2) = LE32(OUTPUT[2]);

    *(gpio + 7) = LE32(((value & 0xffff) << 8) | (REG_STATUS << PIN_A0));
    GPIO_SETTLE();
    WR_STROBE();
    *(gpio + 10) = LE32(CLEAR_BITS);

    *(gpio + 0) = LE32(INPUT[0]);
    *(gpio + 1) = LE32(INPUT[1]);
    *(gpio + 2) = LE32(INPUT[2]);
}

unsigned int ps_read_status_reg()
{
    *(gpio + 7) = LE32(REG_STATUS << PIN_A0);
    GPIO_SETTLE();
    RD_STROBE();
    unsigned int value = LE32(*(gpio + 13));

    *(gpio + 10) = LE32(CLEAR_BITS);

    return (value >> 8) & 0xffff;
}

void ps_reset_state_machine()
{
    ps_write_status_reg(STATUS_BIT_INIT);
    usleep(1500);
    ps_write_status_reg(0);
    usleep(100);
}

#include <boards.h>
extern struct ExpansionBoard **board;
extern struct ExpansionBoard *__boards_start;
extern int board_idx;
extern uint32_t overlay;

void ps_pulse_reset()
{
    ps_write_status_reg(0);
    usleep(30000);
    ps_write_status_reg(STATUS_BIT_RESET);
    
    overlay = 1;
    board = &__boards_start;
    board_idx = 0;
}

unsigned int ps_get_ipl_zero()
{
    unsigned int value = (*(gpio + 13));
    return value & LE32(1 << PIN_IPL_ZERO);
}

#define INT2_ENABLED 1

#define PM_RSTC         ((volatile unsigned int*)(0xf2000000 + 0x0010001c))
#define PM_RSTS         ((volatile unsigned int*)(0xf2000000 + 0x00100020))
#define PM_WDOG         ((volatile unsigned int*)(0xf2000000 + 0x00100024))
#define PM_WDOG_MAGIC   0x5a000000
#define PM_RSTC_FULLRST 0x00000020

volatile int housekeeper_enabled = 0;
extern struct M68KState *__m68k_state;

void ps_housekeeper() 
{
    if (!gpio)
        gpio = ((volatile unsigned *)BCM2708_PERI_BASE) + GPIO_ADDR / 4;
  
    extern uint64_t arm_cnt;
    uint64_t t0;
    uint64_t last_arm_cnt = arm_cnt;

    asm volatile("mrs %0, CNTPCT_EL0":"=r"(t0));
    asm volatile("mrs %0, PMCCNTR_EL0":"=r"(last_arm_cnt));

    kprintf("[HKEEP] Housekeeper activated\n");
    kprintf("[HKEEP] Please note we are burning the cpu with busyloops now\n");

    /* Configure timer-based event stream */
    /* Enable timer regs from EL0, enable event stream on posedge, monitor 2th bit */
    /* This gives a frequency of 2.4MHz for a 19.2MHz timer */
    uint64_t tmp;
    asm volatile("mrs %0, CNTFRQ_EL0":"=r"(tmp));

    if (tmp > 20000000)
    {
        asm volatile("msr CNTKCTL_EL1, %0"::"r"(3 | (1 << 2) | (3 << 8) | (3 << 4)));
    }
    else
    {
        asm volatile("msr CNTKCTL_EL1, %0"::"r"(3 | (1 << 2) | (3 << 8) | (2 << 4)));
    }

    for(;;) {
        if (housekeeper_enabled)
        {
            uint32_t pin = LE32(*(gpio + 13));
            __m68k_state->INT.IPL = (pin & (1 << PIN_IPL_ZERO)) ? 0 : 1;

            asm volatile("":::"memory");

            if (__m68k_state->INT.IPL)
                asm volatile("sev":::"memory");

            if ((pin & (1 << PIN_RESET)) == 0) {
                kprintf("[HKEEP] Houskeeper will reset RasPi now...\n");

                unsigned int r;
                // trigger a restart by instructing the GPU to boot from partition 0
                r = LE32(*PM_RSTS); r &= ~0xfffffaaa;
                *PM_RSTS = LE32(PM_WDOG_MAGIC | r);   // boot from partition 0
                *PM_WDOG = LE32(PM_WDOG_MAGIC | 10);
                *PM_RSTC = LE32(PM_WDOG_MAGIC | PM_RSTC_FULLRST);

                while(1);
            }

            /*
              Wait for event. It can happen that the CPU is flooded with them for some reason, but
              nevertheless, thanks for the event stream set up above, they will appear at 1.2MHz in worst case
            */
            asm volatile("wfe");
        }
    }
}

#if PISTORM_WRITE_BUFFER

#define WRITEBUFFER_SIZE  PISTORM_WRITE_BUFFER_SIZE

struct WriteRequest {
    uint32_t  wr_addr;
    uint32_t  wr_value;
    uint8_t   wr_size;
};

struct WriteRequest *wr_buffer;
volatile uint32_t wr_head;
volatile uint32_t wr_tail;
volatile unsigned char bus_lock = 0;

void wb_push(uint32_t address, uint32_t value, uint8_t size)
{
    while(wr_tail + WRITEBUFFER_SIZE <= wr_head)
        asm volatile("yield");
    
    wr_buffer[wr_head & (WRITEBUFFER_SIZE - 1)].wr_addr = address;
    wr_buffer[wr_head & (WRITEBUFFER_SIZE - 1)].wr_value = value;
    wr_buffer[wr_head & (WRITEBUFFER_SIZE - 1)].wr_size = size;

    asm volatile("dmb sy":::"memory");

    __sync_add_and_fetch(&wr_head, 1);
    
    asm volatile("sev");
}

struct WriteRequest wb_pop()
{
    while (wr_tail == wr_head) {
        asm volatile("wfe");
    }

    struct WriteRequest data = wr_buffer[wr_tail & (WRITEBUFFER_SIZE - 1)];

    __sync_add_and_fetch(&wr_tail, 1);
    
    return data;
}

struct WriteRequest wb_peek()
{
    while (wr_tail == wr_head) {
        asm volatile("wfe");
    }

    struct WriteRequest data = wr_buffer[wr_tail & (WRITEBUFFER_SIZE - 1)];

    return data;
}

void wb_wait()
{
    while (wr_tail == wr_head) {
        asm volatile("wfe");
    }
}

void wb_waitfree()
{
    while (wr_tail != wr_head)
        asm volatile("yield");
}
#endif

void wb_init()
{
#if PISTORM_WRITE_BUFFER
    wr_buffer = tlsf_malloc(tlsf, sizeof(struct WriteRequest) * WRITEBUFFER_SIZE);
    wr_head = wr_tail = 0;
    bus_lock = 0;
#endif
}

static inline void check_blit_active(unsigned int addr, unsigned int size)
{
    if (!__m68k_state || !(__m68k_state->JIT_CONTROL2 & JC2F_BLITWAIT))
        return;

    addr &= 0xffffff;

    const uint32_t bstart = 0xDFF040;   // BLTCON0
    const uint32_t bend = 0xDFF076;     // BLTADAT+2
    if (addr >= bend || addr + size <= bstart)
        return;

    const uint16_t mask = 1<<14 | 1<<9 | 1<<6; // BBUSY | DMAEN | BLTEN
    while ((ps_read_16_int_nowbwait(0xdff002) & mask) == mask) {
        // Dummy reads to not steal too many cycles from the blitter.
        // But don't use e.g. CIA reads as we expect the operation
        // to finish soon.
        ps_read_16_int_nowbwait(0x00f00000);
        ps_read_16_int_nowbwait(0x00f00000);
    }
}

void wb_task()
{
#if PISTORM_WRITE_BUFFER
    kprintf("[WBACK] Write buffer activated\n");

    while(1) {
        struct WriteRequest req = wb_peek();

        while(__atomic_test_and_set(&bus_lock, __ATOMIC_ACQUIRE)) { asm volatile("yield"); }

        check_blit_active(req.wr_addr, req.wr_size);

        switch (req.wr_size) {
            case 1:
                ps_write_8_int(req.wr_addr, req.wr_value);
                break;
            case 2:
                ps_write_16_int(req.wr_addr, req.wr_value);
                break;
            case 4:
                ps_write_32_int(req.wr_addr, req.wr_value);
                break;
        }
#if CIA_DELAY
        if (req.wr_addr >= 0xbf0000 && req.wr_addr <= 0xbfffff) {
            ticksleep(CIA_DELAY);
        }
#endif
#if CHIPSET_DELAY
        if (req.wr_addr >= 0xa00000) {
            ticksleep(CHIPSET_DELAY);
        }
#endif
        __atomic_clear(&bus_lock, __ATOMIC_RELEASE);

        wb_pop();
    }
#else
    while(1) asm volatile("wfi");
#endif
}

void ps_write_8(unsigned int address, unsigned int data)
{
#if PISTORM_WRITE_BUFFER
    if (address < 0xa00000)
    {
        wb_push(address, data, 1);
    }
    else {
        wb_push(address, data, 1);
        wb_waitfree();
    }
#else
    ps_write_8_int(address, data);
#if CIA_DELAY
    if (address >= 0xbf0000 && address <= 0xbfffff) {
        ticksleep(CIA_DELAY);
    }
#endif
#if CHIPSET_DELAY
    if (address >= 0xa00000) {
        ticksleep(CHIPSET_DELAY);
    }
#endif
#endif
    cache_invalidate_range(ICACHE, address, 1);
}

void ps_write_16(unsigned int address, unsigned int data)
{
#if PISTORM_WRITE_BUFFER
    if (address < 0xa00000)
    {
        wb_push(address, data, 2);
    }
    else {
        wb_push(address, data, 2);
        wb_waitfree();
    }
#else
    check_blit_active(address, 2);
    ps_write_16_int(address, data);
#if CIA_DELAY
    if (address >= 0xbf0000 && address <= 0xbfffff) {
        ticksleep(CIA_DELAY);
    }
#endif
#if CHIPSET_DELAY
    if (address >= 0xa00000) {
        ticksleep(CHIPSET_DELAY);
    }
#endif
#endif
    cache_invalidate_range(ICACHE, address, 2);
}

void ps_write_32(unsigned int address, unsigned int data)
{
#if PISTORM_WRITE_BUFFER
    if (address < 0xa00000)
    {
        wb_push(address, data, 4);
    }
    else {
        wb_push(address, data, 4);
        wb_waitfree();
    }
#else
    check_blit_active(address, 4);
    ps_write_32_int(address, data);
#if CIA_DELAY
    if (address >= 0xbf0000 && address <= 0xbfffff) {
        ticksleep(CIA_DELAY);
    }
#endif
#if CHIPSET_DELAY
    if (address >= 0xa00000) {
        ticksleep(CHIPSET_DELAY);
    }
#endif
#endif
    cache_invalidate_range(ICACHE, address, 4);
}

void ps_write_64(unsigned int address, uint64_t data)
{
    ps_write_32(address, data >> 32);
    ps_write_32(address + 4, data & 0xffffffff);
    cache_invalidate_range(ICACHE, address, 8);
}

void ps_write_128(unsigned int address, uint128_t data)
{
    ps_write_32(address, data.hi >> 32);
    ps_write_32(address + 4, data.hi & 0xffffffff);
    ps_write_32(address + 8, data.lo >> 32);
    ps_write_32(address + 12, data.lo & 0xffffffff);
    cache_invalidate_range(ICACHE, address, 16);
}

unsigned int ps_read_8(unsigned int address)
{
    int val = ps_read_8_int(address);

#if CIA_DELAY
    if (address >= 0xbf0000 && address <= 0xbfffff) {
        ticksleep(CIA_DELAY);
    }
#endif
#if CHIPSET_DELAY
    if (address >= 0xa00000) {
        ticksleep(CHIPSET_DELAY);
    }
#endif
    return val;  
}

unsigned int ps_read_16(unsigned int address)
{
    int val = ps_read_16_int(address);
#if CIA_DELAY
    if (address >= 0xbf0000 && address <= 0xbfffff) {
        ticksleep(CIA_DELAY);
    }
#endif
#if CHIPSET_DELAY
    if (address >= 0xa00000) {
        ticksleep(CHIPSET_DELAY);
    }
#endif
    return val;
}

unsigned int ps_read_32(unsigned int address)
{
    int val = ps_read_32_int(address);
#if CIA_DELAY
    if (address >= 0xbf0000 && address <= 0xbfffff) {
        ticksleep(CIA_DELAY);
    }
#endif
#if CHIPSET_DELAY
    if (address >= 0xa00000) {
        ticksleep(CHIPSET_DELAY);
    }
#endif
    return val;
}

uint64_t ps_read_64(unsigned int address)
{
    uint32_t hi, lo;

    hi = ps_read_32(address);
    lo = ps_read_32(address + 4);

    return ((uint64_t)hi << 32) | lo;
}

uint128_t ps_read_128(unsigned int address)
{
    uint128_t res;
    uint32_t hi, lo;

    hi = ps_read_32(address);
    lo = ps_read_32(address + 4);

    res.hi = ((uint64_t)hi << 32) | lo;

    hi = ps_read_32(address + 8);
    lo = ps_read_32(address + 12);

    res.lo = ((uint64_t)hi << 32) | lo;

    return res;
}

void put_char(uint8_t c);
void putByte(void *io_base, char chr);

static void __putc(void *data, char c)
{
    (void)data;
    putByte(data, c);
#ifndef MAC68K
    put_char(c);
#endif
}

static uint32_t _seed;
uint32_t rnd() {
    _seed = (_seed * 1103515245) + 12345;
    return _seed;
}

/* RAM walk test — exercises the physical bus before the emulator runs */

void ps_ramtest(void)
{
    uint32_t errors = 0;
    uint32_t tests = 0;
    uint32_t base = 0x000100;  /* avoid vector table at 0x0 */
    uint32_t e0;

    kprintf_pc(__putc, NULL, "[RAMTEST] Walking-bit RAM test\n");

    /* Deassert ROM overlay — on the Mac SE the BBU clears OVL on the
       first bus access at or above 0x400000 (the permanent ROM region) */
    (void)ps_read_16(0x400000);

    /* ---- Test 1: Walking 1s, byte ---- */
    e0 = errors;
    for (int bit = 0; bit < 8; bit++) {
        uint8_t pat = 1 << bit;
        ps_write_8(base, pat);
        uint8_t got = ps_read_8(base);
        tests++;
        if (got != pat) errors++;
    }
    kprintf_pc(__putc, NULL, "  Walk-1 byte:  %s (%d/%d)\n",
        errors == e0 ? "OK" : "FAIL", errors - e0, 8);

    /* ---- Test 2: Walking 0s, byte ---- */
    e0 = errors;
    for (int bit = 0; bit < 8; bit++) {
        uint8_t pat = ~(1 << bit);
        ps_write_8(base, pat);
        uint8_t got = ps_read_8(base);
        tests++;
        if (got != pat) errors++;
    }
    kprintf_pc(__putc, NULL, "  Walk-0 byte:  %s (%d/%d)\n",
        errors == e0 ? "OK" : "FAIL", errors - e0, 8);

    /* ---- Test 3: Walking 1s, word ---- */
    e0 = errors;
    for (int bit = 0; bit < 16; bit++) {
        uint16_t pat = 1 << bit;
        ps_write_16(base, pat);
        uint16_t got = ps_read_16(base);
        tests++;
        if (got != pat) errors++;
    }
    kprintf_pc(__putc, NULL, "  Walk-1 word:  %s (%d/%d)\n",
        errors == e0 ? "OK" : "FAIL", errors - e0, 16);

    /* ---- Test 4: Walking 0s, word ---- */
    e0 = errors;
    for (int bit = 0; bit < 16; bit++) {
        uint16_t pat = ~(1 << bit) & 0xffff;
        ps_write_16(base, pat);
        uint16_t got = ps_read_16(base);
        tests++;
        if (got != pat) errors++;
    }
    kprintf_pc(__putc, NULL, "  Walk-0 word:  %s (%d/%d)\n",
        errors == e0 ? "OK" : "FAIL", errors - e0, 16);

    /* ---- Test 5: All byte values 0x00-0xFF ---- */
    e0 = errors;
    for (int v = 0; v < 256; v++) {
        ps_write_8(base, v);
        uint8_t got = ps_read_8(base);
        tests++;
        if (got != (uint8_t)v) errors++;
    }
    kprintf_pc(__putc, NULL, "  All-val byte: %s (%d/%d)\n",
        errors == e0 ? "OK" : "FAIL", errors - e0, 256);

    /* ---- Test 6: Address bus (power-of-2 offsets) ---- */
    e0 = errors;
    for (int bit = 0; bit < 20; bit++) {
        uint32_t addr = base + (1 << bit);
        ps_write_8(addr, (uint8_t)(bit + 1));
    }
    for (int bit = 0; bit < 20; bit++) {
        uint32_t addr = base + (1 << bit);
        uint8_t got = ps_read_8(addr);
        tests++;
        if (got != (uint8_t)(bit + 1)) errors++;
    }
    kprintf_pc(__putc, NULL, "  Addr bus:     %s (%d/%d)\n",
        errors == e0 ? "OK" : "FAIL", errors - e0, 20);

    /* ---- Test 7: Block 256 bytes ---- */
    e0 = errors;
    for (int i = 0; i < 256; i++)
        ps_write_8(base + i, (uint8_t)(i ^ 0xA5));
    for (int i = 0; i < 256; i++) {
        uint8_t got = ps_read_8(base + i);
        tests++;
        if (got != (uint8_t)(i ^ 0xA5)) errors++;
    }
    kprintf_pc(__putc, NULL, "  Block 256B:   %s (%d/%d)\n",
        errors == e0 ? "OK" : "FAIL", errors - e0, 256);

    /* ---- Test 8: Block 256 words ---- */
    e0 = errors;
    for (int i = 0; i < 256; i++) {
        uint16_t val = (uint16_t)((i << 8) | (i ^ 0xFF));
        ps_write_16(base + i * 2, val);
    }
    for (int i = 0; i < 256; i++) {
        uint16_t expected = (uint16_t)((i << 8) | (i ^ 0xFF));
        uint16_t got = ps_read_16(base + i * 2);
        tests++;
        if (got != expected) errors++;
    }
    kprintf_pc(__putc, NULL, "  Block 256W:   %s (%d/%d)\n",
        errors == e0 ? "OK" : "FAIL", errors - e0, 256);

    kprintf_pc(__putc, NULL, "[RAMTEST] %d tests, %d errors\n", tests, errors);
    /* ---- Test 9: Read consistency — write once, read 10x ---- */
    {
        uint32_t read_errs = 0;
        uint32_t read_vary = 0;
        kprintf_pc(__putc, NULL, "  Read consist: ");
        for (int pat = 0; pat < 16; pat++) {
            uint8_t val = (uint8_t)(pat * 17); /* 0x00,0x11,0x22,...0xFF */
            ps_write_8(base, val);
            uint8_t reads[10];
            for (int r = 0; r < 10; r++)
                reads[r] = ps_read_8(base);
            int any_wrong = 0, all_same = 1;
            for (int r = 0; r < 10; r++) {
                if (reads[r] != val) any_wrong = 1;
                if (reads[r] != reads[0]) all_same = 0;
            }
            tests += 10;
            if (any_wrong) {
                read_errs++;
                if (!all_same) read_vary++;
            }
        }
        kprintf_pc(__putc, NULL, "%d/16 bad, %d vary\n", read_errs, read_vary);
    }

    /* ---- Test 10: Write consistency — write 10x, read once ---- */
    {
        uint32_t write_errs = 0;
        kprintf_pc(__putc, NULL, "  Write consist:");
        for (int pat = 0; pat < 16; pat++) {
            uint8_t val = (uint8_t)(pat * 17);
            for (int w = 0; w < 10; w++)
                ps_write_8(base, val);
            uint8_t got = ps_read_8(base);
            tests++;
            if (got != val) write_errs++;
        }
        kprintf_pc(__putc, NULL, " %d/16 bad\n", write_errs);
    }

    if (errors) {
        kprintf_pc(__putc, NULL, "*** BUS ERRORS - HALTED ***\n");
        while(1) asm volatile("wfe");
    }
}

/* BupTest by beeanyew, ported to Emu68 */

void ps_buptest(unsigned int test_size, unsigned int maxiter)
{
    // Initialize RNG
    uint64_t tmp;
    asm volatile("mrs %0, CNTPCT_EL0":"=r"(tmp));

    _seed = tmp;

    kprintf_pc(__putc, NULL, "BUPTest with size %dK requested through commandline\n", test_size);

    test_size *= 1024;
    uint32_t frac = test_size / 16;

    uint8_t *garbage = tlsf_malloc(tlsf, test_size);

    ps_write_8(0xbfe201, 0x0101);       //CIA OVL
    ps_write_8(0xbfe001, 0x0000);       //CIA OVL LOW

    for (unsigned int iter = 0; iter < maxiter; iter++) {
        kprintf_pc(__putc, NULL, "Iteration %d...\n", iter + 1);

        // Fill the garbage buffer and chip ram with random data
        kprintf_pc(__putc, NULL, "  Writing BYTE garbage data to Chip...            ");
        for (uint32_t i = 0; i < test_size; i++) {
            uint8_t val = 0;
            val = rnd();
            garbage[i] = val;
            ps_write_8(i, val);

            if ((i % (frac * 2)) == 0)
                kprintf_pc(__putc, NULL, "*");
        }

        for (uint32_t i = 0; i < test_size; i++) {
            uint32_t c = ps_read_8(i);
            if (c != garbage[i]) {
                kprintf_pc(__putc, NULL, "\n    READ8: Garbege data mismatch at $%.6X: %.2X should be %.2X.\n", i, c, garbage[i]);
                while(1);
            }

            if ((i % (frac * 4)) == 0)
                kprintf_pc(__putc, NULL, "*");
        }

        for (uint32_t i = 0; i < (test_size) - 2; i += 2) {
            uint32_t c = BE16(ps_read_16(i));
            if (c != *((uint16_t *)&garbage[i])) {
                kprintf_pc(__putc, NULL, "\n    READ16_EVEN: Garbege data mismatch at $%.6X: %.4X should be %.4X.\n", i, c, *((uint16_t *)&garbage[i]));
                while(1);
            }

            if ((i % (frac * 4)) == 0)
                kprintf_pc(__putc, NULL, "*");
        }

        for (uint32_t i = 1; i < (test_size) - 2; i += 2) {
            uint32_t c = BE16((ps_read_8(i) << 8) | ps_read_8(i + 1));
            if (c != *((uint16_t *)&garbage[i])) {
                kprintf_pc(__putc, NULL, "\n    READ16_ODD: Garbege data mismatch at $%.6X: %.4X should be %.4X.\n", i, c, *((uint16_t *)&garbage[i]));
                while(1);
            }

            if ((i % (frac * 4)) == 1)
                kprintf_pc(__putc, NULL, "*");
        }
        
        for (uint32_t i = 0; i < (test_size) - 4; i += 2) {
            uint32_t c = BE32(ps_read_32(i));
            if (c != *((uint32_t *)&garbage[i])) {
                kprintf_pc(__putc, NULL, "\n    READ32_EVEN: Garbege data mismatch at $%.6X: %.8X should be %.8X.\n", i, c, *((uint32_t *)&garbage[i]));
                while(1);
            }
            
            if ((i % (frac * 4)) == 0)
                kprintf_pc(__putc, NULL, "*");
        }

        for (uint32_t i = 1; i < (test_size) - 4; i += 2) {
            uint32_t c = ps_read_8(i) << 24;
            c |= (BE16(ps_read_16(i + 1)) << 8);
            c |= ps_read_8(i + 3);
            if (c != *((uint32_t *)&garbage[i])) {
                kprintf_pc(__putc, NULL, "\n    READ32_ODD: Garbege data mismatch at $%.6X: %.8X should be %.8X.\n", i, c, *((uint32_t *)&garbage[i]));
                while(1);
            }

            if ((i % (frac * 4)) == 1)
                kprintf_pc(__putc, NULL, "*");
        }

        for (uint32_t i = 0; i < test_size; i++) {
            ps_write_8(i, (uint32_t)0x0);

            if ((i % (frac * 8)) == 0)
                kprintf_pc(__putc, NULL, "*");
        }

        kprintf_pc(__putc, NULL, "\n  Writing WORD garbage data to Chip, unaligned... ");
        for (uint32_t i = 1; i < (test_size) - 2; i += 2) {
            uint16_t v = *((uint16_t *)&garbage[i]);
            ps_write_8(i + 1, (v & 0x00FF));
            ps_write_8(i, (v >> 8));

            if ((i % (frac * 2)) == 1)
                kprintf_pc(__putc, NULL, "*");
        }

        for (uint32_t i = 1; i < (test_size) - 2; i += 2) {
            uint32_t c = BE16((ps_read_8(i) << 8) | ps_read_8(i + 1));
            if (c != *((uint16_t *)&garbage[i])) {
                kprintf_pc(__putc, NULL, "\n    READ16_ODD: Garbege data mismatch at $%.6X: %.4X should be %.4X.\n", i, c, *((uint16_t *)&garbage[i]));
                while(1);
            }

            if ((i % (frac * 2)) == 1)
                kprintf_pc(__putc, NULL, "*");
        }

        for (uint32_t i = 0; i < test_size; i++) {
            ps_write_8(i, (uint32_t)0x0);
        }

        kprintf_pc(__putc, NULL, "\n  Writing LONG garbage data to Chip, unaligned... ");
        for (uint32_t i = 1; i < (test_size) - 4; i += 4) {
            uint32_t v = *((uint32_t *)&garbage[i]);
            ps_write_8(i , v & 0x0000FF);
            ps_write_16(i + 1, BE16(((v & 0x00FFFF00) >> 8)));
            ps_write_8(i + 3 , (v & 0xFF000000) >> 24);

            if ((i % (frac * 2)) == 1)
                kprintf_pc(__putc, NULL, "*");
        }

        for (uint32_t i = 1; i < (test_size) - 4; i += 4) {
            uint32_t c = ps_read_8(i);
            c |= (BE16(ps_read_16(i + 1)) << 8);
            c |= (ps_read_8(i + 3) << 24);
            if (c != *((uint32_t *)&garbage[i])) {
                kprintf_pc(__putc, NULL, "\n    READ32_ODD: Garbege data mismatch at $%.6X: %.8X should be %.8X.\n", i, c, *((uint32_t *)&garbage[i]));
                while(1);
            }

            if ((i % (frac * 2)) == 1)
                kprintf_pc(__putc, NULL, "*");
        }

        kprintf_pc(__putc, NULL, "\n");
    }


    kprintf_pc(__putc, NULL, "All done. BUPTest completed.\n");

    ps_pulse_reset();

    tlsf_free(tlsf, garbage);
}
