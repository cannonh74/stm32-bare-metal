
#include <stdint.h>

// ---------------------------------------------------------------------
// Register access
// ---------------------------------------------------------------------

#define RCC_BASE  0x40021000UL
#define AFIO_BASE 0x40010000UL
#define GPIOA_BASE 0x40010800UL
#define GPIOB_BASE 0x40010C00UL

#define RCC_APB2ENR (*(volatile uint32_t *)(RCC_BASE + 0x18))
#define AFIO_MAPR   (*(volatile uint32_t *)(AFIO_BASE + 0x04))

#define RCC_APB2ENR_AFIOEN (1U << 0)
#define RCC_APB2ENR_IOPAEN (1U << 2)
#define RCC_APB2ENR_IOPBEN (1U << 3)

// GPIOx register offsets
#define GPIO_CRL  0x00U
#define GPIO_CRH  0x04U
#define GPIO_IDR  0x08U
#define GPIO_ODR  0x0CU
#define GPIO_BSRR 0x10U

typedef struct {
    uint32_t base;
    uint8_t  pin;
} gpio_pin_t;

static inline volatile uint32_t *gpio_reg(uint32_t base, uint32_t offset) {
    return (volatile uint32_t *)(base + offset);
}

// mode/cnf per the CRL/CRH 4-bit-per-pin layout (see RM0008 GPIO chapter)
static void gpio_config(gpio_pin_t p, uint32_t mode, uint32_t cnf) {
    uint32_t offset = (p.pin < 8) ? GPIO_CRL : GPIO_CRH;
    uint32_t shift = (uint32_t)((p.pin % 8) * 4);
    volatile uint32_t *cr = gpio_reg(p.base, offset);
    uint32_t val = *cr;
    val &= ~(0xFU << shift);
    val |= ((cnf << 2) | mode) << shift;
    *cr = val;
}

static void gpio_write(gpio_pin_t p, int state) {
    volatile uint32_t *bsrr = gpio_reg(p.base, GPIO_BSRR);
    *bsrr = (uint32_t)(state ? (1U << p.pin) : (1U << (p.pin + 16)));
}

static int gpio_read(gpio_pin_t p) {
    volatile uint32_t *idr = gpio_reg(p.base, GPIO_IDR);
    return (int)((*idr >> p.pin) & 1U);
}

// ---------------------------------------------------------------------
// Board wiring: cell index 0..8, left-to-right/top-to-bottom
//   0 1 2
//   3 4 5
//   6 7 8
// ---------------------------------------------------------------------

static const gpio_pin_t CELL_IN[9] = {
    {GPIOA_BASE, 9},  {GPIOB_BASE, 7},  {GPIOB_BASE, 4},
    {GPIOA_BASE, 10}, {GPIOA_BASE, 15}, {GPIOB_BASE, 5},
    {GPIOA_BASE, 11}, {GPIOB_BASE, 3},  {GPIOB_BASE, 6},
};
// Button 2's input was PA12 - the Blue Pill has a fixed hardware pull-up
// on PA12 (USB D+ detection) that overpowers the internal pull-down and
// reads permanently high. Moved to PB7 instead; rewire button 2's input
// lead from PA12 to PB7 to match.

static const gpio_pin_t CELL_OUT[9] = {
    {GPIOB_BASE, 10}, {GPIOA_BASE, 7}, {GPIOA_BASE, 4},
    {GPIOB_BASE, 1},  {GPIOA_BASE, 6}, {GPIOA_BASE, 3},
    {GPIOB_BASE, 0},  {GPIOA_BASE, 5}, {GPIOA_BASE, 2},
};

static const uint8_t WIN_LINES[8][3] = {
    {0, 1, 2}, {3, 4, 5}, {6, 7, 8},  // rows
    {0, 3, 6}, {1, 4, 7}, {2, 5, 8},  // columns
    {0, 4, 8}, {2, 4, 6},             // diagonals
};

// ---------------------------------------------------------------------
// Timing (all arbitrary busy-wait units - no clock calibration; tune
// to taste if the blink rate/debounce feels off on real hardware)
// ---------------------------------------------------------------------

#define LOOP_DELAY_TICKS      3000U   /* pacing delay per main-loop pass */
#define DEBOUNCE_TICKS        15U     /* consecutive stable passes to accept a new button state */
#define BLINK_HALF_PERIOD     40U     /* main-loop passes per blink half-period */
#define WIN_FLASH_COUNT       6U      /* number of on/off flashes on win/draw */
#define WIN_FLASH_DELAY       200000U /* busy-wait length for each flash phase */

static void delay(volatile uint32_t count) {
    while (count--) (void)0;
}

// ---------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------

#define EMPTY    0U
#define PLAYER_X 1U
#define PLAYER_O 2U

static uint8_t board[9];
static uint8_t current_player;
static uint8_t blink_phase;

static uint8_t debounce_candidate[9];
static uint8_t debounce_count[9];
static uint8_t stable_state[9];

