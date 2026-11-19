# A52 secure-service compile probe
ccflags-y += -include $(srctree)/a52-port-compat.h
ccflags-y += -I$(srctree)/a52-compat/include
ccflags-y += -I$(srctree)/a52-compat/include/uapi
obj-y += qseecom.o qtee_shmbridge.o smcinvoke.o qsee_ipc_irq.o qsee_ipc_irq_bridge.o secure_buffer.o a52_legacy_scm.o
# A52_ACK_SECURE_FLIGHT_RECORDER
obj-y += a52_ack_secure_flight_recorder.o
# A52_DISPLAY_BIND_AUDIT_V1
obj-y += a52_display_bind_audit.o

# A52_PHASE432_CTRL_ODSIGN_KEYMINT_QSEE_V1
obj-y += a52_phase432_ctrl.o
# A52_PHASE433_CPU_MEM_PERF_V1
obj-y += a52_phase433_perf.o
# A52_PHASE434_CLEAN_F0_V1
obj-y += a52_phase434_clean.o
# A52_PHASE435_TERMINAL_RACE_V1
obj-y += a52_phase435_race.o
# A52_PHASE436_ADRENO_START_FRONTIER_V1
obj-y += a52_phase436_frontier.o
# A52_PHASE437_KGSL_OPEN_CORRIDOR_V1
obj-y += a52_phase437_open.o

# A52_PHASE446R2_PON_WAKE_DIAG_V1
obj-y += a52_pon_wake_diag.o
