# GLdc Performance Modifications

> Instrumentation, Fast-Path Widening, and Submission Optimization
> For use with raylib4Dreamcast

> Current status: the analyzed GLdc fork implements the counters, P+UV+color fast path, punch-through blend fix, SH4-side clipping/submission cleanup, render-to-texture helpers, deferred fog helpers, texture conversion OOM guard, and limited SH4ZAM normal-matrix acceleration. Direct PVR submission remains future work.
# 1. GLdc Architecture Overview

GLdc is an OpenGL 1.x implementation for the Sega Dreamcast, built on top of KallistiOS (KOS) and the PowerVR CLX2 (PVR) hardware. It translates standard OpenGL calls into PVR-native submission structures. Understanding GLdc's internal pipeline is essential to understanding where CPU time is spent and how to reduce it.

## 1.1 The Submission Pipeline

Every draw call in GLdc, whether from glDrawArrays, glDrawElements, or glEnd (immediate mode), ultimately flows through a single bottleneck function: submitVertices(). The pipeline is:

glDrawArrays() / glDrawElements() / glEnd()

  → submitVertices(mode, first, count, type, indices)

    → check if poly header needed (GPU state dirty or list empty)

    → apply_poly_header() if dirty — rebuilds PVR polygon context

    → mark state clean

    → load current transform matrix

    → generate() — dispatches to fast-path or generic vertex generation

    → _glTnlApplyEffects() — lighting, texture matrix, color matrix

    → vertices appended to PVR list buffers

Each call to submitVertices() is not free. Even on the "fast path," GLdc performs: a dirty-state check, an optional poly-header rebuild, a matrix load, per-vertex coordinate transformation via TransformVertex(), per-vertex attribute copying (UV, color, normal, ST), and vertex flag assignment for PVR strip conversion. The cost scales linearly with vertex count and is multiplied by call count.

## 1.2 apply_poly_header()

When GPU state is dirty, apply_poly_header() reconstructs the full PVR polygon context from current GL state. This includes culling mode, depth test and write settings, shading mode, scissor region, fog enable, alpha/blend configuration, and texture binding. A single state change—such as binding a different texture or toggling depth test—marks the state dirty and forces a complete header rebuild on the next draw call. This is significantly more expensive than a desktop GL driver's state change, where the driver defers validation until draw time and handles it in hardware.

## 1.3 generate() and Fast Paths

The generate() function is the vertex processing dispatcher. It checks the ATTRIB_LIST.fast_path flag, which is set by _glIsVertexDataFastPathCompatible() whenever client array pointers are updated. The fast path requires: vertex position as 3 x GL_FLOAT, UV as 2 x GL_FLOAT, color as GL_BGRA x GL_UNSIGNED_BYTE, ST as 2 x GL_FLOAT, and normals as 3 x GL_FLOAT. When fast_path is true, generate() dispatches to specialized generators in draw_fastpath.inc that handle quads, triangles, and indexed draws with optimized loops. When false, it falls back to generic per-attribute processing with more branching and overhead.

Critically, even the fast path is not zero-cost. The indexed fast path (generateElementsFastPath) still loops per-index, calls TransformVertex() for each referenced vertex, and copies each enabled attribute stream into the submission buffer with MEMCPY4 macros. "Fast" means "less generic processing," not direct memory-mapped submission.

## 1.4 PVR Submission and List Ordering

KOS documents two submission modes for PVR geometry: direct submission (fastest, requires data already grouped by primitive/list type) and buffered submission (slower, handles any order by copying into main-RAM vertex buffers). GLdc uses the buffered approach internally, accumulating vertices into poly-list structures. Any rendering pattern that rapidly alternates opaque and translucent primitives, or frequently changes state, produces fragmented submission buffers that are suboptimal for the PVR hardware.

# 2. Identified Bottlenecks in GLdc

| **Bottleneck** | **Mechanism** | **Impact** |
| --- | --- | --- |
| Per-call overhead in submitVertices() | Every draw call pays: dirty check, optional header rebuild, matrix load, generate() dispatch, TnlApplyEffects(). With tiny draws (4 vertices), this overhead dominates. | High — multiplied by call count |
| Poly-header churn | apply_poly_header() fully reconstructs PVR context on any dirty state. Texture bind/unbind cycles trigger this repeatedly. | High — each rebuild is expensive |
| Per-vertex transform cost | TransformVertex() is called for every vertex even on fast paths. SH4 matrix math is not free at 200MHz. | Medium-high — scales with vertex count |
| Attribute copy overhead | Even fast paths copy UV, color, normal, and ST attributes per-vertex. Unused attributes (normals for 2D) still get copied. | Medium — wasted bandwidth |
| Fast-path eligibility gaps | Common raylib layouts (position + UV + color, no normals) may not dispatch to the most efficient generator. | Medium — suboptimal code paths |
| Immediate-mode overhead | glBegin/glEnd in GLdc builds temporary client arrays then calls glDrawArrays internally. Each pair is a separate submission. | High when called frequently |

