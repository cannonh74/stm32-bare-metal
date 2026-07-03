
#include <stdint.h>


int main(void) {
    uint32_t cnt = 0, half;

    while (1) {
        cnt += 2;
        half = cnt / 2;
        ++half;
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