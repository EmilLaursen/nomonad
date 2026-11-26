/* SPDX-License-Identifier: 0BSD */
/*
 * Kernel-style integer typedefs. Shared by nomonad.h and layout.h;
 * layout.h is embedded by value in Workspace so it cannot depend on
 * nomonad.h, hence the split.
 *
 * The wayland and xkbcommon APIs use the standard <stdint.h> names;
 * keep those at API boundaries, but for our own state and helpers
 * use these.
 */
#ifndef NOMONAD_TYPES_H
#define NOMONAD_TYPES_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#endif /* NOMONAD_TYPES_H */
