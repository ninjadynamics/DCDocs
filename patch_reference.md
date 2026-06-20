# Patch Reference (A–E)

Performance optimization patches for the raylib4Dreamcast and GLdc rendering pipeline. All Dreamcast code is behind `PLATFORM_DREAMCAST` compile guards; PC/web builds are unaffected.

## Patch A — rlgl Immediate-Mode Batcher

**Layer:** raylib/src/rlgl.h + rlgl_dc_batch.h  
**Target:** Immediate-mode vertex submission overhead

Intercepts `rlBegin`/`rlVertex*`/`rlTexCoord*`/`rlColor*`/`rlEnd` calls for `RL_QUADS` and `RL_TRIANGLES`, accumulates vertices into a single interleaved buffer (`RlDcBatchVertex`, 24 bytes: position + UV + BGRA color), and flushes via one `glDrawArrays` call through GLdc's array fast path. Captures only the position+UV+color attribute subset; normals are skipped during capture. Lines and points fall through to the original GL path.

The batch stays open across consecutive compatible draws and flushes only on correctness boundaries: incompatible texture change, blend/depth/cull/scissor change, matrix mutation, render-target change, draw-mode change, capacity full (4080 vertices, divisible by 12 so no primitive splits), or frame end.

Every matrix operation (`rlPushMatrix`, `rlPopMatrix`, `rlTranslatef`, `rlRotatef`, `rlScalef`, `rlLoadIdentity`, `rlMultMatrixf`, `rlFrustum`, `rlOrtho`) and every GL state operation (`rlEnableColorBlend`, `rlEnableDepthTest`, `rlEnableBackfaceCulling`, `rlEnableScissorTest`, etc.) triggers a flush-before-proceed guard. `rlClearScreenBuffers` and `rlDrawRenderBatchActive` call `rlDcFlushAll` to drain pending geometry. `rlDrawVertexArray` and `rlDrawVertexArrayElements` also flush to preserve draw ordering.

**Files:** `src/rlgl_dc_batch.h` (new, 387 lines), modifications throughout `src/rlgl.h`.

## Patch B — GLdc Instrumentation Counters

**Layer:** GLdc  
**Target:** Diagnostic visibility

Per-frame counters at GLdc hook points: `glDrawArrays` calls, `glDrawElements` calls, `submitVertices` calls, fast-path hits/misses, headers emitted, state-dirty events, vertices transformed, texture binds, immediate-mode begin/end/vertex traffic, clipping counts, scene-list submission, strip counts, and Patch-E hits/fallbacks. Activated with `-DGLDC_ENABLE_STATS`. Added through `submitVertices()`, `generate()`, `apply_poly_header()`, immediate-mode entry points, dirty-state setters, texture binding, and SH4 scene submission/clipping code.

**Status:** Implemented in the analyzed GLdc fork as `GL/gldc_stats.h` and `GL/gldc_stats.c`, with zero-overhead no-op macros when stats are disabled.

## Patch C — GLdc Fast-Path Widening

**Layer:** GLdc  
**Target:** Per-vertex CPU cost in `generate()`

Specialized vertex generators for common raylib client-array layouts that skip generic per-attribute branching. The implemented target layout is position + UV + color (no normals, no secondary UVs), non-indexed quads and triangles. Dispatches based on an attribute mask before falling back to the existing generic fast path. Reduces branchiness and memory traffic for the most common 2D/UI/sprite submission case.

**Status:** Implemented in the analyzed GLdc fork as `generateArraysFastPath_PUC_QUADS()` and `generateArraysFastPath_PUC_TRIS()` in `GL/draw.c`.

## Patch D — Deferred Texture Unbinding

**Layer:** raylib/src/rlgl.h + rlgl_dc_batch.h  
**Target:** Redundant state churn from helper-local texture cleanup

Intercepts `rlSetTexture()` in the OpenGL 1.1 backend. When `rlSetTexture(0)` is called, instead of immediately unbinding and dirtying GLdc state, the batcher sets a `pendingUnbind` flag. If the next `rlSetTexture(id)` matches the current batch texture, the unbind is cancelled entirely. If it's a different texture, the batch is flushed with the old texture first, then the new one is applied. If a non-captured draw mode begins while an unbind is pending, the unbind is resolved immediately.