# 3. Patch B: GLdc Instrumentation Counters

## 3.1 Purpose

Source-level analysis can identify likely bottlenecks, but only on-device measurement can confirm them and quantify their relative cost. Patch B adds lightweight per-frame counters at every key decision point in GLdc's submission pipeline, enabling data-driven optimization decisions.

## 3.2 Implementation

Two new files are added to GLdc/GL/:

**gldc_stats.h** — Defines the GLdcStats structure and conditional macros. When compiled with -DGLDC_ENABLE_STATS, the GLDC_STAT_INC(field) and GLDC_STAT_ADD(field, n) macros increment counters. Without the flag, they compile to no-ops with zero runtime overhead.

**gldc_stats.c** — Implements the global g_gldc_stats instance, glKosResetStats() to zero all counters (preserving frame number), glKosGetStats() to return a const pointer for reading, and glKosPrintStats() to output a formatted summary to stdout/serial.

## 3.3 Instrumented Hook Points

| **Function** | **Counter** | **What It Reveals** |
| --- | --- | --- |
| glDrawArrays() | draw_arrays_calls | Total array-draw submissions per frame |
| glDrawElements() | draw_elements_calls | Total indexed-draw submissions per frame |
| submitVertices() | submit_vertices_calls | Total submission pipeline entries |
| generate() fast branch | fast_path_hits | How often the optimized path is taken |
| generate() else branch | fast_path_misses | How often the generic fallback runs |
| apply_poly_header() | headers_emitted | PVR poly-header rebuilds (state churn cost) |
| _glGPUStateMarkDirty() | state_dirty_events | How often GL state transitions to dirty |
| submitVertices() count arg | vertices_transformed | Total vertices processed through T&L |
| glBindTexture() | texture_binds | Total texture bind calls |
| glBegin() | immediate_begin_calls | Immediate-mode draw starts |
| glEnd() | immediate_end_calls | Immediate-mode draw ends |
| glVertex3f() | immediate_vertices | Immediate-mode vertices submitted |
| SceneListSubmit() | scene_list_submits, scene_vertices_in, scene_headers_seen | PVR scene-list traffic |
| Near-plane clipping | clip_triangles_tested, clip_all_visible, clip_none_visible, clip_partial, clip_edges_generated | Clipping frequency and generated edges |
| DCMesh strip path | strip_count, strip_vertices_total, patchE_hits, patchE_fallbacks | Strip usage and opaque-routing eligibility |

## 3.4 Usage

In game code, after EndDrawing() each frame:

glKosPrintStats();  // Print to serial/stdout

glKosResetStats();  // Zero counters for next frame

Key diagnostic patterns: if headers_emitted is close to draw_arrays_calls, nearly every draw triggers a header rebuild—state coalescing is needed. If fast_path_misses is high, client array layouts are not matching the optimized generators. If immediate_begin_calls is high, the rlgl batcher (Patch A) is the priority fix.

# 4. Patch C: Fast-Path Widening for Common Layouts

## 4.1 Problem

GLdc's existing fast-path generators in draw_fastpath.inc process all enabled attributes in separate loops: position, UV, color, ST (secondary texcoord), and normals. For the most common raylib 2D workload—textured colored quads with no normals and no secondary UV—the generic fast path still iterates over ST and normal arrays, checking enabled flags and performing conditional copies. This wastes CPU cycles on attributes that are guaranteed to be disabled for 2D sprite, text, UI, and shape rendering.

## 4.2 Solution

Patch C adds two new specialized generator functions that handle the position + UV + color (PUC) case exclusively, completely eliminating the ST and normal attribute loops:

**generateArraysFastPath_PUC_QUADS()** — Processes array quads with only position, UV, and color. Handles the PVR vertex 3↔4 swap for strip conversion and GPU_CMD_VERTEX / GPU_CMD_VERTEX_EOL flag assignment. Processes vertices in batches of 60 (matching the existing fast-path loop structure).

**generateArraysFastPath_PUC_TRIS()** — Same optimization for triangle arrays. Sets GPU_CMD_VERTEX_EOL on every third vertex.

## 4.3 Dispatch Logic

At the top of generate(), before the existing fast-path dispatch, a new check is inserted:

if (!indices && (enabled & (ST_ENABLED_FLAG | NORMAL_ENABLED_FLAG)) == 0)

