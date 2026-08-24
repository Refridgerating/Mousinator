#include "usb_serial.h"

#include <libopencm3/usb/cdc.h>
#include <libopencm3/usb/usbd.h>

#define USB_BULK_PACKET_SIZE 64U
#define USB_RX_ENDPOINT 0x01U
#define USB_TX_ENDPOINT 0x82U
#define USB_NOTIFICATION_ENDPOINT 0x83U
#define USB_CDC_REQ_GET_LINE_CODING 0x21U
#define USB_RX_QUEUE_CAPACITY 256U
#define USB_TX_QUEUE_CAPACITY 512U

static const struct usb_device_descriptor device_descriptor = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = USB_CLASS_CDC,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = USB_BULK_PACKET_SIZE,
	.idVendor = 0x1209,
	.idProduct = 0x0001,
	.bcdDevice = 0x0100,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};

/* Linux cdc_acm expects the notification endpoint even though it is unused. */
static const struct usb_endpoint_descriptor communication_endpoints[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_NOTIFICATION_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
	.wMaxPacketSize = 16,
	.bInterval = 255,
}};

static const struct usb_endpoint_descriptor data_endpoints[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_RX_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = USB_BULK_PACKET_SIZE,
	.bInterval = 1,
}, {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_TX_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = USB_BULK_PACKET_SIZE,
	.bInterval = 1,
}};

static const struct {
	struct usb_cdc_header_descriptor header;
	struct usb_cdc_call_management_descriptor call_management;
	struct usb_cdc_acm_descriptor acm;
	struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) cdc_descriptors = {
	.header = {
		.bFunctionLength = sizeof(struct usb_cdc_header_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_HEADER,
		.bcdCDC = 0x0110,
	},
	.call_management = {
		.bFunctionLength = sizeof(struct usb_cdc_call_management_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
		.bmCapabilities = 0,
		.bDataInterface = 1,
	},
	.acm = {
		.bFunctionLength = sizeof(struct usb_cdc_acm_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_ACM,
		.bmCapabilities = 0x02,
	},
	.cdc_union = {
		.bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_UNION,
		.bControlInterface = 0,
		.bSubordinateInterface0 = 1,
	},
};

static const struct usb_interface_descriptor communication_interface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_CDC,
	.bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
	.bInterfaceProtocol = USB_CDC_PROTOCOL_AT,
	.iInterface = 0,
	.endpoint = communication_endpoints,
	.extra = &cdc_descriptors,
	.extralen = sizeof(cdc_descriptors),
}};

static const struct usb_interface_descriptor data_interface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 1,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_DATA,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = 0,
	.iInterface = 0,
	.endpoint = data_endpoints,
}};

static const struct usb_interface interfaces[] = {{
	.num_altsetting = 1,
	.altsetting = communication_interface,
}, {
	.num_altsetting = 1,
	.altsetting = data_interface,
}};

static const struct usb_config_descriptor configuration_descriptor = {
	.bLength = USB_DT_CONFIGURATION_SIZE,
	.bDescriptorType = USB_DT_CONFIGURATION,
	.wTotalLength = 0,
	.bNumInterfaces = 2,
	.bConfigurationValue = 1,
	.iConfiguration = 0,
	.bmAttributes = 0x80,
	.bMaxPower = 50,
	.interface = interfaces,
};

static const char *usb_strings[] = {
	"Sentry",
	"Sentry Motion Controller",
	"0001",
};

static struct usb_cdc_line_coding line_coding = {
	.dwDTERate = 115200,
	.bCharFormat = USB_CDC_1_STOP_BITS,
	.bParityType = USB_CDC_NO_PARITY,
	.bDataBits = 8,
};

static uint8_t control_buffer[128];
static uint8_t rx_queue[USB_RX_QUEUE_CAPACITY];
static uint8_t tx_queue[USB_TX_QUEUE_CAPACITY];
static size_t rx_head;
static size_t rx_tail;
static size_t rx_count;
static size_t tx_head;
static size_t tx_tail;
static size_t tx_count;
static bool rx_endpoint_naked;
static bool usb_configured;
static usbd_device *usb_device;

static void tx_kick(void);

static void rx_queue_push(uint8_t byte)
{
	rx_queue[rx_head] = byte;
	rx_head = (rx_head + 1U) % USB_RX_QUEUE_CAPACITY;
	++rx_count;
}

static void tx_queue_push(uint8_t byte)
{
	tx_queue[tx_head] = byte;
	tx_head = (tx_head + 1U) % USB_TX_QUEUE_CAPACITY;
	++tx_count;
}

