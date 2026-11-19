/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A52_GPU_CX_TRACE_H
#define A52_GPU_CX_TRACE_H
/* A52_PHASE446S_GPU_CX_VOTES_V1: diagnostic, no regulator writes */
struct regulator;
void a52_gpu_cx_dump_votes(struct regulator *reg, const char *phase);
#endif
