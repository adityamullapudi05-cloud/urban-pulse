/**
 * ============================================================================
 * hal_gpio.c  —  GPIO Hardware Abstraction Layer
 * Smart City RTOS Fault & Performance Monitoring Platform
 *
 * APPLIES TO: Both Raspberry Pi 1 (Workload) and Raspberry Pi 2 (Supervisor)
 *
 * BEHAVIOR — Level-Based (not edge-based):
 *   PIN LOW  (touching GND)  → Fault ACTIVE   (inject & hold)
 *   PIN HIGH (released)      → Fault CLEARED  (auto-recover to NORMAL)
 *
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                  RASPBERRY PI 1 — WORKLOAD NODE                │
 * ├────────────┬──────────┬─────────────────────────────────────────┤
 * │  RPi Pin   │  GPIO    │  Touch to GND → / Release →             │
 * ├────────────┼──────────┼─────────────────────────────────────────┤
 * │  Pin 16    │  GPIO 23 │  Service B STARVATION / NORMAL          │
 * │  Pin 18    │  GPIO 24 │  CPU OVERLOAD active / NORMAL           │
 * │  Pin 22    │  GPIO 25 │  Service A DEADLINE MISS / NORMAL       │
 * └────────────┴──────────┴─────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                  RASPBERRY PI 2 — SUPERVISOR NODE              │
 * ├────────────┬──────────┬─────────────────────────────────────────┤
 * │  RPi Pin   │  GPIO    │  Touch to GND → / Release →             │
 * ├────────────┼──────────┼─────────────────────────────────────────┤
 * │  Pin 16    │  GPIO 23 │  Simulate NODE DISCONNECTED / RECONNECT │
 * │  Pin 18    │  GPIO 24 │  Simulate IPC TIMEOUT / NORMAL          │
 * │  Pin 22    │  GPIO 25 │  Simulate CPU OVERLOAD / NORMAL         │
 * └────────────┴──────────┴─────────────────────────────────────────┘
 *
 * OUTPUT LEDS (same on both nodes):
 *   GPIO 17 (Pin 11) → Green  LED = HEALTHY
 *   GPIO 27 (Pin 13) → Yellow LED = WARNING
 *   GPIO 22 (Pin 15) → Red    LED = CRITICAL FAULT
 *
 * WIRING:
 *   - All button pins use internal PULL-UP → HIGH by default (NORMAL)
 *   - Touch pin to any GND pin (e.g. Pin 6, 9, 14, 20, 25, 30, 34, 39)
 *   - Pin goes LOW while contact is held → fault active during contact only
 *   - Release → pin goes HIGH → fault auto-clears → normal state
 * ============================================================================
 */

#include "smart_city_common.h"

/* ============================================================================
 * PIN DEFINITIONS (same for both nodes)
 * ============================================================================ */

/* OUTPUT — Health indicator LEDs */
#define GPIO_LED_GREEN      17    /* Pin 11 */
#define GPIO_LED_YELLOW     27    /* Pin 13 */
#define GPIO_LED_RED        22    /* Pin 15 */

/* INPUT — Physical fault buttons (active LOW via pull-up, touch to GND) */
#define GPIO_BTN_1          23    /* Pin 16 */
#define GPIO_BTN_2          24    /* Pin 18 */
#define GPIO_BTN_3          25    /* Pin 22 */

/* BCM2711 (RPi 4) GPIO peripheral base */
#define BCM2711_GPIO_BASE   0xFE200000UL
#define GPIO_REG_SIZE       0xB4

/* GPIO register offsets */
#define GPFSEL0     0x00
#define GPFSEL1     0x04
#define GPFSEL2     0x08
#define GPSET0      0x1C
#define GPCLR0      0x28
#define GPLEV0      0x34
#define GPPUD       0x94
#define GPPUDCLK0   0x98

/* ============================================================================
 * BUTTON CALLBACK TYPE
 * btn_id   : 0, 1, or 2 (which button)
 * pressed  : true  = GND contact active  (fault ON)
 *            false = GND released         (fault OFF / normal)
 * ============================================================================ */
