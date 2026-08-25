from pathlib import Path


source = Path("src/tmc_uart.c").read_text(encoding="utf-8")
exchange_start = source.index("static tmc2209_error_t tmc_uart_exchange(")
exchange_end = source.index("static void tmc_uart_recover", exchange_start)
exchange = source[exchange_start:exchange_end]

stale_clear = exchange.index("clear_receive_state(false);")
receiver_off = exchange.index("disable_receiver_for_transmit();")
send = exchange.index("usart_send(TMC_USART, transmit[index]);")
wait_tc = exchange.index("wait_for_transmit_flag(USART_SR_TC")
receiver_on = exchange.index("start_receive_phase();", wait_tc)
reply_loop = exchange.index(
    "while (*receive_length < TMC2209_REPLY_DATAGRAM_SIZE)", receiver_on
)
post_read_guard = exchange.index(
    "wait_for_bus_idle(TMC_UART_POST_READ_IDLE_US);", reply_loop
)

recovery_start = source.index("static void tmc_uart_recover")
recovery_end = source.index(
    "static uint8_t tmc_uart_last_exchange_flags", recovery_start
)
recovery = source[recovery_start:recovery_end]

assert (
    stale_clear
    < receiver_off
    < send
    < wait_tc
    < receiver_on
    < reply_loop
    < post_read_guard
)
assert "capture_available" not in exchange[receiver_off:receiver_on]
assert "wait_for_bus_idle(TMC_UART_RECOVERY_IDLE_US);" in recovery
assert "usart_send" not in source[source.index("static void wait_for_bus_idle"):exchange_start]
assert "usart_send" not in recovery
assert "TMC_UART_TURNAROUND_CAPTURE_US" not in source
assert "expected_length += transmit_length" not in source

print("tmc uart transport phase tests passed")
