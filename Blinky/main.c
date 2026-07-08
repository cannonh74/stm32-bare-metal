
#include <stdint.h>

#define RCC_BASE   0x40021000UL
#define GPIOC_BASE 0x40011000UL

#define RCC_APB2ENR (*(volatile uint32_t *)(RCC_BASE + 0x18))
#define GPIOC_CRH   (*(volatile uint32_t *)(GPIOC_BASE + 0x04))
#define GPIOC_ODR   (*(volatile uint32_t *)(GPIOC_BASE + 0x0C))

#define RCC_APB2ENR_IOPCEN (1U << 4)
#define LED_PIN            13U

static void delay(volatile uint32_t count) {
    while (count--) (void)0;
}

int main(void) {
    RCC_APB2ENR |= RCC_APB2ENR_IOPCEN;  // enable GPIOC clock

    // PC13: general purpose push-pull output, max speed 2MHz
    GPIOC_CRH &= ~(0xFU << ((LED_PIN - 8) * 4));
    GPIOC_CRH |=  (0x2U << ((LED_PIN - 8) * 4));

    while (1) {
        GPIOC_ODR ^= (1U << LED_PIN);  // onboard LED is active low
        delay(300000);
    }
}

// Startup code
__attribute__((naked, noreturn)) void _reset(void) {
    extern long _sbss, _ebss, _sdata, _edata, _sidata;

    for (long* dst = &_sbss; dst < &_ebss; dst++) *dst = 0;

    for (long *dst = &_sdata, *src = &_sidata; dst < &_edata;) *dst++ = *src++;

    main();

    for (;;) (void)0;  // Infinite loop - should never be reached
}

extern void _estack(void);  //defined in stm32xx.ld

// Vector table (initial stack pointer + reset handler)
__attribute__((section(".vectors"))) void (*const tab[])(void) = {
    _estack,
    _reset,
};
