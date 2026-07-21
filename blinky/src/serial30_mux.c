#include "serial30_mux.h"

#include <nrfx_twim.h>
#include <nrfx_uarte.h>
#include <hal/nrf_gpio.h>

static nrfx_twim_t twim30 = NRFX_TWIM_INSTANCE(30);
static nrfx_uarte_t uarte30 = NRFX_UARTE_INSTANCE(30);

int serial30_uart_init(void)
{
	nrfx_uarte_config_t config = NRFX_UARTE_DEFAULT_CONFIG(
		NRF_GPIO_PIN_MAP(0, 0), NRF_UARTE_PSEL_DISCONNECTED);

	return nrfx_uarte_init(&uarte30, &config, NULL);
}

int serial30_uart_putc(int c)
{
	uint8_t byte = (uint8_t)c;
	return nrfx_uarte_tx(&uarte30, &byte, 1, 0) == 0 ? c : -1;
}

int serial30_twim_init(void)
{
	nrfx_twim_config_t config = NRFX_TWIM_DEFAULT_CONFIG(
		NRF_GPIO_PIN_MAP(0, 3), NRF_GPIO_PIN_MAP(0, 4));
	int err = nrfx_twim_init(&twim30, &config, NULL, NULL);

	if (err == 0) {
		nrfx_twim_enable(&twim30);
	}
	return err;
}

int serial30_twim_write(uint16_t address, const uint8_t *data, size_t length)
{
	nrfx_twim_xfer_desc_t transfer = NRFX_TWIM_XFER_DESC_TX(
		address, (uint8_t *)data, length);
	return nrfx_twim_xfer(&twim30, &transfer, 0);
}

int serial30_twim_read(uint16_t address, uint8_t *data, size_t length)
{
	nrfx_twim_xfer_desc_t transfer = NRFX_TWIM_XFER_DESC_RX(
		address, data, length);
	return nrfx_twim_xfer(&twim30, &transfer, 0);
}
