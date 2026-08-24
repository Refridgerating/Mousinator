#include "board.h"
#include "driver_control.h"
#include "pan_controller.h"
#include "protocol.h"
#include "usb_serial.h"

#include <stddef.h>
#include <stdint.h>

static bool protocol_usb_write(const uint8_t *data, size_t length, void *context)
{
	(void)context;
	return usb_serial_write(data, length);
}

int main(void)
{
	protocol_t protocol;
	uint8_t received_byte;

	board_safe_init();
	board_clock_init();
	pan_controller_init();
	protocol_init(&protocol, protocol_usb_write, NULL);
	usb_serial_init();
	board_usb_connect(true);
	driver_control_init();

	for (;;) {
		usb_serial_poll();

		/*
		 * Preserve room for the largest possible response before consuming
		 * another byte that could terminate a command.
		 */
		while ((usb_serial_tx_free() >= PROTOCOL_MAX_RESPONSE_SIZE) &&
		       usb_serial_read_byte(&received_byte)) {
			protocol_receive(&protocol, &received_byte, 1U);
		}
	}
}
