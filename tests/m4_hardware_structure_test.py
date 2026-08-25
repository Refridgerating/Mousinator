from pathlib import Path


board = Path("src/board.c").read_text(encoding="utf-8")
stepper = Path("src/stepper.c").read_text(encoding="utf-8")
controller = Path("src/motion_controller.c").read_text(encoding="utf-8")
uart = Path("src/tmc_uart.c").read_text(encoding="utf-8")

assert "GPIO_CNF_INPUT_PULL_UPDOWN" in board
assert "gpio_set(GPIOC, TILT_ENDSTOP_PIN);" in board
assert "BOARD_TILT_ENDSTOP_ACTIVE_LOW" in board
assert "AFIO_MAPR_TIM2_REMAP_PARTIAL_REMAP2" in board
assert "AFIO_MAPR_USART3_REMAP_PARTIAL_REMAP" in board
assert "AFIO_MAPR_TIM3" not in board

assert "#define PAN_STEP_TIMER TIM2" in stepper
assert "#define TILT_STEP_TIMER TIM3" in stepper
assert "timer_set_oc_mode(TILT_STEP_TIMER, TIM_OC3, TIM_OCM_PWM2);" in stepper
assert "timer_set_oc_value(TILT_STEP_TIMER, TIM_OC3" in stepper
tilt_init = stepper[
    stepper.index("void stepper_tilt_init(void)") : stepper.index(
        "void stepper_tilt_set_enabled", stepper.index("void stepper_tilt_init(void)")
    )
]
assert "board_tilt_set_enabled(false);" in tilt_init
assert "board_tilt_set_enabled(true);" not in tilt_init

assert "#define CONTROL_TIMER TIM4" in controller
assert "void tim4_isr(void)" in controller
assert "void tim3_isr(void)" not in controller
assert "void tim2_isr(void)" not in controller
assert "driver_control_ready(driver_axis(axis))" in controller
assert controller.index("driver_control_ready(driver_axis(axis))") < controller.index(
    "stepper_set_enabled(axis, true);"
)

dual_start = controller.index("motion_controller_set_both_velocities(")
dual_end = controller.index("void motion_controller_stop_all", dual_start)
dual = controller[dual_start:dual_end]
assert dual.count("control_irq_disable();") == 1
assert dual.index("result.tilt_result") < dual.index(
    "axis_motion_set_target_velocity(&pan_state"
)

isr = controller[controller.index("void tim4_isr(void)") :]
for forbidden in (
    "usb_",
    "tmc2209",
    "tmc_uart",
    "usart_",
    "printf",
    "malloc",
    "calloc",
    "realloc",
    "free(",
):
    assert forbidden not in isr
assert "/" not in isr

assert "TMC_UART_BAUD_RATE 40000U" in Path("include/tmc_uart.h").read_text(
    encoding="utf-8"
)
assert "void tim4_isr" not in uart

print("M4 hardware structure tests passed")
