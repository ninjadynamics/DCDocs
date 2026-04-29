# Dreamcast Rendering Performance Optimization Overview

> Initial Research and Implementation Plan  
> [ChatGPT Deep Research - Investigation](deep-research-investigation.md) – Full rendering pipeline, bottlenecks, and constraints on Dreamcast  
> [ChatGPT Deep Research - Implementation Plan](deep-research-implementation.md) – Optimization strategy, architecture, and practical steps  
>
> Claude's Implementation  
> [GLdc Performance Modifications](gldc_performance_modifications.md) – Deep dive into GLdc bottlenecks and optimizations  
> [raylib Dreamcast Batcher](raylib_dreamcast_batcher.md) – Design and implementation of the batching layer  
>
> Implementation history:  
> [Patch Reference (A–E)](patch_reference.md) – Detailed description of all patches including strip pipeline  
> [Changelog / Post-Batcher Work](changelog.md) – What was added after the batcher (dcmesh, runtime, hooks) 
>
> Repositories  
> [GLdc fork](https://www.github.com/ninjadynamics/GLdc/tree/master) | [raylib fork](https://github.com/ninjadynamics/raylib/tree/experimental) | [dcmesh converter](https://github.com/ninjadynamics/dcmesh)

# 1. Executive Summary

The Sega Dreamcast’s Hitachi SH4 processor runs at 200 MHz with 16 MB of main RAM. Its PowerVR CLX2 GPU uses a tile-based deferred renderer that expects geometry submitted as sorted polygon lists with pre-built headers. The rendering stack for raylib on Dreamcast is: game code → raylib helpers → rlgl → GLdc (OpenGL 1.x implementation) → KOS (KallistiOS) → PVR hardware.

The core finding is that stock raylib’s rendering helpers, designed for portability, produce hundreds of tiny independent draw calls per frame. On desktop GPUs with hardware-accelerated drivers, this pattern is absorbed transparently by rlgl’s internal batcher. On Dreamcast, where rlgl uses the OpenGL 1.1 backend with no batching, each helper call becomes a separate trip through GLdc’s full submission pipeline: state checking, poly-header construction, per-vertex transformation, attribute copying, and PVR list management.

The implemented solution keeps game code completely untouched and adds Dreamcast-specific layers underneath rlgl:

* Immediate-mode batching and state coalescing (Patches A–D)
* Strip-based mesh rendering via offline preprocessing (Patch E)

Measured result: up to 3 milliseconds saved per frame from batching alone, with additional gains for 3D mesh-heavy scenes.

# 2. Research Methodology

## 2.1 Initial Investigation

The investigation began with an analysis of the full rendering pipeline from game code through PVR hardware submission. The following source repositories were examined: raylib4Consoles/raylib (dreamcast branch) for the rlgl OpenGL 1.1 backend, Kazade/GLdc for the Dreamcast OpenGL implementation, and KallistiOS documentation for PVR scene submission strategies.

## 2.2 Independent Verification

Each architectural claim was verified against the actual source code:

* **rlgl GL 1.1 backend has no batching** — Confirmed. rlBegin() directly calls glBegin(), rlVertex3f() directly calls glVertex3f(), rlEnd() directly calls glEnd(). The internal batch renderer (rlVertexBuffer, rlDrawCall, rlRenderBatch) is compiled only under GRAPHICS_API_OPENGL_33 or GRAPHICS_API_OPENGL_ES2.

* **GLdc’s immediate mode converts to glDrawArrays internally** — Confirmed in GLdc/GL/immediate.c. glEnd() converts the accumulated immediate-mode vertices into client arrays and calls glDrawArrays(), meaning each glBegin/glEnd pair is a separate submitVertices() call.

* **submitVertices() has significant per-call overhead** — Confirmed in GLdc/GL/draw.c. Each call checks dirty state, conditionally rebuilds the poly header via apply_poly_header(), loads the transform matrix, dispatches through generate(), and applies T&L effects.

* **Fast-path eligibility requires specific attribute layouts** — Confirmed in GLdc/GL/attributes.c. _glIsVertexDataFastPathCompatible() requires vertex as 3×GL_FLOAT, UV as 2×GL_FLOAT, and color as GL_BGRA×GL_UNSIGNED_BYTE.

* **raylib helper code uses the bind/draw/unbind pattern** — Confirmed in raylib/src/rshapes.c. DrawRectangle and related functions call rlSetTexture(id), emit a small quad via rlBegin/rlEnd, then call rlSetTexture(0).

* **KOS documents two PVR submission modes** — Confirmed. Direct submission is fastest for already-sorted data; buffered submission handles any order but copies to main-RAM buffers.

## 2.3 Additional Finding

One finding not emphasized in the initial research: the raylib4Consoles/raylib dreamcast fork had made no Dreamcast-specific performance modifications to rlgl. The optimization surface was entirely untouched, meaning the full benefit of batching, state coalescing, and fast-path targeting was available.

# 3. The Rendering Pipeline

The complete call flow for a single DrawTexture() on Dreamcast, before optimization:

| **Layer** | **Function**      | **What Happens**                                                     |
| --------- | ----------------- | -------------------------------------------------------------------- |
| Game      | DrawTexture()     | Calls raylib helper with texture, position, tint                     |
| raylib    | DrawTexturePro()  | Computes UV and position, calls rlgl functions                       |
| rlgl      | rlSetTexture(id)  | GL 1.1: directly calls glEnable + glBindTexture                      |
| rlgl      | rlBegin(RL_QUADS) | GL 1.1: directly calls glBegin(GL_QUADS)                             |
| rlgl      | rlVertex2f() × 4  | GL 1.1: directly calls glVertex2f() × 4                              |
| rlgl      | rlEnd()           | GL 1.1: directly calls glEnd()                                       |
| GLdc      | glEnd()           | Converts 4 immediate vertices to client arrays, calls glDrawArrays() |
| GLdc      | submitVertices()  | Checks dirty state, rebuilds poly header, loads matrix               |
| GLdc      | generate()        | Transforms 4 vertices via TransformVertex(), copies UV+color         |
| GLdc      | TnlApplyEffects() | Applies lighting/texture/color matrix effects if enabled             |
| KOS       | PVR list append   | Vertices added to opaque or translucent PVR list buffer              |
| rlgl      | rlSetTexture(0)   | GL 1.1: glDisable + glBindTexture(0) — dirties state again           |

After optimization, the same DrawTexture() call follows this path:

| **Layer** | **Function**                 | **What Happens**                                              |
| --------- | ---------------------------- | ------------------------------------------------------------- |
| Game      | DrawTexture()                | Unchanged                                                     |
| raylib    | DrawTexturePro()             | Unchanged                                                     |
| rlgl      | rlSetTexture(id)             | Batcher: stores texture ID, cancels pending unbind if same    |
| rlgl      | rlBegin(RL_QUADS)            | Batcher: sets capture flag, returns                           |
| rlgl      | rlVertex2f() × 4             | Batcher: appends 4 vertices to 24-byte buffer (96 bytes)      |
| rlgl      | rlEnd()                      | Batcher: clears capture flag, does NOT flush                  |
| rlgl      | rlSetTexture(0)              | Batcher: sets pendingUnbind flag, does NOT unbind             |
|           | (next frame or state change) | Batcher flushes: one glDrawArrays() for all buffered vertices |

# 4. Implemented Patches

## 4.1 Patch Overview

| **Patch** | **Name**                | **Target**        | **Risk** | **Effort** | **Expected Win** |
| --------- | ----------------------- | ----------------- | -------- | ---------- | ---------------- |
| A         | rlgl Batcher            | raylib/src/rlgl.h | Medium   | Medium     | Very high        |
| B         | GLdc Counters           | GLdc/GL/*.c       | Low      | Low        | Diagnostic       |
| C         | Fast-Path Widen         | GLdc/GL/draw.c    | Medium   | Medium     | Medium-high      |
| D         | Deferred Unbind         | raylib/src/rlgl.h | Low      | Low        | Medium           |
| E         | Triangle Strip Pipeline | raylib + dcmesh   | Medium   | Medium     | High (3D meshes) |

## 4.2 How Patches Work Together

The patches form a layered optimization stack. Patch D (deferred unbind) eliminates redundant texture state changes at the rlgl layer, preventing them from reaching GLdc at all. Patch A (batcher) converts hundreds of tiny immediate-mode draws into a handful of large array draws. Patch C (fast-path widening) ensures those large array draws are processed by the most efficient vertex generator in GLdc, skipping unnecessary attribute loops. Patch B (counters) provides the instrumentation to verify all of the above on real hardware.

The interaction between patches is multiplicative, not additive. Patch A reduces the number of submitVertices() calls from N to ~1. Patch D reduces the state-dirty events that would force header rebuilds between those reduced calls. Patch C reduces the per-vertex cost within each call. The total frame-time reduction is the product of these improvements, which is why a seemingly modest set of changes produces a 3ms per-frame improvement on hardware.

---

## 4.3 Triangle Strip Pipeline

Strips are submitted as `GL_TRIANGLE_STRIP` through GLdc, which handles them with minimal overhead (one EOL flag per strip). To make that viable at scale, meshes are preprocessed offline into strip-friendly layouts and consumed transparently at runtime.

### Offline (dcmesh converter)

A PC-side tool converts `.glb` files into `.dcmesh` sidecar files:

* Applies node world transforms (matching raylib `LoadModel`)
* Optimizes index order for strip generation
* Generates triangle strips
* Pre-expands vertices into a 24-byte interleaved format (position + UV + BGRA)
* Emits a `vertex_map` for runtime synchronization

### Runtime (raylib integration)

The runtime integrates transparently into raylib:

* `LoadModel()` loads `.dcmesh` automatically if present
* `DrawMesh()` switches to strip rendering automatically
* `UploadMesh()` syncs vertex/color updates via `vertex_map`
* `UnloadModel()` cleans up strip data

Other than placing the `.dcmesh` next to the `.glb`, there are **zero code changes required**.

### Rendering behavior

* One client array setup per submesh
* Each strip submitted via `glDrawArrays(GL_TRIANGLE_STRIP, …)`
* Minimal GLdc overhead per strip
* Opaque single-texture meshes share one GL state across all strips

This removes index parsing, reduces state churn, and aligns geometry submission with the PowerVR pipeline’s preferred format.

# 5. Quantified Impact

## 5.1 Same-Texture Quad Scene (200 DrawTexture Calls)

| **Metric**              | **Before**      | **After**          |
| ----------------------- | --------------- | ------------------ |
| submitVertices() calls  | 200             | 1–2                |
| Poly headers emitted    | Up to 200       | 1–2                |
| State-dirty events      | Up to 400       | 0–10               |
| Texture bind calls      | Up to 400       | 1–2                |
| Immediate glBegin/glEnd | 200 / 200       | 0 / 0              |
| glDrawArrays() calls    | 200 (via glEnd) | 1–2 (via batcher)  |
| Vertices per draw call  | 4               | 800                |
| Frame time reduction    | —               | Up to 3ms measured |

## 5.2 Where the Time Is Saved

The 3ms saving comes from eliminating repeated fixed-cost work. On the SH4 at 200MHz, each submitVertices() call involves: function call overhead, L1 cache pressure from branching through state checks, the apply_poly_header() conditional path (even when not taken, the dirty-check branch costs cycles), matrix load from memory, and generate() dispatch. At approximately 15–20 microseconds per call for a 4-vertex submission, 200 calls cost approximately 3–4ms of pure overhead. The batcher eliminates 199 of those calls.

# 6. Correctness Guarantees

The batcher preserves visual correctness through its flush trigger architecture. Every operation that could cause buffered geometry to render incorrectly forces a flush before proceeding:

* **Texture changes** — Flush when a new, incompatible texture is set. Same-texture rebinds are absorbed.

* **Draw mode changes** — Flush when switching between RL_QUADS and RL_TRIANGLES.

* **Matrix operations** — Flush on push, pop, translate, rotate, scale, load identity, multiply, frustum, or ortho.

* **GL state changes** — Flush on blend, depth, cull, scissor, and related changes.

* **Direct draw calls** — Flush before array draws to preserve ordering.

* **Frame boundaries** — Flush at EndDrawing and before clears.

# 7. Platform Isolation

Every line of Dreamcast-specific code is guarded behind `PLATFORM_DREAMCAST`. PC, web, and other builds are unaffected.

# 8. Future Work

## 8.1 Patch E Evolution

Patch E already aligns geometry submission with the GPU’s preferred strip format, significantly reducing GLdc overhead. A full direct-PVR path remains possible but is no longer required for substantial gains.

## 8.2 Matrix Pre-Transform

Pre-transforming vertices could allow batching across matrix changes.

## 8.3 Indexed Batch Submission

Shared index buffers could reduce vertex count further.

## 8.4 Texture Atlas Awareness

Sorting by texture could reduce flushes further.

# 9. File Reference

## 9.1 raylib Side

| **File**        | **Status** | **Purpose**               |
| --------------- | ---------- | ------------------------- |
| rlgl_dc_batch.h | New        | Batcher + deferred unbind |
| rlgl.h          | Modified   | Integration               |
| dcmesh.h        | New        | Format definition         |
| dc_mesh.h       | New        | Runtime API               |
| dc_mesh.c       | New        | Strip system              |

## 9.2 GLdc Side

| **File**     | **Status** | **Purpose**          |
| ------------ | ---------- | -------------------- |
| gldc_stats.h | New        | Stats                |
| gldc_stats.c | New        | Stats impl           |
| draw.c       | Modified   | Counters + fast path |
| immediate.c  | Modified   | Counters             |
| state.c      | Modified   | Counters             |
| texture.c    | Modified   | Counters             |

## 9.3 Example / Test

| **File**           | **Status** | **Purpose**             |
| ------------------ | ---------- | ----------------------- |
| dc_stats_example.c | New        | Stats + microbenchmarks |

## 9.4 External Tooling

| **Tool** | **Purpose**                  |
| -------- | ---------------------------- |
| dcmesh   | `.glb` → `.dcmesh` converter |