static void gpio_setup(void) {
    RCC_APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN;

    // PA15/PB3/PB4 default to the JTAG function (JTDI/JTDO/NJTRST) out of
    // reset. Three of our buttons live on those pins, so disable JTAG-DP
    // and keep SW-DP only (SWJ_CFG = 010) - this frees the pins for GPIO
    // while leaving PA13/PA14 (SWDIO/SWCLK) intact for the OpenOCD/SWD
    // debugger.
    AFIO_MAPR = (AFIO_MAPR & ~(0x7UL << 24)) | (0x2UL << 24);

    for (uint8_t i = 0; i < 9; i++) {
        // Input, pull-up/pull-down mode (MODE=00, CNF=10), ODR=0 selects
        // pull-down: pin reads 0 idle, 1 when the button is pressed.
        gpio_config(CELL_IN[i], 0x0U, 0x2U);
        gpio_write(CELL_IN[i], 0);

        // Output push-pull, 2MHz (MODE=10, CNF=00). Assumes the LED
        // lights when the pin is driven high.
        gpio_config(CELL_OUT[i], 0x2U, 0x0U);
        gpio_write(CELL_OUT[i], 0);
    }
}

static void board_reset(void) {
    for (uint8_t i = 0; i < 9; i++) board[i] = EMPTY;
    current_player = PLAYER_X;
}

static int check_win_line(void) {
    for (uint8_t l = 0; l < 8; l++) {
        uint8_t a = board[WIN_LINES[l][0]];
        uint8_t b = board[WIN_LINES[l][1]];
        uint8_t c = board[WIN_LINES[l][2]];
        if (a != EMPTY && a == b && b == c) return l;
    }
    return -1;
}

static int board_full(void) {
    for (uint8_t i = 0; i < 9; i++)
        if (board[i] == EMPTY) return 0;
    return 1;
}

static void flash_cells(const uint8_t *cells, uint8_t n) {
    for (uint8_t t = 0; t < WIN_FLASH_COUNT; t++) {
        for (uint8_t i = 0; i < n; i++) gpio_write(CELL_OUT[cells[i]], 1);
        delay(WIN_FLASH_DELAY);
        for (uint8_t i = 0; i < n; i++) gpio_write(CELL_OUT[cells[i]], 0);
        delay(WIN_FLASH_DELAY);
    }
}

static void play_end_animation(int win_line) {
    // Blank every LED first - otherwise any non-winning cell still lit
    // (solid X, or an O caught mid-blink) stays on for the whole
    // animation instead of the board going dark immediately.
    for (uint8_t i = 0; i < 9; i++) gpio_write(CELL_OUT[i], 0);

    if (win_line >= 0) {
        flash_cells(WIN_LINES[win_line], 3);
    } else {
        uint8_t all[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
        flash_cells(all, 9);
    }
}

static void update_leds(void) {
    for (uint8_t i = 0; i < 9; i++) {
        if (board[i] == EMPTY) {
            gpio_write(CELL_OUT[i], 0);
        } else if (board[i] == PLAYER_X) {
            gpio_write(CELL_OUT[i], 1);          // solid = X
        } else {
            gpio_write(CELL_OUT[i], (int)blink_phase);  // blink = O
        }
    }
}

int main(void) {
    gpio_setup();
    board_reset();

    for (uint8_t i = 0; i < 9; i++) {
        debounce_candidate[i] = 0;
        debounce_count[i] = 0;
        stable_state[i] = 0;
    }

    uint32_t blink_tick = 0;

    while (1) {
        for (uint8_t i = 0; i < 9; i++) {
            int raw = gpio_read(CELL_IN[i]);
            uint8_t r = (uint8_t)raw;

            if (r != debounce_candidate[i]) {
                debounce_candidate[i] = r;
                debounce_count[i] = 0;
            } else if (debounce_count[i] < DEBOUNCE_TICKS) {
                debounce_count[i]++;
            }

            if (debounce_count[i] == DEBOUNCE_TICKS && stable_state[i] != debounce_candidate[i]) {
                uint8_t prev = stable_state[i];
                stable_state[i] = debounce_candidate[i];

                if (prev == 0 && stable_state[i] == 1 && board[i] == EMPTY) {
                    board[i] = current_player;

                    int win_line = check_win_line();
                    if (win_line >= 0 || board_full()) {
                        update_leds();
                        play_end_animation(win_line);
                        board_reset();
                    } else {
                        current_player = (current_player == PLAYER_X) ? PLAYER_O : PLAYER_X;
                    }
                }
            }
        }

        update_leds();

        if (++blink_tick >= BLINK_HALF_PERIOD) {
            blink_tick = 0;
            blink_phase ^= 1U;
        }

        delay(LOOP_DELAY_TICKS);
    }
}

// ---------------------------------------------------------------------
// Startup code (matches the other bare-metal projects in this repo)
// ---------------------------------------------------------------------

__attribute__((naked, noreturn)) void _reset(void) {
    extern long _sbss, _ebss, _sdata, _edata, _sidata;

    for (long *dst = &_sbss; dst < &_ebss; dst++) *dst = 0;

    for (long *dst = &_sdata, *src = &_sidata; dst < &_edata;) *dst++ = *src++;

    main();

    for (;;) (void)0;  // Infinite loop - should never be reached
}

extern void _estack(void);  // defined in stm32f103c8t6.ld

__attribute__((section(".vectors"))) void (*const tab[])(void) = {
    _estack,
    _reset,
};
