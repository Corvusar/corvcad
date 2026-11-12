// SPDX-License-Identifier: GPL-3.0-or-later
// C surface of the 3D part generator for the app (docs/3D_MODELS.md gate 3). Linked only in the
// NATIVE_FREECAD edition, whose static OCCT it uses; other editions never call it.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One triangle of a generated part, in KiCad's model frame (millimetres, z up), counter-clockwise
// seen from outside, with its solid's colour (0..1).
typedef struct { float ax, ay, az, bx, by, bz, cx, cy, cz, r, g, b; } cc_part_triangle;

// The triangles of the generated part for a footprint's model path (directory and extension are
// ignored). Returns how many there are; `out` may be null to ask. Returns 0 with `error` set when no
// generator covers the name or construction failed. Parts are kept in memory after the first call.
size_t cc_parts3d_triangles(const char *model_path, cc_part_triangle *out, size_t capacity,
                            char *error, size_t error_capacity);

// The key a cached mesh of this model path is stored under (generator version, family, parameter
// hash), written to `out`; returns its length, 0 when no generator covers the name.
size_t cc_parts3d_key(const char *model_path, char *out, size_t capacity);

// Forgets every part kept in memory, so the next call generates again.
void cc_parts3d_clear(void);

#ifdef __cplusplus
}
#endif
