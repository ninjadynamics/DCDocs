# raylib/rlgl Dreamcast Batcher

> Immediate-Mode Batching, Deferred Texture Unbinding, and State-Change Flush Architecture

# 1. The Problem: Why Stock raylib Is Slow on Dreamcast

## 1.1 rlgl****'****s Two Backend Models

raylib's rendering abstraction layer, rlgl, supports multiple OpenGL backends. On modern platforms (GL 3.3, ES 2.0), rlgl implements an internal batch renderer: vertex data from rlBegin/rlVertex/rlEnd calls is accumulated into CPU-side vertex buffers (rlVertexBuffer), organized by draw calls (rlDrawCall) that track mode and texture changes, and flushed to the GPU as large VBO uploads with indexed drawing. This batching is transparent to game code and is why raylib performs well on desktop and mobile.

On Dreamcast, however, rlgl uses the OpenGL 1.1 backend. In this mode, every rlBegin() call directly calls glBegin(), every rlVertex3f() directly calls glVertex3f(), and every rlEnd() directly calls glEnd(). There is no internal batching whatsoever. Each helper function's draw is an independent GL submission.

## 1.2 The Helper Pattern

raylib's shape, text, and texture helper functions follow a consistent pattern:

rlSetTexture(textureId);    // Bind texture

rlBegin(RL_QUADS);         // Start primitive

    rlTexCoord2f(u0, v0);

    rlVertex2f(x0, y0);    // 4 vertices for one quad

    rlTexCoord2f(u1, v1);

    rlVertex2f(x1, y1);

    // ... (2 more vertices)

rlEnd();                   // End primitive

rlSetTexture(0);           // Unbind texture

This pattern is visible throughout rshapes.c (DrawRectangle, DrawCircle, etc.), rtext.c (DrawTextEx), and rtextures.c (DrawTexture). Each helper call is self-contained: it binds its texture, draws its small primitive, and restores texture state. On desktop, rlgl's internal batcher absorbs these into a single large draw. On Dreamcast's GL 1.1 backend, each one becomes a separate submission through GLdc's full pipeline.

## 1.3 The Cost on Dreamcast

On the GL 1.1 backend, GLdc's immediate-mode implementation (immediate.c) converts each glBegin/glEnd pair into a separate glDrawArrays() call internally. Each of those calls then enters submitVertices(), which may rebuild a poly header, loads the transform matrix, transforms every vertex, copies every enabled attribute, and appends to the PVR list buffers.

A frame that draws 200 textured rectangles through DrawTexture() produces: 200 glBegin/glEnd pairs converted to 200 glDrawArrays calls, 200 submitVertices() entries, up to 200 poly-header rebuilds (due to the bind/unbind texture cycle), 400 state-dirty events (bind + unbind per rectangle), and 800 vertices each individually transformed. The overhead is dominated by per-call fixed costs, not vertex processing.

# 2. Patch A: The Dreamcast Immediate-Mode Batcher

## 2.1 Design Principle

The batcher intercepts rlgl's immediate-mode API calls (rlBegin, rlVertex, rlTexCoord, rlColor, rlEnd) at the rlgl layer, before they reach GLdc. Instead of forwarding each call to GL immediately, it accumulates vertices into a local buffer and defers the actual GL submission until a correctness boundary is reached. At flush time, it submits all accumulated vertices as a single glDrawArrays() call using client arrays, which feeds directly into GLdc's optimized array fast path.

Game code is completely unaffected. The batcher operates entirely underneath rlgl's public API. All Dreamcast-specific code is guarded behind #if defined(PLATFORM_DREAMCAST), so PC, web, and other console builds are not modified.

## 2.2 Vertex Format

The batcher uses a 24-byte interleaved vertex structure designed to match GLdc's fast-path requirements exactly:

| **Field** | **Type** | **Size** | **Notes** |
| --- | --- | --- | --- |
| x, y, z | float × 3 | 12 bytes | Position |
| u, v | float × 2 | 8 bytes | Texture coordinate |
| b, g, r, a | unsigned char × 4 | 4 bytes | Color in BGRA order |

Total: 24 bytes per vertex. The BGRA byte ordering matches GLdc's internal representation (B8IDX=0, G8IDX=1, R8IDX=2, A8IDX=3) and is submitted via glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, stride, ptr), which GLdc accepts as a valid color format for fast-path eligibility.

