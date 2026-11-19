/* Phase446X TG sec_input logging compatibility (5.10). */
#ifndef _A52_TOUCH_COMPAT_H
#define _A52_TOUCH_COMPAT_H
#include <linux/device.h>
#include <linux/printk.h>
#ifndef SECLOG
#define SECLOG "[sec_input]"
#endif
#ifndef input_info
#define input_info(mode, dev, fmt, ...) dev_info((dev), fmt, ##__VA_ARGS__)
#endif
#ifndef input_err
#define input_err(mode, dev, fmt, ...) dev_err((dev), fmt, ##__VA_ARGS__)
#endif
#ifndef input_dbg
#define input_dbg(mode, dev, fmt, ...) dev_dbg((dev), fmt, ##__VA_ARGS__)
#endif
#ifndef input_raw_info
#define input_raw_info(mode, dev, fmt, ...) dev_info((dev), fmt, ##__VA_ARGS__)
#endif
#ifndef input_log_fix
#define input_log_fix() do { } while (0)
#endif
/* TG 4.19 Samsung input-event-codes values absent from A12 GKI UAPI. */
#ifndef KEY_SIDE_GESTURE
#define KEY_SIDE_GESTURE 0x1c6
#endif
#ifndef KEY_BLACK_UI_GESTURE
#define KEY_BLACK_UI_GESTURE 0x1c7
#endif
#ifndef KEY_SIDE_GESTURE_RIGHT
#define KEY_SIDE_GESTURE_RIGHT 0x1ca
#endif
#ifndef KEY_SIDE_GESTURE_LEFT
#define KEY_SIDE_GESTURE_LEFT 0x1cb
#endif
#ifndef KEY_INT_CANCEL
#define KEY_INT_CANCEL 0x2be
#endif
/* A52 factory sysfs feature enumeration: four independent bits.
 * Source TG fts_sec.c declares the flags by capability field, but the
 * Samsung common feature defines are absent in A12 GKI.
 * Do not mutate underlying board capability booleans.
 */
#ifndef INPUT_FEATURE_ENABLE_SETTINGS_AOT
#define INPUT_FEATURE_ENABLE_SETTINGS_AOT BIT(0)
#endif
#ifndef INPUT_FEATURE_ENABLE_SYNC_RR120
#define INPUT_FEATURE_ENABLE_SYNC_RR120 BIT(1)
#endif
#ifndef INPUT_FEATURE_SUPPORT_OPEN_SHORT_TEST
#define INPUT_FEATURE_SUPPORT_OPEN_SHORT_TEST BIT(2)
#endif
#ifndef INPUT_FEATURE_SUPPORT_MIS_CALIBRATION_TEST
#define INPUT_FEATURE_SUPPORT_MIS_CALIBRATION_TEST BIT(3)
#endif
#ifndef ABS_MT_CUSTOM
#define ABS_MT_CUSTOM 0x3e
#endif
#ifndef ABS_MT_CUSTOM2
#define ABS_MT_CUSTOM2 0x3f
#endif
#endif
