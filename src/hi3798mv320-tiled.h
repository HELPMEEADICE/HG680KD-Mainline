/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef HI3798MV320_TILED_H
#define HI3798MV320_TILED_H

/* Private Phase 1 ABI. See VDP-KMS-PLAN.md for the storage layout. */
#define MV320_TILED_8		('H' | '8' << 8 | 'T' << 16 | 'V' << 24)
#define MV320_TILED_10		('H' | '0' << 8 | 'T' << 16 | 'V' << 24)
#define MV320_TILED_MODIFIER	0xfe00000000000320ULL

#endif