typedef void (*gpio_callback_t)(int btn_id, bool pressed);

/* ============================================================================
 * GPIO CONTEXT
 * ============================================================================ */
typedef struct {
    bool initialized;
    int  prev_level[3];    /* Previous pin level — for edge detection */
#if defined(__QNX__) || defined(__QNXNTO__)
    uintptr_t gpio_base;
#endif
} GpioContext;

static GpioContext gpio_ctx;

/* ============================================================================
 * QNX REAL HARDWARE IMPLEMENTATION
 * ============================================================================ */
#if defined(__QNX__) || defined(__QNXNTO__)
#include <hw/inout.h>
#include <sys/mman.h>

static inline void gpio_write(uint32_t offset, uint32_t val) {
    out32(gpio_ctx.gpio_base + offset, val);
}
static inline uint32_t gpio_read(uint32_t offset) {
    return in32(gpio_ctx.gpio_base + offset);
}

/* Set pin as OUTPUT (001 in 3-bit GPFSEL field) */
static void gpio_set_output(int pin) {
    uint32_t offset = GPFSEL0 + ((pin / 10) * 4);
    int bit = (pin % 10) * 3;
    uint32_t val = gpio_read(offset);
    val &= ~(7U << bit);
    val |=  (1U << bit);
    gpio_write(offset, val);
}

/* Set pin as INPUT (000) with pull-up so it reads HIGH when floating */
static void gpio_set_input_pullup(int pin) {
    uint32_t offset = GPFSEL0 + ((pin / 10) * 4);
    int bit = (pin % 10) * 3;
    uint32_t val = gpio_read(offset);
    val &= ~(7U << bit);   /* 000 = input */
    gpio_write(offset, val);

    /* Enable pull-up via GPPUD sequence */
    gpio_write(GPPUD, 2);          /* 2 = pull-up */
    sleep_ms(1);
    gpio_write(GPPUDCLK0, 1U << pin);
    sleep_ms(1);
    gpio_write(GPPUD, 0);
    gpio_write(GPPUDCLK0, 0);
}

static void gpio_pin_set(int pin) { gpio_write(GPSET0, 1U << pin); }
static void gpio_pin_clr(int pin) { gpio_write(GPCLR0, 1U << pin); }
static int  gpio_pin_read(int pin) { return (gpio_read(GPLEV0) >> pin) & 1; }

/* ---- Public API ---- */

int hal_gpio_init(void) {
    if (ThreadCtl(_NTO_TCTL_IO, 0) == -1) {
        perror("[GPIO] ThreadCtl IO failed"); return -1;
    }
    gpio_ctx.gpio_base = mmap_device_io(GPIO_REG_SIZE, BCM2711_GPIO_BASE);
    if (gpio_ctx.gpio_base == MAP_DEVICE_FAILED) {
        perror("[GPIO] mmap_device_io failed"); return -1;
    }

    /* Configure LED output pins */
    gpio_set_output(GPIO_LED_GREEN);
    gpio_set_output(GPIO_LED_YELLOW);
    gpio_set_output(GPIO_LED_RED);
    gpio_pin_clr(GPIO_LED_GREEN);
    gpio_pin_clr(GPIO_LED_YELLOW);
    gpio_pin_clr(GPIO_LED_RED);

    /* Configure button input pins with pull-up */
    gpio_set_input_pullup(GPIO_BTN_1);
    gpio_set_input_pullup(GPIO_BTN_2);
    gpio_set_input_pullup(GPIO_BTN_3);

    /* Initialize previous levels as HIGH (not pressed) */
    gpio_ctx.prev_level[0] = 1;
    gpio_ctx.prev_level[1] = 1;
    gpio_ctx.prev_level[2] = 1;

    gpio_ctx.initialized = true;
    printf("[GPIO] Hardware ready. LEDs: G=%d Y=%d R=%d | Buttons: %d %d %d\n",
           GPIO_LED_GREEN, GPIO_LED_YELLOW, GPIO_LED_RED,
           GPIO_BTN_1, GPIO_BTN_2, GPIO_BTN_3);
    return 0;
}

