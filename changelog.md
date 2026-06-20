# Current Changelog and Fork State

What was implemented in the raylib-dc, GLdc, and dcmesh forks after the immediate-mode batcher was in place. For exact repository heads and cleanup notes, see [Current Fork Snapshot](fork_snapshot_2026_06.md).

---

## Repository Snapshot

| Repo | Baseline | Current analyzed head | Main delta |
| --- | --- | --- | --- |
| raylib-dc | `65cfa062e5207f0105b9b41f2ec17967c5299cd4` | `ba16cf013` | rlgl batcher, DCMesh runtime, raymath SH4ZAM paths, Dreamcast mipmap path, re-upload guard |
| GLdc | `ba10615722b21bd60f527676f0218d6adc6599fd` | `e99062a` | stats, PUC fast path, clipping/submission tuning, punch-through fix, RTT, deferred fog |
| dcmesh | `86bc187ed054b5835dac2772f495ff5b95f21da8` | `a6adf79` | format v2, vertex map, node transform baking, generation script updates |

---

## dcmesh Converter (new repo: `ninjadynamics/dcmesh`)

A PC-side offline tool that converts `.glb` files into `.dcmesh` sidecar files optimized for Dreamcast strip rendering.

**What it does:** Loads a glTF binary via cgltf, extracts position/UV/color per primitive, looks up the scene-graph node that owns each mesh and applies its world transform to positions (matching raylib's `LoadModel` behavior), optimizes index order with `meshopt_optimizeVertexCacheStrip`, stripifies with `meshopt_stripify` using `~0u` as restart index, then pre-expands strip vertices into a de-indexed 24-byte interleaved format (3 floats position, 2 floats UV, 1 uint32 BGRA color). Writes a binary `.dcmesh` file.

**Format (v2):** File header (`DCM1` magic, version, counts) followed by per-submesh blocks. Each block has a submesh header (material index, vertex count, strip count, opacity flag), then the vertex array, strip descriptors (`first_vertex` + `vertex_count`), and a `vertex_map` array (`uint16_t` per strip vertex → source vertex index). The vertex map was added in v2 to enable runtime sync of positions and colors from raylib mesh arrays back into strip vertices.

**Node transforms:** The converter walks `cgltf_node_transform_world()` for each mesh's owning node and multiplies positions by the resulting column-major 4×4 matrix. Without this, dcmesh vertices would be in raw accessor space instead of the world-space positions raylib produces at load time.

---

## DCMesh Runtime (in raylib `master` branch)

Three new files added to `raylib/src/`: `dcmesh.h`, `dc_mesh.h`, `dc_mesh.c`.

### Registry

A global table (`dc_registry`, max 64 entries) maps loaded `DCMeshData` pointers to integer indices. On Dreamcast GL 1.1, `Mesh.vaoId` is unused (VAOs are a GL 3.3+ concept), so it's repurposed to encode a registry link. The encoding packs a `0xDC` magic prefix, a 16-bit registry index, and an 8-bit submesh index into a single `unsigned int`. Macros (`DCMESH_IS_REGISTRY_ID`, `DCMESH_REGISTRY_INDEX`, `DCMESH_SUBMESH_INDEX`, `DCMESH_MAKE_ID`) handle packing and unpacking.

### Sidecar Loading

`dcMeshLoadSidecar(Model*, const char*)` derives a `.dcmesh` path from the model file path (replaces extension), reads and validates the file header, allocates runtime structures, reads each submesh's vertices/strips/vertex_map, registers the data, and writes `DCMESH_MAKE_ID(reg_idx, submesh_idx)` into each corresponding `model->meshes[i].vaoId`.

### Strip Rendering

`dcDrawSubmesh()` sets up client arrays once for an entire submesh's vertex buffer (stride = 24 bytes, matching the batcher's `RlDcBatchVertex` layout), pushes the model transform via `rlPushMatrix`/`rlMultMatrixf`, applies the material tint color, then loops over all strips calling `glDrawArrays(GL_TRIANGLE_STRIP, strip->first_vertex, strip->vertex_count)` per strip. GLdc's `genTriangleStrip()` just sets one EOL flag, so per-strip overhead is minimal.

Patch E eligibility is checked per submesh: must be marked opaque, must have a valid diffuse texture, and material color alpha must be 255. When eligible, all strips share one GL state context with no header rebuilds between them. Ineligible submeshes still render via strips but without the Patch E state guarantee.

### UploadMesh Hook