## 2.3 Buffer Configuration

The batch buffer holds up to 4080 vertices (configurable via RLDC_BATCH_CAPACITY). This number is chosen to be divisible by 12 (the LCM of 3 and 4), ensuring that a capacity-triggered flush never splits an incomplete triangle or quad. At 24 bytes per vertex, the buffer occupies approximately 95 KB of RAM.

An additional overflow margin of 120 vertices is allocated beyond the capacity. This handles the edge case where a single rlBegin/rlEnd block produces more vertices than the clean capacity. The clean-boundary flush (at exactly 4080) fires in rlDcBegin(), between primitives, guaranteeing correctness. The hard overflow limit fires in rlDcAppendVertex() as a safety net.

## 2.4 Capture and Deferral

**rlDcBegin(mode)** — Called from rlBegin(). If the mode is RL_QUADS or RL_TRIANGLES, the batcher captures it: sets the active flag, checks for mode changes (flush if different), checks capacity (flush if at limit), and returns 1 to signal that the original glBegin() should be skipped. For RL_LINES or other modes, the batcher flushes any pending geometry and returns 0, letting the call fall through to the original GL path.

**rlDcAppendVertex(x, y, z)** — Called from rlVertex2f/rlVertex3f when capture is active. Writes position, current UV (curU, curV), and current color (curR, curG, curB, curA) into the next slot of the batch buffer. Current UV and color are updated by rlTexCoord2f and rlColor4ub interceptors.

**rlDcEnd()** — Called from rlEnd() when capture is active. Does nothing. This is the key insight: by not flushing at rlEnd(), the batch stays open across consecutive compatible draws. The next rlBegin() either continues accumulating (same mode and texture) or triggers a flush (incompatible state).

## 2.5 Flush Mechanics

rlDcFlushBatch() submits all accumulated vertices to GLdc as a single draw call. It:

- Binds the batch texture (with redundant-bind elimination via lastFlushedTexId tracking)

- Enables GL_VERTEX_ARRAY, GL_TEXTURE_COORD_ARRAY, and GL_COLOR_ARRAY client states

