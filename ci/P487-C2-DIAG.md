# P487 C2 diagnostic quieting (partial inventory)

a52.diag=0 is the compiled default, a52.diag=1 enables the selected old trace output. Existing explicit a52.trace_verbose remains an opt-in for the display verbosity path. Only printk calls changed; original ring collection, counters, hardware operations, failure markers, secure flight recorder and safety guards remain. Existing original errors/warnings not selected remain visible. IPA bus vote failure always logged; successful high-rate vote is gated. KGSL successful runtime-PM reference traces gated, failures unchanged. I2C/BT/UART selected repetitive traces gated, not full inventory.

Changed print sites:
- drivers/platform/msm/ipa/ipa_v3/ipa.c: 1
- drivers/gpu/drm/drm_atomic_uapi.c: 23
- drivers/gpu/msm/kgsl_pwrctrl.c: 2
- drivers/gpu/msm/kgsl_gmu.c: 4
- drivers/i2c/busses/i2c-qcom-geni.c: 9
- drivers/bluetooth/bluetooth_power_p473.c: 2
- drivers/tty/serial/qcom_geni_serial.c: 5
- drivers/a52_display/msm/msm_drv.c: 3

This is partial C2, not proof of all old display/GPU/I2C/BT traces being quiet. Remaining P446/P447 sources and actual log-rate test needed. Main untouched. NO FLASH.
