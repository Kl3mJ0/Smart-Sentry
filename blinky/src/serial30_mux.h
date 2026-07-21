#ifndef SERIAL30_MUX_H_
#define SERIAL30_MUX_H_

#include <stddef.h>
#include <stdint.h>

int serial30_uart_init(void);
int serial30_uart_putc(int c);
int serial30_twim_init(void);
int serial30_twim_write(uint16_t address, const uint8_t *data, size_t length);
int serial30_twim_read(uint16_t address, uint8_t *data, size_t length);

#endif
