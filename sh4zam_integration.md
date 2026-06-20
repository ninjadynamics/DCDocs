# SH4ZAM Integration for raylib and GLdc on Dreamcast

## Overview

[SH4ZAM](https://github.com/gyrovorbis/sh4zam) is a hand-optimized math and linear algebra library for the Sega Dreamcast's SH4 CPU. It provides hardware-accelerated replacements for common floating-point operations using the SH4's dedicated instructions: `FTRV` (4D vector × 4×4 matrix transform), `FIPR` (4D dot product), `FSRRA` (reciprocal square root), and `FSCA` (sine/cosine). SH4ZAM is an official part of [kos-ports](https://github.com/KallistiOS/kos-ports) and was originally developed for the GTA III and Vice City Dreamcast ports.

This document describes the optional integration of SH4ZAM into the [raylib](https://github.com/ninjadynamics/raylib/tree/master) and [GLdc](https://github.com/ninjadynamics/GLdc/tree/master) forks. All changes are compile-time optional behind `#ifdef USE_SH4ZAM`. Without the define, the code compiles identically to before.

Current status: useful pieces are implemented, but SH4ZAM coverage is not complete. The June 2026 follow-up plan treats SH4ZAM as a staged optimization program with hardware verification after each tier.

## What SH4ZAM Replaces

### In raylib (`src/raymath.h`)

| Function | Original | SH4ZAM replacement | Why it helps |
|---|---|---|---|
| `MatrixMultiply` | 64 scalar multiply-adds | `shz_xmtrx_load_apply_store_unaligned_4x4` | Camera, model transforms, DCMesh pipeline |
| `MatrixInvert` | ~50 scalar ops (Cramer's rule) | `shz_mat4x4_inverse` | Camera setup, inverse transforms |
| `Vector3Normalize` | `sqrtf` + divide | `shz_vec3_normalize_safe` (FIPR + FSRRA) | Per-object direction/normal math |
| `Vector3Transform` | 12 scalar multiply-adds | `shz_xmtrx_load_unaligned_4x4` + `shz_xmtrx_transform_vec4` (FTRV) | Coordinate transforms |
| `MatrixRotateX/Y/Z` | scalar sin/cos matrix construction | `shz_mat4x4_init_rotation_x/y/z` | Frequent simple rotation matrices |
| `QuaternionNormalize` | `sqrtf` + divide | `shz_vec4_normalize` (FIPR + FSRRA) | Animation, rotation blending |

### In GLdc (`GL/matrix.c`)

| Function | Original | SH4ZAM replacement | Why it helps |
|---|---|---|---|
| `UpdateNormalMatrix` | Scalar `inverse()` + `transpose()` | `shz_mat4x4_inverse` + `shz_mat4x4_transpose` | Called every modelview change when lighting is active |

### Current Limits and Corrections

- **`rlRotatef` / `glRotatef` are still follow-up targets.** The current plan says direct SH4ZAM XMTRX rotation helpers may avoid matrix construction and memory round-trips.
- **GLdc per-vertex transforms already use FTRV-style hardware instructions.** Replacing only that inner transform with a SH4ZAM wrapper may be lateral unless profiling shows a concrete win.
- **GLdc matrix stack operations are not considered complete.** Older notes treated KOS `mat_load` / `mat_apply` as "already hardware-accelerated enough." The current follow-up plan corrects that: KOS and SH4ZAM can use similar hardware but still differ in scheduling, prefetch, and FPU details. `_glMultMatrix`, `UploadMatrix4x4`, `MultiplyMatrix4x4`, `glLoadMatrixf`, `glMultMatrixf`, `glTranslatef`, `glScalef`, and `glRotatef` need a careful SH4ZAM audit.
- **Composite raymath rotation helpers are still incomplete.** `MatrixRotate`, `MatrixRotateXYZ`, `MatrixRotateZYX`, and quaternion constructors still need review for SH4ZAM direct constructors or `shz_sincosf`.

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

### Alignment Hazard

On SH4, by-value raylib `Matrix` parameters may only be 4-byte aligned. Plain SH4ZAM loaders that rely on `fmov.d` can fault on real hardware when the address is not sufficiently aligned. The current fork uses unaligned-safe loaders for `Vector3Transform()` and `MatrixMultiply()` so the path remains correct even when the ABI provides weaker alignment.

Future alignment work should prefer narrowly scoped fixes first: aligned scratch buffers at hot call sites, or public struct alignment only after checking ABI and interoperability risks.

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

Add `-DUSE_SH4ZAM` to CFLAGS and `-lsh4zam` to LIBS where the SH4ZAM code is compiled:

**raylib** (its Makefile or your build system):
```makefile
USE_SH4ZAM=1
CFLAGS += -DUSE_SH4ZAM
LDLIBS += -lsh4zam
```

**GLdc** (its Makefile or your build system):
```makefile
USE_SH4ZAM=ON
CFLAGS += -DUSE_SH4ZAM
LDLIBS += -lsh4zam
```

**Your game**:
```makefile
CFLAGS += -DUSE_SH4ZAM
LIBS := $(RAYLIB_LINK) $(GLDC_LINK) -lkosutils -lm -lsh4zam
```

The define must reach each library that contains SH4ZAM `#ifdef` blocks. If only the application has it, raylib's `raymath.h` and GLdc's `matrix.c` were compiled without it and contain the original scalar/KOS paths.

### Verification

The local forks print canary messages when `USE_SH4ZAM` is enabled. For upstream-quality verification, also inspect built objects or binaries for expected symbols after each tier:

- `shz_xmtrx_rotate`
- `shz_sincosf`
- `shz_xmtrx_load_4x4`
- `shz_xmtrx_apply_4x4`
- `shz_xmtrx_load_unaligned_4x4`
- `shz_xmtrx_load_apply_store_unaligned_4x4`

Visual validation must happen on hardware. Emulator success is not enough for alignment and matrix correctness.

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
- `MatrixRotateX()`
- `MatrixRotateY()`
- `MatrixRotateZ()`
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

## Follow-up Plan

Work is ordered by risk and ROI:

1. Replace `rlRotatef` / `glRotatef` matrix construction paths with direct SH4ZAM XMTRX rotation helpers where semantics match.
2. Expand raymath coverage for `MatrixRotate`, `MatrixRotateXYZ`, `MatrixRotateZYX`, and quaternion constructors.
3. Audit GLdc matrix stack operations and replace remaining KOS/scalar hot paths under `USE_SH4ZAM`.
4. Audit 32-byte alignment, using aligned scratch buffers or public struct alignment only where safe.
5. Replace hot Dreamcast-only `sinf`/`cosf` pairs with SH4ZAM trig helpers where correctness is clear.

## Expected Impact

Modest but real for the already-implemented paths, with larger upside in the planned rotation and matrix-stack tiers. The functions replaced so far are called per-object or per-matrix-change, not per-vertex. Impact scales with:

- Number of `MatrixMultiply` calls per frame (camera, hierarchical transforms)
- Frequency of modelview changes with lighting active (`UpdateNormalMatrix`)
- Volume of `Vector3Normalize` / `Vector3Transform` calls from game logic

For a scene with simple camera and a few dozen objects, expect single-digit percentage frame time improvement. For scenes with deep transform hierarchies or heavy quaternion animation, the improvement is larger.

## References

- [SH4ZAM repository](https://github.com/gyrovorbis/sh4zam)
- [SH4ZAM documentation](https://sh4zam.falcogirgis.net/)
- [SH4 FTRV Optimizations (Dreamcast Wiki)](https://dreamcast.wiki/SH4_FTRV_Optimizations)
- [raylib fork (master branch)](https://github.com/ninjadynamics/raylib/tree/master)
- [GLdc fork (master branch)](https://github.com/ninjadynamics/GLdc/tree/master)
