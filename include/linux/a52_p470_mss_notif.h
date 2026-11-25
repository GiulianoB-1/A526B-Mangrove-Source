/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_A52_P470_MSS_NOTIF_H
#define _LINUX_A52_P470_MSS_NOTIF_H
void a52_p470_mss_legacy_enter(void);
void a52_p470_mss_legacy_exit(void);
struct rproc;
struct rproc_subdev;
struct rproc_subdev *a52_p487_mss_sync_attach(struct rproc *rproc);
void a52_p487_mss_sync_detach(struct rproc *rproc, struct rproc_subdev *subdev);
#endif