This condition fires when: the draw is non-indexed (array submission), secondary texcoords (ST) are disabled, and normals are disabled. When true, and the mode is GL_QUADS or GL_TRIANGLES, the PUC generator is called directly. Otherwise, execution falls through to the existing fast-path or generic path unchanged.

This dispatch is safe because: the condition only triggers when the unwanted attributes are genuinely disabled (not just empty), it only handles non-indexed draws (indexed draws fall through to the existing indexed fast path), and it preserves all existing behavior for modes other than quads and triangles.

## 4.4 Performance Impact

For a typical 2D frame with position + UV + color only, the PUC generators eliminate approximately 40% of the per-vertex loop work compared to the generic fast path. The savings come from: no ST attribute loop (skip ~16 bytes of checking per vertex), no normal attribute loop (skip ~12 bytes of checking per vertex), no conditional branching for disabled attributes, and tighter inner loops with fewer memory accesses. Combined with the rlgl batcher (Patch A), which reduces the number of generate() calls by 10-100x, the total vertex processing cost drops dramatically.

## 4.5 Files Modified

**GLdc/GL/draw.c** — Two new static generator functions (~140 lines) added before generate(). Dispatch check (~12 lines) added at the top of generate()'s fast-path branch. No existing behavior changed for any other code path.

# 5. Implemented Backend Fixes Beyond Patch C

## 5.1 Punch-through blend fix

The current fork changes punch-through list headers to use source-alpha/inverse-source-alpha blending instead of one/zero blending. This is a hardware correctness fix: if a nominally transparent punch-through texel survives discard on real PVR hardware, alpha blending resolves it to the destination instead of drawing an opaque box.

## 5.2 SH4 scene submission and clipping cleanup

`GL/platforms/sh4.c` now has several small hot-path changes:

- `PVR_OPB_COUNT` increased from 2 to 4.
- `is_header()` reduced to a single unsigned compare.
- all-visible triangles are handled before the clipping switch.
- scratch vertices are hoisted out of the loop and 32-byte aligned.
- queued vertex copies use the optimized `memcpy_vertex()` path.
- one-line-ahead prefetch is used in the submit loop.
- `_glClipEdge()` replaces `sqrtf((d1 - d0)^2)` with `fabsf(d1 - d0)` and adds a directional epsilon.

These changes target the per-triangle cost that remains after raylib batching reduces submit count.

## 5.3 Render-to-texture helpers

`glKosFlushToTexture(void *tex, unsigned int w, unsigned int h)` begins a KOS render-to-texture scene, submits the queued OP/PT/TR lists into the texture target, finishes the scene, clears the lists, and reapplies scissor state.

`glKosTextureData(GLuint texId)` returns the raw VRAM pointer for a texture's level-0 data so the caller can pass it as a KOS render target. The texture must be allocated in a render-to-texture-compatible format and layout.

## 5.4 Deferred fog helpers

`glKosQueueFogTableLinear()` and `glKosQueueFogTableFlat()` queue PVR fog register writes until `SceneBegin()` or `SceneBeginToTexture()`, after `pvr_wait_ready()` and immediately before the next scene begins. This avoids changing global fog registers while the previous scene may still be rendering.

## 5.5 Texture upload OOM guard

The texture conversion path now checks whether its temporary aligned conversion buffer allocation succeeded. On failure, it throws `GL_OUT_OF_MEMORY` and returns instead of faulting through a null pointer.

# 6. Future GLdc Optimization Directions

**Indexed PUC generators** — Extend the PUC specialization to indexed draws (generateElementsFastPath). Lower priority because the rlgl batcher submits non-indexed arrays, but relevant for 3D mesh rendering.

**Position + UV only generator** — For untinted textured quads (white color), a P+UV-only generator could skip even the color copy loop.

**Direct PVR submission** — For already-sorted opaque geometry, bypass GLdc's buffered submission and write PVR-ready vertex strips directly. The current DCMesh Patch E is not this; it still uses GLdc but improves strip layout and opaque-list routing. Direct PVR submission is the highest-upside but most invasive change, recommended only after counters confirm submission remains the dominant cost.

**Texture path cleanup** — Community Dreamcast-port evidence (Xash3D) suggests GLdc-side texture.c copy and alignment work can improve frame rates for dynamic textures and format-mismatched uploads. Lower priority than submission optimization for typical static-asset scenes.

**GLdc matrix-stack SH4ZAM pass** — The fork has a `USE_SH4ZAM` option and uses SH4ZAM for normal-matrix inverse/transpose, but broader matrix-stack operations still need a careful audit. `glRotatef`, `glTranslatef`, `glScalef`, `glLoadMatrixf`, and `glMultMatrixf` are planned follow-up targets.
