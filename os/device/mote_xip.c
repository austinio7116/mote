/*
 * Mote OS device — fast-XIP restore + ATRANS save/restore.
 *
 * mote_xip_fast_setup is ported verbatim (renamed) from ThumbyOne's
 * thumbyone_xip_fast_setup — the W25Q reset-first sequence is load-bearing
 * (resets flash out of continuous-read mode before reconfiguring XIP) and the
 * function must run from RAM since it reconfigures the QMI the CPU fetches
 * through.
 */
#include "mote_xip.h"

#include "pico/platform/sections.h"   /* __not_in_flash_func */
#include "hardware/structs/qmi.h"
#include "hardware/regs/qmi.h"
#include "hardware/regs/addressmap.h"

void mote_xip_save_atrans(uint32_t saved[4]) {
    for (int i = 0; i < 4; i++) saved[i] = qmi_hw->atrans[i];
}

void mote_xip_restore_atrans(const uint32_t saved[4]) {
    for (int i = 0; i < 4; i++) qmi_hw->atrans[i] = saved[i];
    __asm__ volatile("dsb" ::: "memory");
}

/* --- fast QPI XIP config (W25Q080/16JV/AT25SF081, matches boot2) --- */
#define MX_CLKDIV   2
#define MX_RXDELAY  2
#define MX_RCMD_EB  0xEB
#define MX_MODE_A0  0xA0
#define MX_WAIT     4

#define MX_M0_TIMING ( \
    (1u        << QMI_M0_TIMING_COOLDOWN_LSB) | \
    (MX_RXDELAY << QMI_M0_TIMING_RXDELAY_LSB)  | \
    (MX_CLKDIV  << QMI_M0_TIMING_CLKDIV_LSB))

#define MX_M0_RCMD ( \
    (MX_RCMD_EB << QMI_M0_RCMD_PREFIX_LSB) | \
    (MX_MODE_A0 << QMI_M0_RCMD_SUFFIX_LSB))

#define MX_M0_RFMT ( \
    (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_S << QMI_M0_RFMT_PREFIX_WIDTH_LSB) | \
    (QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB)   | \
    (QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB) | \
    (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB)  | \
    (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB)   | \
    (QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB)   | \
    (QMI_M0_RFMT_SUFFIX_LEN_VALUE_8   << QMI_M0_RFMT_SUFFIX_LEN_LSB)   | \
    (MX_WAIT                          << QMI_M0_RFMT_DUMMY_LEN_LSB))

void __not_in_flash_func(mote_xip_fast_setup)(void) __attribute__((noinline));

void __not_in_flash_func(mote_xip_fast_setup)(void) {
    /* Step 1: reset the flash out of whatever state it's in (66h/99h). */
    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB)
                       | QMI_DIRECT_CSR_EN_BITS
                       | QMI_DIRECT_CSR_AUTO_CS0N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) tight_loop_contents();
    qmi_hw->direct_tx = 0x66u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) tight_loop_contents();
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_tx = 0x99u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) tight_loop_contents();
    (void)qmi_hw->direct_rx;
    for (volatile int i = 0; i < 3000; ++i) __asm__ volatile("nop");  /* ~tRST */
    qmi_hw->direct_csr = 0;

    /* Step 2: fast QPI XIP config on the now-clean flash. */
    qmi_hw->m[0].timing = MX_M0_TIMING;
    qmi_hw->m[0].rcmd   = MX_M0_RCMD;
    qmi_hw->m[0].rfmt   = MX_M0_RFMT;
    volatile uint32_t dummy = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    (void)dummy;
    qmi_hw->m[0].rfmt = MX_M0_RFMT & ~QMI_M0_RFMT_PREFIX_LEN_BITS;
    __asm__ volatile("dsb" ::: "memory");
}
