/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>

/*
 * x0: little-endian P010 output, x1: high bytes, x2: packed low pairs,
 * w3: positive sample count, a multiple of 16, w4: swap chroma pairs.
 * The caller owns the SIMD context.
 */
SYM_FUNC_START(vdec_p010_pack_neon)
	adr	x5, .Lexpand
	ld1	{v4.16b, v5.16b}, [x5]
	movi	v6.16b, #0xc0
.Lrow:
	ld1r	{v0.4s}, [x2], #4
#ifdef CONFIG_CPU_BIG_ENDIAN
	rev32	v0.16b, v0.16b
#endif
	ld1	{v1.16b}, [x1], #16
	tbl	v0.16b, {v0.16b}, v4.16b
	ushl	v0.16b, v0.16b, v5.16b
	and	v0.16b, v0.16b, v6.16b
	zip1	v2.16b, v0.16b, v1.16b
	zip2	v3.16b, v0.16b, v1.16b
	cbz	w4, .Lstore
	rev32	v2.8h, v2.8h
	rev32	v3.8h, v3.8h
.Lstore:
	st1	{v2.16b, v3.16b}, [x0], #32
	subs	w3, w3, #16
	b.ne	.Lrow
	ret
SYM_FUNC_END(vdec_p010_pack_neon)

	.p2align 4
.Lexpand:
	.byte	0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3
	.byte	6, 4, 2, 0, 6, 4, 2, 0, 6, 4, 2, 0, 6, 4, 2, 0