`dcMeshHandleUpload(Mesh*, bool)` is called from the patched `UploadMesh()` in `rmodels.c`. If the mesh has dcmesh data (detected via `DCMESH_IS_REGISTRY_ID` on `vaoId`), it syncs positions and colors from the raylib mesh arrays into the strip vertices using the `vertex_map`. Positions are copied directly (3 floats per source vertex). Colors are converted from RGBA to BGRA byte order during copy. After sync, it returns 1 to skip the normal `UploadMesh` body. This means game code that modifies mesh vertices (e.g. recentering geometry) and then calls `UploadMesh` will have those changes automatically propagated to the strip data.

A `dcMeshUploadSafe()` convenience wrapper temporarily clears `vaoId` before calling `UploadMesh` to avoid the "already loaded" warning, then restores it.

### Transparent Routing Hooks in rmodels.c

Four insertion points, all guarded by `#if defined(PLATFORM_DREAMCAST) && ENABLE_STRIPS`:

- **`LoadModel()`** — calls `dcMeshLoadSidecar(&model, fileName)` after the model is fully loaded. If a `.dcmesh` file exists next to the model, strip data is loaded and linked automatically.
- **`UnloadModel()`** — calls `dcMeshUnloadModel(&model)` before meshes are freed, cleaning up registry entries and strip data.
- **`UploadMesh()`** — calls `dcMeshHandleUpload(mesh, dynamic)` at the top. If it returns 1, the rest of `UploadMesh` is skipped (strip vertices are synced instead of uploading to GL).
- **`DrawMesh()`** — checks `dcMeshHasStripData(mesh)` and routes to `dcMeshDraw()` if true. Otherwise falls through to the standard GL 1.1 immediate-mode mesh drawing.

### Build Integration

`dc_mesh.c` added to `src/Makefile` and `src/CMakeLists.txt`. Feature toggles `ENABLE_STRIPS` and `ENABLE_PATCH_E` both default to 1 when `PLATFORM_DREAMCAST` is defined. Setting either to 0 at compile time disables the corresponding path.

---

## Additional raylib-dc Changes

### Dreamcast mipmap generation

`rlGenTextureMipmaps()` now has a Dreamcast branch that calls GLdc `glGenerateMipmap(GL_TEXTURE_2D)` for square power-of-two textures, then updates raylib's mipmap count. Non-square textures warn and skip generation, matching GLdc's constraints.

### Mesh re-upload guard

`UploadMesh()` now frees an existing `mesh->vboId` array before allocating a new one on GLdc/OpenGL 1.x paths where `vaoId` remains zero. This prevents repeated runtime uploads from leaking the bookkeeping array. Modern GL paths still return through the usual `vaoId > 0` guard before this path.

### SH4ZAM hooks

`raymath.h` includes optional `USE_SH4ZAM` paths for selected vector, matrix, and quaternion helpers. The most important correctness detail is the use of unaligned-safe matrix loaders for by-value `Matrix` parameters; plain aligned SH4ZAM loaders can fault on real hardware when the SH4 ABI provides only 4-byte alignment.

---

## GLdc Changes After the Original Patch Set

### Expanded stats

GLdc now has optional counters for draw calls, submit calls, fast-path hits/misses, headers, dirty-state events, transformed vertices, texture binds, immediate-mode traffic, clipping, scene-list submission, strip counts, and Patch-E hits/fallbacks.

### P+UV+color fast path

`GL/draw.c` adds specialized non-indexed quad/triangle generators for the position+UV+color layout. The generators skip normal and secondary-UV loops entirely, which is the layout emitted by the raylib-dc batcher and DCMesh runtime.

### Punch-through blend fix

Punch-through list headers now use source-alpha/inverse-source-alpha blending. This protects real hardware from visible opaque boxes when punch-through discard fails to kill a nominally transparent texel.

### Render-to-texture helpers

`glKosFlushToTexture()` renders queued GLdc lists into a KOS/PVR texture target, clears the lists, and lets a second pass composite overlays predictably. `glKosTextureData()` exposes the raw VRAM pointer for a GL texture id so it can be used as the target.

### Deferred fog helpers

`glKosQueueFogTableLinear()` and `glKosQueueFogTableFlat()` queue fog register writes until GLdc has waited for the previous render and is about to begin the next scene. This avoids changing global PVR fog registers while hardware may still be drawing.

### Texture conversion OOM guard

`glTexImage2D()` now checks the temporary conversion buffer allocation and throws `GL_OUT_OF_MEMORY` instead of writing through a null pointer.

---

## Known Upstream Cleanup

- Remove timestamp/canary log churn before proposing upstream changes.
- Resolve the DCMesh color-channel TODO in `dcMeshSyncFromRaylib()`.
- Clarify that local DCMesh "Patch E" means opaque-list routing, not full direct-PVR submission.
- Consider making `dc_mesh.o` Dreamcast-only in generic build files.
- Validate `vertex_map` source indices before narrowing to `uint16_t`.
