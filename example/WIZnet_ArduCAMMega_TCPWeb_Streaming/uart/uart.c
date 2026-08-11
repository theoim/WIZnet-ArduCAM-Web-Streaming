#include "uart.h"
#include "pico/stdlib.h"

#define UART_ID uart0
#define BAUD_RATE 921600

// We are using pins 0 and 1, but see the GPIO function select table in the
// datasheet for information on which other pins can be used.
#define UART_TX_PIN 0
#define UART_RX_PIN 1


void serial_init()
{
    // Set up our UART with the required speed.
    uart_init(UART_ID, BAUD_RATE);

    // Set the TX and RX pins by using the function select on the GPIO
    // Set datasheet for more information on function select
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
}

void serial_write_blocking(uint8_t *src, size_t len)
{
    uart_write_blocking(UART_ID, src, len);
}

char serial_getc()
{
   return uart_getc(UART_ID);
}