static enum usbd_request_return_codes cdc_control_request(
	usbd_device *device, struct usb_setup_data *request, uint8_t **buffer,
	uint16_t *length,
	void (**complete)(usbd_device *device, struct usb_setup_data *request))
{
	(void)device;
	(void)complete;

	switch (request->bRequest) {
	case USB_CDC_REQ_SET_CONTROL_LINE_STATE:
		/* DTR and RTS are acknowledged but never gate command processing. */
		return USBD_REQ_HANDLED;

	case USB_CDC_REQ_SET_LINE_CODING:
		if (*length < sizeof(struct usb_cdc_line_coding)) {
			return USBD_REQ_NOTSUPP;
		}
		return USBD_REQ_HANDLED;

	case USB_CDC_REQ_GET_LINE_CODING:
		*buffer = (uint8_t *)&line_coding;
		*length = sizeof(line_coding);
		return USBD_REQ_HANDLED;

	default:
		return USBD_REQ_NOTSUPP;
	}
}

static void data_rx_callback(usbd_device *device, uint8_t endpoint)
{
	uint8_t packet[USB_BULK_PACKET_SIZE];
	uint16_t packet_length;
	uint16_t index;

	(void)endpoint;

	if ((USB_RX_QUEUE_CAPACITY - rx_count) < USB_BULK_PACKET_SIZE) {
		usbd_ep_nak_set(device, USB_RX_ENDPOINT, 1U);
		rx_endpoint_naked = true;
		return;
	}

	packet_length = usbd_ep_read_packet(device, USB_RX_ENDPOINT, packet,
					    USB_BULK_PACKET_SIZE);
	for (index = 0U; index < packet_length; ++index) {
		rx_queue_push(packet[index]);
	}

	if ((USB_RX_QUEUE_CAPACITY - rx_count) < USB_BULK_PACKET_SIZE) {
		usbd_ep_nak_set(device, USB_RX_ENDPOINT, 1U);
		rx_endpoint_naked = true;
	}
}

static void data_tx_callback(usbd_device *device, uint8_t endpoint)
{
	(void)device;
	(void)endpoint;
	tx_kick();
}

static void set_configuration(usbd_device *device, uint16_t value)
{
	(void)value;

	usbd_ep_setup(device, USB_RX_ENDPOINT, USB_ENDPOINT_ATTR_BULK,
		      USB_BULK_PACKET_SIZE, data_rx_callback);
	usbd_ep_setup(device, USB_TX_ENDPOINT, USB_ENDPOINT_ATTR_BULK,
		      USB_BULK_PACKET_SIZE, data_tx_callback);
	usbd_ep_setup(device, USB_NOTIFICATION_ENDPOINT,
		      USB_ENDPOINT_ATTR_INTERRUPT, 16U, NULL);
	usbd_register_control_callback(
		device, USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
		USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT, cdc_control_request);
	usb_configured = true;
	tx_kick();
}

static void tx_kick(void)
{
	uint8_t packet[USB_BULK_PACKET_SIZE];
	size_t packet_length;
	size_t index;
	uint16_t written;

	if (!usb_configured || (tx_count == 0U)) {
		return;
	}

	packet_length = tx_count;
	if (packet_length > USB_BULK_PACKET_SIZE) {
		packet_length = USB_BULK_PACKET_SIZE;
	}

	for (index = 0U; index < packet_length; ++index) {
		packet[index] = tx_queue[(tx_tail + index) % USB_TX_QUEUE_CAPACITY];
	}

	written = usbd_ep_write_packet(usb_device, USB_TX_ENDPOINT, packet,
				       (uint16_t)packet_length);
	if (written == 0U) {
		return;
	}

	tx_tail = (tx_tail + written) % USB_TX_QUEUE_CAPACITY;
	tx_count -= written;
}

void usb_serial_init(void)
{
	rx_head = 0U;
	rx_tail = 0U;
	rx_count = 0U;
	tx_head = 0U;
	tx_tail = 0U;
	tx_count = 0U;
	rx_endpoint_naked = false;
	usb_configured = false;

	usb_device = usbd_init(&st_usbfs_v1_usb_driver, &device_descriptor,
			       &configuration_descriptor, usb_strings, 3,
			       control_buffer, sizeof(control_buffer));
	(void)usbd_register_set_config_callback(usb_device, set_configuration);
}

void usb_serial_poll(void)
{
	usbd_poll(usb_device);
	tx_kick();

	if (rx_endpoint_naked &&
	    ((USB_RX_QUEUE_CAPACITY - rx_count) >= USB_BULK_PACKET_SIZE)) {
		usbd_ep_nak_set(usb_device, USB_RX_ENDPOINT, 0U);
		rx_endpoint_naked = false;
	}
}

bool usb_serial_read_byte(uint8_t *byte)
{
	if (rx_count == 0U) {
		return false;
	}

	*byte = rx_queue[rx_tail];
	rx_tail = (rx_tail + 1U) % USB_RX_QUEUE_CAPACITY;
	--rx_count;
	return true;
}

bool usb_serial_write(const uint8_t *data, size_t length)
{
	size_t index;

	if (length > usb_serial_tx_free()) {
		return false;
	}

	for (index = 0U; index < length; ++index) {
		tx_queue_push(data[index]);
	}
	tx_kick();
	return true;
}

size_t usb_serial_tx_free(void)
{
	return USB_TX_QUEUE_CAPACITY - tx_count;
}
