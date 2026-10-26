// SX1262 SPI/GPIO bridge firmware for the Waveshare USB-TO-LoRa (GD32F103C8 + SX1262).
// Flashed at 0x08004000 behind Archie3d's bootloader. See ../protocol.h for the wire format.
// Board facts (pins, TCXO power, RF switch polarity) are from Archie3d/waveshare-usb-lora-firmware.

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/spi.h>
#include <libopencm3/stm32/usart.h>
#include <stdbool.h>
#include <stdint.h>

#include "../protocol.h"

#ifndef WITH_TCXO
#define WITH_TCXO 1 // 0 for the -B (XTAL) variant
#endif

#define LED_PORT GPIOA
#define LED_RXD GPIO6 // active low
#define LED_TXD GPIO7 // active low
#define RESET_PORT GPIOA
#define RESET_PIN GPIO4
#define CS_PORT GPIOB
#define CS_PIN GPIO12
#define RFSW_PORT GPIOB
#define RFSW_PIN GPIO4
#define BUSY_PORT GPIOB
#define BUSY_PIN GPIO1
#define DIO1_PORT GPIOB
#define DIO1_PIN GPIO0

#define SX126X_OP_SET_TX 0x83

static volatile uint32_t ms;
static volatile bool irq_pending;

// UART receive ring, filled from the ISR
#define RXQ_SIZE 2048
static volatile uint8_t rxq[RXQ_SIZE];
static volatile uint16_t rxq_head;
static uint16_t rxq_tail;

static uint8_t buf[WSB_MAX_PAYLOAD];

// LED timing: TXD lit from SetTx until the next DIO1 (TxDone), RXD flashes on other DIO1s
static bool transmitting;
static uint32_t rxd_off_at;

void sys_tick_handler(void) { ms++; }

void usart1_isr(void)
{
    if (usart_get_flag(USART1, USART_SR_RXNE)) {
        uint8_t c = usart_recv(USART1);
        uint16_t next = (rxq_head + 1) % RXQ_SIZE;
        if (next != rxq_tail) {
            rxq[rxq_head] = c;
            rxq_head = next;
        }
    }
}

void exti0_isr(void)
{
    exti_reset_request(EXTI0);
    irq_pending = true;
}

static int rx_byte(void)
{
    if (rxq_tail == rxq_head)
        return -1;
    uint8_t c = rxq[rxq_tail];
    rxq_tail = (rxq_tail + 1) % RXQ_SIZE;
    return c;
}

static uint8_t rx_wait(void)
{
    int c;
    while ((c = rx_byte()) < 0)
        ;
    return (uint8_t)c;
}

static void tx_byte(uint8_t c) { usart_send_blocking(USART1, c); }

static void send_frame(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    tx_byte(WSB_SYNC);
    tx_byte(cmd);
    tx_byte(len & 0xFF);
    tx_byte(len >> 8);
    for (uint16_t i = 0; i < len; i++)
        tx_byte(data[i]);
}

static void clock_init(void)
{
    SCB_CCR &= ~SCB_CCR_UNALIGN_TRP;
#if WITH_TCXO
    // The TCXO feeding OSC_IN is powered from PD1 (OSC_OUT, free as GPIO in bypass mode)
    rcc_periph_clock_enable(RCC_GPIOD);
    gpio_set_mode(GPIOD, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO1);
    gpio_set(GPIOD, GPIO1);
    for (volatile int i = 0; i < 60000; i++)
        __asm__("nop");
    RCC_CR |= RCC_CR_HSEBYP;
#endif
    rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE8_72MHZ]);

    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB_DIV8);
    systick_set_reload(72000000 / 8 / 1000 - 1);
    systick_interrupt_enable();
    systick_counter_enable();
}

static void gpio_init(void)
{
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_AFIO);

    // PB4 is JTAG NJTRST by default; keep SWD, drop JTAG
    AFIO_MAPR = (AFIO_MAPR & ~AFIO_MAPR_SWJ_MASK) | AFIO_MAPR_SWJ_CFG_JTAG_OFF_SW_ON;

    gpio_set(LED_PORT, LED_RXD | LED_TXD);
    gpio_set_mode(LED_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, LED_RXD | LED_TXD);

    gpio_set(RESET_PORT, RESET_PIN);
    gpio_set_mode(RESET_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, RESET_PIN);

    gpio_set(CS_PORT, CS_PIN);
    gpio_set_mode(CS_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, CS_PIN);

    gpio_set(RFSW_PORT, RFSW_PIN); // RX path
    gpio_set_mode(RFSW_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, RFSW_PIN);

    // BUSY pulled down so a missing chip doesn't read as busy forever
    gpio_clear(BUSY_PORT, BUSY_PIN);
    gpio_set_mode(BUSY_PORT, GPIO_MODE_INPUT, GPIO_CNF_INPUT_PULL_UPDOWN, BUSY_PIN);
    gpio_set_mode(DIO1_PORT, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, DIO1_PIN);

    exti_select_source(EXTI0, GPIOB);
    exti_set_trigger(EXTI0, EXTI_TRIGGER_RISING);
    exti_enable_request(EXTI0);
    nvic_enable_irq(NVIC_EXTI0_IRQ);
}

static void spi_init(void)
{
    rcc_periph_clock_enable(RCC_SPI2);
    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO13 | GPIO15);
    gpio_set_mode(GPIOB, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO14);
    // APB1 36 MHz / 8 = 4.5 MHz, mode 0, 8-bit MSB first. CS is a plain GPIO, so SSM+SSI must go in
    // the same write as MSTR: spi_init_master() sets MSTR first and the GD32 mode-faults out of master.
    (void)SPI_SR(SPI2); // clear any MODF
    SPI_CR1(SPI2) = SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_MSTR | SPI_CR1_BAUDRATE_FPCLK_DIV_8;
    SPI_CR1(SPI2) |= SPI_CR1_SPE;
}

