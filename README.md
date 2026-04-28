# Dreamcast Rendering Performance Optimization Overview
> Research, Architecture, and Implementation
# 1. Executive Summary

The Sega Dreamcast’s Hitachi SH4 processor runs at 200 MHz with 16 MB of main RAM. Its PowerVR CLX2 GPU uses a tile-based deferred renderer that expects geometry submitted as sorted polygon lists with pre-built headers. The rendering stack for raylib on Dreamcast is: game code → raylib helpers → rlgl → GLdc (OpenGL 1.x implementation) → KOS (KallistiOS) → PVR hardware.

The core finding is that stock raylib’s rendering helpers, designed for portability, produce hundreds of tiny independent draw calls per frame. On desktop GPUs with hardware-accelerated drivers, this pattern is absorbed transparently by rlgl’s internal batcher. On Dreamcast, where rlgl uses the OpenGL 1.1 backend with no batching, each helper call becomes a separate trip through GLdc’s full submission pipeline: state checking, poly-header construction, per-vertex transformation, attribute copying, and PVR list management.

The implemented solution keeps game code completely untouched and adds a Dreamcast-specific layer underneath rlgl that captures immediate-mode traffic, coalesces redundant state changes, and submits batched geometry through GLdc’s array fast path. Measured result: up to 3 milliseconds saved per frame.

# 2. Research Methodology

## 2.1 Initial Investigation

The investigation began with an analysis of the full rendering pipeline from game code through PVR hardware submission. The following source repositories were examined: raylib4Consoles/raylib (dreamcast branch) for the rlgl OpenGL 1.1 backend, Kazade/GLdc for the Dreamcast OpenGL implementation, and KallistiOS documentation for PVR scene submission strategies.

## 2.2 Independent Verification

Each architectural claim was verified against the actual source code:

- **rlgl GL 1.1 backend has no batching** — Confirmed. rlBegin() directly calls glBegin(), rlVertex3f() directly calls glVertex3f(), rlEnd() directly calls glEnd(). The internal batch renderer (rlVertexBuffer, rlDrawCall, rlRenderBatch) is compiled only under GRAPHICS_API_OPENGL_33 or GRAPHICS_API_OPENGL_ES2.

- **GLdc’s immediate mode converts to glDrawArrays internally** — Confirmed in GLdc/GL/immediate.c. glEnd() converts the accumulated immediate-mode vertices into client arrays and calls glDrawArrays(), meaning each glBegin/glEnd pair is a separate submitVertices() call.

- **submitVertices() has significant per-call overhead** — Confirmed in GLdc/GL/draw.c. Each call checks dirty state, conditionally rebuilds the poly header via apply_poly_header(), loads the transform matrix, dispatches through generate(), and applies T&L effects.

- **Fast-path eligibility requires specific attribute layouts** — Confirmed in GLdc/GL/attributes.c. _glIsVertexDataFastPathCompatible() requires vertex as 3×GL_FLOAT, UV as 2×GL_FLOAT, and color as GL_BGRA×GL_UNSIGNED_BYTE.

- **raylib helper code uses the bind/draw/unbind pattern** — Confirmed in raylib/src/rshapes.c. DrawRectangle and related functions call rlSetTexture(id), emit a small quad via rlBegin/rlEnd, then call rlSetTexture(0).

- **KOS documents two PVR submission modes** — Confirmed. Direct submission is fastest for already-sorted data; buffered submission handles any order but copies to main-RAM buffers.

## 2.3 Additional Finding

One finding not emphasized in the initial research: the raylib4Consoles/raylib dreamcast fork had made no Dreamcast-specific performance modifications to rlgl. The optimization surface was entirely untouched, meaning the full benefit of batching, state coalescing, and fast-path targeting was available.

# 3. The Rendering Pipeline

The complete call flow for a single DrawTexture() on Dreamcast, before optimization:

| **Layer** | **Function** | **What Happens** |
| --- | --- | --- |
| Game | DrawTexture() | Calls raylib helper with texture, position, tint |
| raylib | DrawTexturePro() | Computes UV and position, calls rlgl functions |
| rlgl | rlSetTexture(id) | GL 1.1: directly calls glEnable + glBindTexture |
| rlgl | rlBegin(RL_QUADS) | GL 1.1: directly calls glBegin(GL_QUADS) |
| rlgl | rlVertex2f() × 4 | GL 1.1: directly calls glVertex2f() × 4 |
| rlgl | rlEnd() | GL 1.1: directly calls glEnd() |
| GLdc | glEnd() | Converts 4 immediate vertices to client arrays, calls glDrawArrays() |
| GLdc | submitVertices() | Checks dirty state, rebuilds poly header, loads matrix |
| GLdc | generate() | Transforms 4 vertices via TransformVertex(), copies UV+color |
| GLdc | TnlApplyEffects() | Applies lighting/texture/color matrix effects if enabled |
| KOS | PVR list append | Vertices added to opaque or translucent PVR list buffer |
| rlgl | rlSetTexture(0) | GL 1.1: glDisable + glBindTexture(0) — dirties state again |