void hal_gpio_set_led(HealthLedState state) {
    if (!gpio_ctx.initialized) return;
    gpio_pin_clr(GPIO_LED_GREEN);
    gpio_pin_clr(GPIO_LED_YELLOW);
    gpio_pin_clr(GPIO_LED_RED);
    switch (state) {
        case LED_GREEN:  gpio_pin_set(GPIO_LED_GREEN);  break;
        case LED_YELLOW: gpio_pin_set(GPIO_LED_YELLOW); break;
        case LED_RED:    gpio_pin_set(GPIO_LED_RED);    break;
        default: break;
    }
}

/**
 * hal_gpio_poll_thread()
 *
 * Runs continuously at highest priority (PRIORITY_FAULT_DETECTOR = 25).
 * Polls 3 input button pins every 20ms.
 *
 * LEVEL-BASED BEHAVIOR:
 *   Any time a pin CHANGES level, callback is fired with the new state:
 *     pressed=true  → pin went HIGH→LOW  (user touched GND) → inject fault
 *     pressed=false → pin went LOW→HIGH  (user released GND) → clear fault
 *
 * arg: pointer to a gpio_callback_t function in the application layer
 */
void* hal_gpio_poll_thread(void *arg) {
    if (!gpio_ctx.initialized) {
        printf("[GPIO] Hardware not initialized (run as root for GPIO access). Polling disabled.\n");
        return NULL;
    }
    gpio_callback_t cb = (gpio_callback_t)arg;
    int pins[3] = { GPIO_BTN_1, GPIO_BTN_2, GPIO_BTN_3 };

    printf("[GPIO] Level-based fault injection monitoring active.\n");
    printf("[GPIO]   Pin 16 (GPIO 23) → hold to GND = fault ON, release = NORMAL\n");
    printf("[GPIO]   Pin 18 (GPIO 24) → hold to GND = fault ON, release = NORMAL\n");
    printf("[GPIO]   Pin 22 (GPIO 25) → hold to GND = fault ON, release = NORMAL\n");

    while (1) {
        for (int i = 0; i < 3; i++) {
            int level = gpio_pin_read(pins[i]);

            if (level != gpio_ctx.prev_level[i]) {
                /* Level changed — fire callback with new pressed state */
                bool pressed = (level == 0);  /* LOW = pressed (GND contact) */
                printf("[GPIO] Pin GPIO %d → %s\n", pins[i],
                       pressed ? "GND TOUCHED  (Fault ACTIVE)" : "RELEASED     (Returning to NORMAL)");
                if (cb) cb(i, pressed);
                gpio_ctx.prev_level[i] = level;
            }
        }
        sleep_ms(20);  /* 20ms debounce polling interval */
    }
    return NULL;
}

void hal_gpio_deinit(void) {
    if (!gpio_ctx.initialized) return;
    gpio_pin_clr(GPIO_LED_GREEN);
    gpio_pin_clr(GPIO_LED_YELLOW);
    gpio_pin_clr(GPIO_LED_RED);
    munmap_device_io(gpio_ctx.gpio_base, GPIO_REG_SIZE);
    gpio_ctx.initialized = false;
    printf("[GPIO] Hardware released.\n");
}

/* ============================================================================
 * NON-QNX SIMULATION FALLBACK (Windows / Linux dev machine)
 * ============================================================================ */
#else

int hal_gpio_init(void) {
    gpio_ctx.prev_level[0] = 1;
    gpio_ctx.prev_level[1] = 1;
    gpio_ctx.prev_level[2] = 1;
    gpio_ctx.initialized = true;
    printf("[GPIO] SIMULATION MODE — no QNX hardware. Use CLI inject commands.\n");
    return 0;
}

void hal_gpio_set_led(HealthLedState state) {
    /* LED state shown via hal_led_str() in console */
    (void)state;
}

void* hal_gpio_poll_thread(void *arg) {
    /* No physical GPIO on dev machine — thread exits immediately */
    (void)arg;
    return NULL;
}

void hal_gpio_deinit(void) {
    gpio_ctx.initialized = false;
}

#endif /* QNX */