This directly targets the `rlSetTexture(id)` → draw → `rlSetTexture(0)` → `rlSetTexture(id)` → draw pattern that raylib helpers use throughout `rshapes.c`, `rtext.c`, and similar files.

**Files:** Integrated into `src/rlgl_dc_batch.h` alongside Patch A. `rlSetTexture()` in `src/rlgl.h` routes through `rlDcSetTexture()` on Dreamcast.

## Patch E — Triangle Strip Pipeline and Opaque-List Routing

**Layer:** raylib/src/dc_mesh.c + dc_mesh.h + dcmesh.h, and the dcmesh offline converter  
**Target:** 3D mesh rendering overhead (DrawMesh path)

A two-part system: an offline PC-side converter (`dcmesh`) that turns `.glb` models into `.dcmesh` sidecar files containing pre-computed triangle strips, and a Dreamcast runtime that loads those sidecars and renders strips via `GL_TRIANGLE_STRIP` with minimal state changes.

Note on naming: the current local Patch E does not bypass GLdc with full direct-PVR submission. It routes eligible opaque DCMesh submeshes to GLdc's opaque list by temporarily disabling blending, then submits precomputed strips through `glDrawArrays(GL_TRIANGLE_STRIP, ...)`. A future direct-PVR path remains possible, but it is separate from the implemented DCMesh runtime.

**Offline converter** (`dcmesh` repo): Loads `.glb` via cgltf, applies node world transforms to positions, runs `meshopt_optimizeVertexCacheStrip()` for strip-friendly index ordering, runs `meshopt_stripify()` to generate triangle strips, then pre-expands strip vertices into a de-indexed 24-byte format (position + UV + BGRA color) matching the rlgl batcher layout. Also emits a `vertex_map` array (strip vertex → original vertex index) for runtime sync. Output is a binary `.dcmesh` file (magic `DCM1`, version 2) containing per-submesh headers, vertices, strips, and vertex maps.

**Runtime** (`dc_mesh.c` in raylib): A registry maps `Mesh.vaoId` (unused on GL 1.1) to loaded `DCMeshData` using a `0xDC` magic prefix that encodes both registry index and submesh index. `dcMeshLoadSidecar()` is called transparently from `LoadModel()`, and `dcMeshDraw()` is called transparently from `DrawMesh()` when strip data is present. Rendering sets up client arrays once per submesh, pushes the model transform, then submits each strip as a separate `glDrawArrays(GL_TRIANGLE_STRIP, ...)` call — GLdc's `genTriangleStrip()` is minimal (one EOL flag), so this is efficient. Patch E eligibility (opaque, single-texture, full alpha) is checked per submesh; ineligible submeshes still use the strip path but without the optimized-state guarantee.

**Transparent routing hooks in rmodels.c:** `LoadModel()` calls `dcMeshLoadSidecar()`. `UnloadModel()` calls `dcMeshUnloadModel()`. `UploadMesh()` calls `dcMeshHandleUpload()` to sync raylib vertex/color data to strip vertices via `vertex_map`. `DrawMesh()` checks `dcMeshHasStripData()` and routes to `dcMeshDraw()`.

**Files:** `src/dcmesh.h` (format definition, 112 lines), `src/dc_mesh.h` (runtime API, 85 lines), `src/dc_mesh.c` (implementation, roughly 500 lines). Hooks in `src/rmodels.c`. Converter in separate `dcmesh` repo (`src/dcmesh.c` + shared `src/dcmesh.h`).

## Post-Patch GLdc Utility Work

Additional GLdc changes landed after the original A-E framing:

- Punch-through list blend fix: use source-alpha/inverse-source-alpha to avoid hardware-visible opaque boxes when discard is unreliable.
- Render-to-texture helpers: `glKosFlushToTexture()` and `glKosTextureData()`.
- Deferred fog helpers: `glKosQueueFogTableLinear()` and `glKosQueueFogTableFlat()`.
- Texture conversion OOM guard in `glTexImage2D()`.
- Limited `USE_SH4ZAM` support in `GL/matrix.c`; broader matrix-stack work remains planned.
