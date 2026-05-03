# SH4ZAM Integration for raylib and GLdc on Dreamcast

## Overview

[SH4ZAM](https://github.com/gyrovorbis/sh4zam) is a hand-optimized math and linear algebra library for the Sega Dreamcast's SH4 CPU. It provides hardware-accelerated replacements for common floating-point operations using the SH4's dedicated instructions: `FTRV` (4D vector × 4×4 matrix transform), `FIPR` (4D dot product), `FSRRA` (reciprocal square root), and `FSCA` (sine/cosine). SH4ZAM is an official part of [kos-ports](https://github.com/KallistiOS/kos-ports) and was originally developed for the GTA III and Vice City Dreamcast ports.

This document describes the optional integration of SH4ZAM into the [raylib](https://github.com/ninjadynamics/raylib/tree/experimental) and [GLdc](https://github.com/ninjadynamics/GLdc/tree/master) forks. All changes are compile-time optional behind `#ifdef USE_SH4ZAM`. Without the define, the code compiles identically to before.

## What SH4ZAM Replaces

### In raylib (`src/raymath.h`)

| Function | Original | SH4ZAM replacement | Why it helps |
|---|---|---|---|
| `MatrixMultiply` | 64 scalar multiply-adds | `shz_mat4x4_mult` (4× FTRV) | Camera, model transforms, DCMesh pipeline |
| `MatrixInvert` | ~50 scalar ops (Cramer's rule) | `shz_mat4x4_inverse` | Camera setup, inverse transforms |
| `Vector3Normalize` | `sqrtf` + divide | `shz_vec3_normalize_safe` (FIPR + FSRRA) | Per-object direction/normal math |
| `Vector3Transform` | 12 scalar multiply-adds | `shz_xmtrx_load_4x4` + `shz_xmtrx_transform_vec4` (FTRV) | Coordinate transforms |
| `QuaternionNormalize` | `sqrtf` + divide | `shz_vec4_normalize` (FIPR + FSRRA) | Animation, rotation blending |

### In GLdc (`GL/matrix.c`)

| Function | Original | SH4ZAM replacement | Why it helps |
|---|---|---|---|
| `UpdateNormalMatrix` | Scalar `inverse()` + `transpose()` | `shz_mat4x4_inverse` + `shz_mat4x4_transpose` | Called every modelview change when lighting is active |

### What Was NOT Replaced

- **`glRotatef` trig** — Already uses KOS `fsincos()` (SH4 FSCA instruction) under `_arch_dreamcast`. No improvement possible.
- **GLdc per-vertex transforms** (`transform()` in `draw.c`) — Already uses inline `ftrv xmtrx` assembly via KOS `mat_trans_single3_nodiv`. SH4ZAM wraps the same instruction; replacing it would be a lateral move.
- **GLdc matrix stack operations** (`_glMultMatrix` → `UploadMatrix4x4` / `MultiplyMatrix4x4`) — Already uses KOS `mat_load` / `mat_apply` which are XMTRX-based. Already hardware-accelerated.

## Memory Layout Compatibility

Raylib and SH4ZAM use different memory layouts for the same semantic data. The integration includes conversion helpers that handle this transparently.

### Matrix Layout

Raylib's `Matrix` struct stores elements row-major in memory:

```
Memory: [m0 m4 m8 m12 | m1 m5 m9 m13 | m2 m6 m10 m14 | m3 m7 m11 m15]
```

SH4ZAM's `shz_mat4x4_t` stores elements column-major in memory:

```
Memory: [m0 m1 m2 m3 | m4 m5 m6 m7 | m8 m9 m10 m11 | m12 m13 m14 m15]
```

The named elements (`m0`–`m15`) have the same semantic meaning in both systems, but they sit at different memory offsets. The conversion helpers `_rlShzFromMatrix` and `_rlShzToMatrix` reorder the elements by name, which is equivalent to a transpose.

GLdc's `Matrix4x4` is `float[16]` in column-major order (same as SH4ZAM), so `shz_mat4x4_t*` can be cast directly from `Matrix4x4*` — no conversion needed.

### Quaternion Layout

Raylib's `Quaternion` stores components as `{x, y, z, w}`.
SH4ZAM's `shz_quat_t` stores components as `{w, x, y, z}`.

The conversion helpers `_rlShzFromQuat` and `_rlShzToQuat` handle the swizzle. Getting this wrong produces silent rotation errors — the values are all valid floats, just in the wrong slots.

## How to Enable

### Prerequisites

SH4ZAM must be installed. If you followed the standard KOS setup with kos-ports, it's already there. Otherwise:

```bash
cd $KOS_PORTS/sh4zam
make install clean
```

### Build Flags

Add `-DUSE_SH4ZAM` to CFLAGS and `-lsh4zam` to LIBS for **all three** targets:

**raylib** (its Makefile or your build system):
```makefile
CFLAGS += -DUSE_SH4ZAM
```

**GLdc** (its Makefile or your build system):
```makefile
CFLAGS += -DUSE_SH4ZAM
```

**Your game**:
```makefile
CFLAGS += -DUSE_SH4ZAM
LIBS := $(RAYLIB_LINK) $(GLDC_LINK) -lkosutils -lm -lsh4zam
```

All three must have the define. If only the game has it, the `#ifdef` blocks inside raylib's `raymath.h` and GLdc's `matrix.c` were compiled without it and contain the original scalar code.

### Verification

Add a canary to your game's `main.c`:

```c
static void hello_sh4zam(void) {
#ifdef USE_SH4ZAM
    printf(">>>>> HELLO FROM SH4ZAM!\n");
#else
    printf(">>>>> SH4ZAM is NOT enabled.\n");
#endif
}
```

Call it early in `main()`. If you see `NOT enabled`, the define isn't reaching your compilation unit.

## Modified Files

### `raylib/src/raymath.h`

Added after the `Vector3ToFloat` macro block:

- `#ifdef USE_SH4ZAM` include block for sh4zam headers (`shz_vector.h`, `shz_matrix.h`, `shz_quat.h`, `shz_xmtrx.h`)
- `_rlShzFromMatrix()` / `_rlShzToMatrix()` — matrix layout conversion
- `_rlShzFromQuat()` / `_rlShzToQuat()` — quaternion component swizzle

Modified functions (each wraps existing body in `#ifdef USE_SH4ZAM` / `#else` / `#endif`):

- `Vector3Normalize()`
- `Vector3Transform()`
- `MatrixInvert()`
- `MatrixMultiply()`
- `QuaternionNormalize()`

### `GLdc/GL/matrix.c`

Added at the top:

```c
#ifdef USE_SH4ZAM
#include <sh4zam/shz_matrix.h>
#endif
```

Modified function:

- `UpdateNormalMatrix()` — wraps `inverse()` + `transpose()` with `shz_mat4x4_inverse` + `shz_mat4x4_transpose`

## Relationship to Other Patches

SH4ZAM integration is independent of and complementary to the batcher and GLdc performance patches:

| Patch | What it attacks | SH4ZAM relation |
|---|---|---|
| A — rlgl Batcher | Draw call count, submission overhead | Independent. SH4ZAM doesn't change draw call patterns. |
| B — GLdc Counters | Diagnostics | Independent. Counters measure submission, not math. |
| C — Fast-Path Widen | Per-vertex attribute copy cost | Independent. SH4ZAM targets matrix/vector math, not attribute layout. |
| D — Deferred Unbind | State churn from helper unbinds | Independent. SH4ZAM doesn't change state management. |
| **SH4ZAM** | **Scalar math cost in matrix ops, normalization, transforms** | **Additive polish on top of the above.** |

The batcher and state coalescing patches attack the dominant bottleneck (submission overhead). SH4ZAM attacks a secondary bottleneck (scalar math cost). They stack — apply both for best results.

## Expected Impact

Modest but real. The functions replaced are called per-object or per-matrix-change, not per-vertex (GLdc's per-vertex transforms already use FTRV). Impact scales with:

- Number of `MatrixMultiply` calls per frame (camera, hierarchical transforms)
- Frequency of modelview changes with lighting active (`UpdateNormalMatrix`)
- Volume of `Vector3Normalize` / `Vector3Transform` calls from game logic

For a scene with simple camera and a few dozen objects, expect single-digit percentage frame time improvement. For scenes with deep transform hierarchies or heavy quaternion animation, the improvement is larger.

## References

- [SH4ZAM repository](https://github.com/gyrovorbis/sh4zam)
- [SH4ZAM documentation](https://sh4zam.falcogirgis.net/)
- [SH4 FTRV Optimizations (Dreamcast Wiki)](https://dreamcast.wiki/SH4_FTRV_Optimizations)
- [raylib fork (experimental branch)](https://github.com/ninjadynamics/raylib/tree/experimental)
- [GLdc fork (master branch)](https://github.com/ninjadynamics/GLdc/tree/master)
