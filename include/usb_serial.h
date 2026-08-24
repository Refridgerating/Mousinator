#ifndef SENTRY_USB_SERIAL_H
#define SENTRY_USB_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void usb_serial_init(void);
void usb_serial_poll(void);
bool usb_serial_read_byte(uint8_t *byte);
bool usb_serial_write(const uint8_t *data, size_t length);
size_t usb_serial_tx_free(void);

#endif