After optimization, the same DrawTexture() call follows this path:

| **Layer** | **Function** | **What Happens** |
| --- | --- | --- |
| Game | DrawTexture() | Unchanged |
| raylib | DrawTexturePro() | Unchanged |
| rlgl | rlSetTexture(id) | Batcher: stores texture ID, cancels pending unbind if same |
| rlgl | rlBegin(RL_QUADS) | Batcher: sets capture flag, returns |
| rlgl | rlVertex2f() × 4 | Batcher: appends 4 vertices to 24-byte buffer (96 bytes) |
| rlgl | rlEnd() | Batcher: clears capture flag, does NOT flush |
| rlgl | rlSetTexture(0) | Batcher: sets pendingUnbind flag, does NOT unbind |
|  | (next frame or state change) | Batcher flushes: one glDrawArrays() for all buffered vertices |

# 4. Implemented Patches

## 4.1 Patch Overview

| **Patch** | **Name** | **Target** | **Risk** | **Effort** | **Expected Win** |
| --- | --- | --- | --- | --- | --- |
| A | rlgl Batcher | raylib/src/rlgl.h | Medium | Medium | Very high |
| B | GLdc Counters | GLdc/GL/*.c | Low | Low | Diagnostic |
| C | Fast-Path Widen | GLdc/GL/draw.c | Medium | Medium | Medium-high |
| D | Deferred Unbind | raylib/src/rlgl.h | Low | Low | Medium |

## 4.2 How Patches Work Together

The patches form a layered optimization stack. Patch D (deferred unbind) eliminates redundant texture state changes at the rlgl layer, preventing them from reaching GLdc at all. Patch A (batcher) converts hundreds of tiny immediate-mode draws into a handful of large array draws. Patch C (fast-path widening) ensures those large array draws are processed by the most efficient vertex generator in GLdc, skipping unnecessary attribute loops. Patch B (counters) provides the instrumentation to verify all of the above on real hardware.

The interaction between patches is multiplicative, not additive. Patch A reduces the number of submitVertices() calls from N to ~1. Patch D reduces the state-dirty events that would force header rebuilds between those reduced calls. Patch C reduces the per-vertex cost within each call. The total frame-time reduction is the product of these improvements, which is why a seemingly modest set of changes produces a 3ms per-frame improvement on hardware.

# 5. Quantified Impact

## 5.1 Same-Texture Quad Scene (200 DrawTexture Calls)

| **Metric** | **Before** | **After** |
| --- | --- | --- |
| submitVertices() calls | 200 | 1–2 |
| Poly headers emitted | Up to 200 | 1–2 |
| State-dirty events | Up to 400 | 0–10 |
| Texture bind calls | Up to 400 | 1–2 |
| Immediate glBegin/glEnd | 200 / 200 | 0 / 0 |
| glDrawArrays() calls | 200 (via glEnd) | 1–2 (via batcher) |
| Vertices per draw call | 4 | 800 |
| Frame time reduction | — | Up to 3ms measured |

## 5.2 Where the Time Is Saved

The 3ms saving comes from eliminating repeated fixed-cost work. On the SH4 at 200MHz, each submitVertices() call involves: function call overhead, L1 cache pressure from branching through state checks, the apply_poly_header() conditional path (even when not taken, the dirty-check branch costs cycles), matrix load from memory, and generate() dispatch. At approximately 15–20 microseconds per call for a 4-vertex submission, 200 calls cost approximately 3–4ms of pure overhead. The batcher eliminates 199 of those calls.

# 6. Correctness Guarantees

The batcher preserves visual correctness through its flush trigger architecture. Every operation that could cause buffered geometry to render incorrectly forces a flush before proceeding:

- **Texture changes** — Flush when a new, incompatible texture is set. Same-texture rebinds are absorbed.

- **Draw mode changes** — Flush when switching between RL_QUADS and RL_TRIANGLES, since they have different vertex counts per primitive.

- **Matrix operations** — Flush on push, pop, translate, rotate, scale, load identity, multiply, frustum, or ortho. GLdc loads the matrix once per submitVertices(), so all buffered vertices must share one matrix.

- **GL state changes** — Flush on enable/disable of blend, depth test, depth mask, backface culling, scissor, and on cull face mode or color mask changes. These affect GLdc's poly-header construction.

- **Direct draw calls** — Flush before rlDrawVertexArray and rlDrawVertexArrayElements to preserve draw ordering with non-batched geometry (e.g., 3D models).

- **Frame boundaries** — Flush at rlDrawRenderBatchActive (called by EndDrawing) and before rlClearScreenBuffers.

- **Non-captured modes** — Flush before falling through to raw GL for RL_LINES or unsupported modes.

All flush triggers are verified by the second-pass code review, which caught several critical missing triggers in the initial implementation (GL state changes, direct draw calls, rlFrustum/rlOrtho, and rlEnableTexture/rlDisableTexture bypass) before hardware testing.

# 7. Platform Isolation

Every line of Dreamcast-specific code is guarded behind #if defined(PLATFORM_DREAMCAST). This define is already set by the KOS build system and the raylib4Consoles Dreamcast toolchain. PC, web, mobile, and other console builds never see the batcher code. The rlgl_dc_batch.h header is only included on Dreamcast builds. The patch_rlgl_v5.py patcher only inserts PLATFORM_DREAMCAST-guarded blocks into existing functions; it does not modify any non-guarded code.

The GLdc patches (Patch B counters and Patch C fast-path widening) are similarly isolated. Counters are compiled only with -DGLDC_ENABLE_STATS. The PUC fast-path generators in draw.c are always compiled but only dispatch when the attribute mask exactly matches position + UV + color with no normals and no ST—a condition that only occurs with the batcher's specific client array setup.

# 8. Future Work

## 8.1 Patch E: Direct PVR Submission

The most architecturally ambitious optimization is to bypass GLdc entirely for specific, already-sorted geometry classes. KOS documents that direct PVR submission is the fastest path when data is pre-grouped by list type. For opaque textured quads that dominate typical 2D game frames, a specialized path could write PVR-ready vertex strips directly to the tile accelerator, skipping GLdc's state management, header construction, and vertex transformation entirely.

This is recommended only after Patches A–D are stable and instrumentation counters confirm that GLdc submission overhead remains the dominant cost. Risk is high: it requires duplicate state tracking, careful GL semantic preservation, and thorough testing of edge cases (translucent blending, depth interaction, scissor regions).

## 8.2 Matrix Pre-Transform

Currently, the batcher flushes on every matrix operation because GLdc loads the transform once per submitVertices(). A more aggressive optimization would pre-transform captured vertices into world/view space on the CPU before appending them to the batch buffer, allowing the batch to span matrix changes. This would significantly reduce flush frequency for helper-heavy code that uses rlPushMatrix/rlPopMatrix per shape (common in text rendering and procedural geometry). The trade-off is additional CPU cost per vertex for the matrix multiply, which should be gated behind profiling to confirm net benefit.

## 8.3 Indexed Batch Submission

The batcher currently submits non-indexed arrays via glDrawArrays(). For quad-heavy workloads, converting to indexed submission with a shared index buffer (0,1,2, 0,2,3, 4,5,6, 4,6,7, ...) would reduce the vertex count by 33% (4 vertices per quad instead of 6 for triangle-decomposed quads). However, GLdc's indexed fast path still performs per-index gather and transform, so the net benefit depends on whether the reduced vertex count outweighs the index-parsing overhead. The counters provide the data to make this decision.

## 8.4 Texture Atlas Awareness

If the game uses a texture atlas (single texture containing multiple sprites), the batcher's deferred unbind already maximizes batching efficiency—all sprites from the same atlas produce zero flushes. For games that do not use atlases, an rlgl-layer texture-sorting pass could reorder independent draws to group same-texture work together, reducing texture-change flushes. This requires careful analysis of draw-order dependencies (Z overlap, blending) and is lower priority than the implemented patches.

# 9. File Reference

## 9.1 raylib Side

| **File** | **Status** | **Purpose** |
| --- | --- | --- |
| rlgl_dc_batch.h | New | Self-contained Dreamcast batcher + deferred unbind + stats |
| rlgl.h | Modified | Batcher integration at 28+ points via PLATFORM_DREAMCAST guards |
| patch_rlgl_v5.py | Tool | Automated patcher for rlgl.h v5.0, performs 11 targeted replacements |

## 9.2 GLdc Side

| **File** | **Status** | **Purpose** |
| --- | --- | --- |
| gldc_stats.h | New | Stats structure + conditional macros |
| gldc_stats.c | New | Stats implementation: reset, get, print |
| draw.c | Modified | Counters in glDrawArrays/Elements, submitVertices, generate, apply_poly_header + PUC generators |
| immediate.c | Modified | Counters in glBegin, glEnd, glVertex3f |
| state.c | Modified | Counter in _glGPUStateMarkDirty |
| texture.c | Modified | Counter in glBindTexture |

## 9.3 Example / Test

| **File** | **Status** | **Purpose** |
| --- | --- | --- |
| dc_stats_example.c | New | Per-frame stats printer + 4 microbenchmark scenes |