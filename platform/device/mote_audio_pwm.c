/*
 * Mote — RP2350 PWM audio driver (Thumby Color). 22050 Hz, 12-bit PWM DAC on
 * GP23 with the amp enable on GP20; a sample-rate IRQ (timer slice 4) pulls one
 * sample per fire from a ring buffer that the frame loop refills. Ported from
 * the ThumbyCraft/Elite craft_audio_pwm driver (triangular dither kept — it
 * removes the shimmer when quantising 16-bit to 12-bit on sustained tones).
 */
#include "mote_audio_pwm.h"

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

#define AUDIO_PWM_PIN     23
#define AUDIO_ENABLE_PIN  20
#define TIMER_SLICE        4
#define PWM_WRAP        4096      /* 12-bit DAC */
#define RING_SIZE 2048
#define RING_MASK (RING_SIZE - 1)

static volatile int16_t  ring[RING_SIZE];
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;

static void __isr __not_in_flash_func(mote_audio_irq)(void){
    pwm_clear_irq(TIMER_SLICE);
    int16_t s = 0;
    uint32_t t = ring_tail;
    if (t != ring_head) { s = ring[t & RING_MASK]; ring_tail = t + 1; }
    static uint32_t dr = 1;
    dr = dr * 1103515245u + 12345u; int d1 = (int)(dr >> 20) & 0x1F;
    dr = dr * 1103515245u + 12345u; int d2 = (int)(dr >> 20) & 0x1F;
    int v = ((int)s + 32768 + (d1 + d2 - 31)) >> 4;
    if (v < 0) v = 0;
    if (v >= PWM_WRAP) v = PWM_WRAP - 1;
    pwm_set_gpio_level(AUDIO_PWM_PIN, (uint32_t)v);
}

/* Idempotent: safe to call once at boot AND again at every game launch to
 * re-establish the timer/IRQ/amp + flush the ring. (Audio went silent after a
 * game switch with only a full reboot fixing it — re-arming per game cures it
 * whatever leaves the slice/IRQ in a bad state.) The IRQ handler is registered
 * once; everything else is reconfigured each call. */
void mote_audio_pwm_init(void){
    static int handler_set;
    ring_head = ring_tail = 0;                        /* flush any stale/frozen ring */

    gpio_init(AUDIO_ENABLE_PIN); gpio_set_dir(AUDIO_ENABLE_PIN, GPIO_OUT); gpio_put(AUDIO_ENABLE_PIN, 0);
    gpio_set_function(AUDIO_PWM_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(AUDIO_PWM_PIN);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&c, 1); pwm_config_set_wrap(&c, PWM_WRAP);
    pwm_init(slice, &c, true);
    pwm_set_gpio_level(AUDIO_PWM_PIN, PWM_WRAP / 2);

    pwm_clear_irq(TIMER_SLICE);
    pwm_set_irq_enabled(TIMER_SLICE, true);
    if (!handler_set) {
        irq_set_exclusive_handler(PWM_IRQ_WRAP, mote_audio_irq);
        irq_set_priority(PWM_IRQ_WRAP, PICO_LOWEST_IRQ_PRIORITY);
        handler_set = 1;
    }
    irq_set_enabled(PWM_IRQ_WRAP, true);

    pwm_config tc = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&tc, 1);
    pwm_config_set_wrap(&tc, (uint16_t)((clock_get_hz(clk_sys) / 22050) - 1));
    pwm_init(TIMER_SLICE, &tc, true);

    gpio_put(AUDIO_ENABLE_PIN, 1);
}

void mote_audio_pwm_push(const int16_t *s, int n){
    for (int i = 0; i < n; i++){ uint32_t h = ring_head;
        if ((h - ring_tail) >= RING_SIZE) break;
        ring[h & RING_MASK] = s[i]; ring_head = h + 1; }
}
int mote_audio_pwm_room(void){ return RING_SIZE - (int)(ring_head - ring_tail); }
