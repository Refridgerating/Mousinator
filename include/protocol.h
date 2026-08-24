#ifndef SENTRY_PROTOCOL_H
#define SENTRY_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTOCOL_LINE_CAPACITY 64U
#define PROTOCOL_MAX_RESPONSE_SIZE 128U

typedef bool (*protocol_write_fn)(const uint8_t *data, size_t length,
					  void *context);

typedef struct {
	char line[PROTOCOL_LINE_CAPACITY];
	size_t line_length;
	bool discarding_oversized_line;
	bool previous_was_carriage_return;
	protocol_write_fn write;
	void *write_context;
} protocol_t;

void protocol_init(protocol_t *protocol, protocol_write_fn write,
		   void *write_context);
void protocol_receive(protocol_t *protocol, const uint8_t *data, size_t length);

#endif
