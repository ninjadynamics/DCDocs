# Current Fork Snapshot (June 2026)

This page records the concrete state of the Dreamcast fork stack after the first batch of raylib-dc, GLdc, and dcmesh work. It complements the design pages by naming what is actually present in the forks, what is still planned, and what should be cleaned up before proposing wider upstream adoption.

## Repository Heads

### raylib-dc

Remote:

`https://github.com/ninjadynamics/raylib.git`

Baseline used for local comparison:

`65cfa062e5207f0105b9b41f2ec17967c5299cd4`

Current head in the analyzed fork:

`ba16cf013`

Primary changes since baseline:

- Dreamcast rlgl immediate-mode batcher in `src/rlgl_dc_batch.h`.
- Dreamcast hooks in `src/rlgl.h` for capture, deferred unbinds, flush boundaries, direct array draw drains, and GLdc mipmap generation.
- DCMesh runtime in `src/dc_mesh.c`, `src/dc_mesh.h`, and `src/dcmesh.h`.
- Transparent DCMesh hooks in `src/rmodels.c` for `LoadModel`, `UnloadModel`, `UploadMesh`, and `DrawMesh`.
- Optional SH4ZAM paths in `src/raymath.h`.
- Mesh re-upload guard for GLdc/OpenGL 1.x paths where `vaoId` remains zero.
- Makefile/CMake build integration for DCMesh and `USE_SH4ZAM`.

The raw diff is large because of textual churn in `rmodels.c`, but with whitespace ignored the substantive raylib-dc delta is about 1500 insertions and 30 deletions.

### GLdc

Remote:

`https://github.com/ninjadynamics/GLdc.git`

Baseline used for local comparison:

`ba10615722b21bd60f527676f0218d6adc6599fd`

Current head in the analyzed fork:

`e99062a`

Primary changes since baseline:

- Optional GLdc performance counters in `GL/gldc_stats.h` and `GL/gldc_stats.c`.
- Draw, submit, fast-path, header, dirty-state, texture-bind, immediate-mode, clipping, scene-list, strip, and Patch-E counters.
- Position+UV+color fast path for non-indexed quads and triangles in `GL/draw.c`.
- Punch-through blend fix using `SRC_ALPHA` / `INV_SRC_ALPHA` instead of one/zero.
- SH4-side clipping/submission optimization in `GL/platforms/sh4.c`.
- Deferred fog queue APIs in `include/GL/glkos.h`.
- Render-to-texture utility APIs in `include/GL/glkos.h`.
- Texture conversion out-of-memory guard.
- Limited SH4ZAM support in `GL/matrix.c`.

### dcmesh

Remote:

`git@github.com:ninjadynamics/dcmesh.git`

Initial baseline:

`86bc187ed054b5835dac2772f495ff5b95f21da8`

Current head in the analyzed fork:

`a6adf79`

Primary changes since initial version:

- `.dcmesh` format version 2.
- `vertex_map` per expanded strip vertex.
- Node world transform baking with `cgltf_node_transform_world()`.
- Convenience generation script updates.

## Implemented Architecture

The current stack should be understood as one pipeline:

`raylib helpers -> rlgl Dreamcast batcher -> GLdc array fast paths -> KOS/PVR`

For helper-heavy 2D/UI/textured geometry, the batcher captures `RL_QUADS` and `RL_TRIANGLES` into a 24-byte interleaved layout:

- 3 floats position
- 2 floats UV
- BGRA color bytes

That layout is deliberately matched by:

- `RlDcBatchVertex` in raylib-dc
- `DCVertex` in DCMesh
- GLdc's P+UV+color fast path

The important point is that the wins are multiplicative. The raylib-dc batcher reduces submit count, deferred unbinds reduce dirty state/header churn, and GLdc's PUC path reduces the per-vertex work that remains.

## DCMesh Status

DCMesh is an optional sidecar path, not a replacement for raylib's model loader.

Runtime behavior:

- `LoadModel()` loads the usual raylib model first.
- If `model.dcmesh` exists next to the model, it is loaded and linked.
- `DrawMesh()` routes DCMesh-backed meshes through strip rendering.
- `UploadMesh()` syncs raylib vertices/colors into strip-expanded data through `vertex_map`.
- `UnloadModel()` frees the DCMesh registry entry.

The runtime uses `Mesh.vaoId` as a tagged registry handle on Dreamcast GL 1.1 because VAOs are unused there. This is practical, but upstream documentation should call it out clearly as backend-private behavior.

## GLdc Utility APIs Added

The fork exposes a few Dreamcast/KOS-specific helpers through `glkos.h`:

- `glKosQueueFogTableLinear(...)`
- `glKosQueueFogTableFlat(...)`
- `glKosFlushToTexture(void *tex, unsigned int w, unsigned int h)`
- `glKosTextureData(GLuint texId)`

These are not generic OpenGL abstractions. They are KOS/PVR utilities for safe fog register timing and render-to-texture workflows.

## SH4ZAM Status

SH4ZAM is present but not complete.

Already implemented:

- raymath conversions between raylib `Matrix`/`Quaternion` and SH4ZAM layouts.
- SH4ZAM paths for selected normalization, transform, multiply, inverse, and simple rotation helpers.
- alignment-safe unaligned matrix loads for by-value raylib `Matrix` parameters.
- GLdc normal-matrix inverse/transpose path.

Still planned:

- direct `rlRotatef` / `glRotatef` XMTRX rotation paths.
- wider raymath rotation/quaternion constructor coverage.
- GLdc matrix-stack audit and replacement of remaining KOS/scalar hot paths.
- 32-byte alignment audit.
- trig audit for hot Dreamcast-only paths.

The older assumption that GLdc matrix stack operations are already "done" because they use KOS matrix functions should be treated as provisional. The follow-up plan says those paths may still be materially slower than the corresponding SH4ZAM routines.

## Upstream Cleanup List

Before upstreaming broadly:

- Remove timestamp/canary logging or replace it with normal build metadata.
- Resolve the DCMesh color-channel TODO in `dcMeshSyncFromRaylib()`.
- Clarify the term "Patch E": local DCMesh Patch E is opaque-list routing, not full direct-PVR submission.
- Make DCMesh build inclusion Dreamcast-only where build systems prefer that.
- Validate `vertex_map` source indices before narrowing to `uint16_t`.
- Document `Mesh.vaoId` tagging as Dreamcast GL 1.1 backend-private.
- Keep GLdc stats optional and zero-overhead when disabled.
- Hardware-test SH4ZAM paths; emulator success is not enough for alignment or matrix correctness.