static void uart_init(void)
{
    rcc_periph_clock_enable(RCC_USART1);
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX);
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO_USART1_RX);
    usart_set_baudrate(USART1, WSB_BAUD);
    usart_set_databits(USART1, 8);
    usart_set_stopbits(USART1, USART_STOPBITS_1);
    usart_set_parity(USART1, USART_PARITY_NONE);
    usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);
    usart_set_mode(USART1, USART_MODE_TX_RX);
    usart_enable_rx_interrupt(USART1);
    nvic_enable_irq(NVIC_USART1_IRQ);
    usart_enable(USART1);
}

static void set_pin(uint8_t pin, bool level)
{
    switch (pin) {
    case WSB_PIN_CS:
        level ? gpio_set(CS_PORT, CS_PIN) : gpio_clear(CS_PORT, CS_PIN);
        break;
    case WSB_PIN_RXEN:
        level ? gpio_set(RFSW_PORT, RFSW_PIN) : gpio_clear(RFSW_PORT, RFSW_PIN);
        break;
    case WSB_PIN_RESET:
        level ? gpio_set(RESET_PORT, RESET_PIN) : gpio_clear(RESET_PORT, RESET_PIN);
        break;
    }
}

static uint8_t get_pin(uint8_t pin)
{
    switch (pin) {
    case WSB_PIN_BUSY:
        return gpio_get(BUSY_PORT, BUSY_PIN) ? 1 : 0;
    case WSB_PIN_IRQ:
        return gpio_get(DIO1_PORT, DIO1_PIN) ? 1 : 0;
    case WSB_PIN_CS:
        return gpio_get(CS_PORT, CS_PIN) ? 1 : 0;
    case WSB_PIN_RXEN:
        return gpio_get(RFSW_PORT, RFSW_PIN) ? 1 : 0;
    case WSB_PIN_RESET:
        return gpio_get(RESET_PORT, RESET_PIN) ? 1 : 0;
    }
    return 0;
}

static void set_mode(uint8_t pin, bool output)
{
    // Only NRESET is ever released to an input by RadioLib; the SX1262 pulls it up itself
    if (pin == WSB_PIN_RESET)
        gpio_set_mode(RESET_PORT, output ? GPIO_MODE_OUTPUT_2_MHZ : GPIO_MODE_INPUT,
                      output ? GPIO_CNF_OUTPUT_PUSHPULL : GPIO_CNF_INPUT_FLOAT, RESET_PIN);
}

static void spi_xfer_buf(uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
        data[i] = (uint8_t)spi_xfer(SPI2, data[i]);
    while (SPI_SR(SPI2) & SPI_SR_BSY)
        ;
}

static void handle(uint8_t cmd, uint16_t len)
{
    switch (cmd) {
    case WSB_CMD_HELLO: {
        const uint8_t hello[] = {'W', 'S', 'B', 'R', WSB_VERSION, WITH_TCXO};
        send_frame(WSB_REPLY(cmd), hello, sizeof(hello));
        break;
    }
    case WSB_CMD_XFER:
    case WSB_CMD_XFER_CS:
        if (len && buf[0] == SX126X_OP_SET_TX) {
            transmitting = true;
            gpio_clear(LED_PORT, LED_TXD);
        }
        if (cmd == WSB_CMD_XFER_CS)
            gpio_clear(CS_PORT, CS_PIN);
        spi_xfer_buf(buf, len);
        if (cmd == WSB_CMD_XFER_CS)
            gpio_set(CS_PORT, CS_PIN);
        send_frame(WSB_REPLY(cmd), buf, len);
        break;
    case WSB_CMD_WRITE:
        if (len >= 2)
            set_pin(buf[0], buf[1]);
        break;
    case WSB_CMD_READ: {
        uint8_t v = len >= 1 ? get_pin(buf[0]) : 0;
        send_frame(WSB_REPLY(cmd), &v, 1);
        break;
    }
    case WSB_CMD_MODE:
        if (len >= 2)
            set_mode(buf[0], buf[1]);
        break;
    }
}

static void service_irq(void)
{
    if (!irq_pending)
        return;
    irq_pending = false;
    send_frame(WSB_EVT_IRQ, 0, 0);
    if (transmitting) {
        transmitting = false; // TxDone or timeout
        gpio_set(LED_PORT, LED_TXD);
    } else {
        gpio_clear(LED_PORT, LED_RXD);
        rxd_off_at = ms + 80;
    }
}

int main(void)
{
    clock_init();
    gpio_init();
    spi_init();
    uart_init();
    // The bootloader jumps here with interrupts masked (cpsid i)
    __asm__ volatile("cpsie i");

    // Both LEDs blink once: bridge firmware is up
    gpio_clear(LED_PORT, LED_RXD | LED_TXD);
    uint32_t t = ms + 300;
    while ((int32_t)(ms - t) < 0)
        ;
    gpio_set(LED_PORT, LED_RXD | LED_TXD);

    for (;;) {
        service_irq();
        if (rxd_off_at && (int32_t)(ms - rxd_off_at) >= 0) {
            rxd_off_at = 0;
            gpio_set(LED_PORT, LED_RXD);
        }

        int c = rx_byte();
        if (c != WSB_SYNC)
            continue; // idle, or resyncing after garbage
        uint8_t cmd = rx_wait();
        uint16_t len = rx_wait();
        len |= (uint16_t)rx_wait() << 8;
        if (len > WSB_MAX_PAYLOAD)
            continue;
        for (uint16_t i = 0; i < len; i++)
            buf[i] = rx_wait();
        handle(cmd, len);
    }
}