- Disables GL_NORMAL_ARRAY (normals are not needed for 2D work and skipping them reduces GLdc's per-vertex copy cost)

- Sets up interleaved vertex/texcoord/color pointers with stride = 24

- Calls glDrawArrays(GL_QUADS or GL_TRIANGLES, 0, count)

- Disables client states and resets the vertex count to 0

The result: 200 DrawTexture() calls that would have produced 200 separate submitVertices() calls now produce 1 submitVertices() call with 800 vertices. The per-call overhead is eliminated, and the vertex processing benefits from GLdc's array fast path with better cache behavior.

# 3. Patch D: Deferred Texture Unbinding

## 3.1 The Redundant Bind/Unbind Problem

The standard raylib helper pattern ends with rlSetTexture(0) to restore the untextured state. When the next helper immediately calls rlSetTexture(sameId), the sequence becomes: bind(id) → draw → unbind(0) → bind(id) → draw → unbind(0). Each unbind and rebind dirties GPU state in GLdc and can force a poly-header rebuild. For a sequence of same-texture draws, this produces 2N state-dirty events for N draws, all of which are semantically unnecessary.

## 3.2 The Deferred Unbind Solution

**rlDcSetTexture(0)** — Instead of immediately unbinding, sets a pendingUnbind flag and returns. The texture remains logically bound in the batcher.

**rlDcSetTexture(id) where id == current batch texture** — If a pending unbind exists and the new texture matches the current batch, the unbind is cancelled entirely. The batch continues uninterrupted. A stat counter (statCancelledUnbinds) tracks how many redundant unbinds were avoided.

**rlDcSetTexture(id) where id != current batch texture** — The pending unbind is cleared, the current batch is flushed, and the new texture becomes the active batch texture.

**Pending unbind resolution** — If the pending unbind is still active at frame end, non-captured mode entry, or explicit flush, it resolves by actually unbinding (glDisable(GL_TEXTURE_2D) + glBindTexture(0)) and invalidating the lastFlushedTexId tracker.

In practice, for a typical 2D scene where most draws use the same sprite atlas texture, deferred unbinding eliminates 80–95% of texture state-dirty events.

# 4. Flush Trigger Architecture

The batcher must flush before any operation that would change the GL state under which the buffered geometry should be rendered. Missing a flush trigger causes geometry to be submitted with incorrect state (wrong texture, wrong depth mode, wrong blend), producing visual corruption. The following categories of operations have flush triggers:

## 4.1 Matrix Operations

GLdc loads the current transform matrix once per submitVertices() call. If the matrix changes between two buffered draws, both would be rendered with the second matrix. Affected functions: rlPushMatrix, rlPopMatrix, rlLoadIdentity, rlTranslatef, rlRotatef, rlScalef, rlMultMatrixf, rlFrustum, rlOrtho. All call rlDcFlushOnMatrixChange(), which flushes only if the buffer is non-empty.

## 4.2 GL State Changes

GLdc's apply_poly_header() builds the PVR polygon context from current GL state. Any state change between buffered draws would cause earlier geometry to render with later state. Affected functions: rlEnableColorBlend, rlDisableColorBlend, rlEnableDepthTest, rlDisableDepthTest, rlEnableDepthMask, rlDisableDepthMask, rlEnableBackfaceCulling, rlDisableBackfaceCulling, rlColorMask, rlSetCullFace, rlEnableScissorTest, rlDisableScissorTest, rlScissor. All call rlDcFlushOnStateChange(), which flushes and invalidates the lastFlushedTexId (since GL state context has changed).

## 4.3 Texture Operations (Non-Batcher Path)

rlEnableTexture() and rlDisableTexture() can be called directly, bypassing rlSetTexture() and the batcher's deferred unbind logic. Both have flush triggers that flush the batch, update the batcher's textureId tracking, and invalidate lastFlushedTexValid to prevent stale texture state.

## 4.4 Direct Draw Calls

rlDrawVertexArray() and rlDrawVertexArrayElements() go directly to glDrawArrays/glDrawElements without passing through the batcher. Without a flush trigger, buffered geometry would be submitted after the direct draw, breaking z-order. Both functions call rlDcFlushOnStateChange() before their GL draw call.

## 4.5 Frame Boundaries

rlDrawRenderBatchActive() is called by EndDrawing() at the end of each frame. It calls rlDcFlushAll(), which flushes any remaining geometry and resolves any pending texture unbind. rlClearScreenBuffers() also calls rlDcFlushAll() defensively, ensuring no geometry is lost before a screen clear.

# 5. rlgl Batcher Statistics

When compiled with -DRLDC_ENABLE_STATS, the batcher tracks per-frame counters:

| **Counter** | **Meaning** |
| --- | --- |
| statFlushes | Total batch flushes (each = one glDrawArrays call) |
| statFlushTexChange | Flushes caused by incompatible texture change |
| statFlushModeChange | Flushes caused by draw mode change (quads ↔ triangles) |
| statFlushMatrixChange | Flushes caused by matrix operations |
| statFlushStateChange | Flushes caused by GL state changes (blend, depth, etc.) |
| statFlushCapacity | Flushes caused by buffer reaching capacity |
| statFlushExplicit | Flushes at frame end or screen clear |
| statTotalVertices | Total vertices submitted through the batcher |
| statCancelledUnbinds | Redundant rlSetTexture(0) calls that were cancelled |

Access via rlDcPrintStats() and rlDcResetStats(). When stats are disabled, both functions and all counter macros compile to no-ops.

# 6. Files and Integration

**rlgl_dc_batch.h** — Self-contained header file (~280 lines) containing the complete batcher implementation: data structures, vertex append, flush, deferred unbind, begin/end capture, matrix/state flush helpers, and optional stats. Included from rlgl.h after the GL 1.1 header include, guarded by PLATFORM_DREAMCAST.

**rlgl.h modifications** — Applied via patch_rlgl_v5.py, which performs 11 targeted string replacements in the RLGL_IMPLEMENTATION section. All modifications are additive (inserting #if defined(PLATFORM_DREAMCAST) blocks) and do not alter any non-Dreamcast code paths. The patcher creates a .bak backup and reports exactly which patches applied.

**Compile flags** — PLATFORM_DREAMCAST: enables the batcher (required, already set by the DC build). RLDC_ENABLE_STATS: enables batcher stats counters (optional, zero overhead when omitted). RLDC_BATCH_CAPACITY: overrides the default 4080-vertex buffer size (optional).