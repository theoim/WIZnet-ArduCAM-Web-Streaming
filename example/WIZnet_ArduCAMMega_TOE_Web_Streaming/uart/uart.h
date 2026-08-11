#ifndef _UART_H
#define _UART_H

#include "hardware/uart.h"

void serial_init();
char serial_getc();
void serial_write_blocking(uint8_t *src, size_t len);

#endif