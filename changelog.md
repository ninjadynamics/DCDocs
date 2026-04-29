# Changes Since Patches A+D (rlgl Batcher)

What was implemented in the raylib `experimental` branch and the `dcmesh` converter repo after the immediate-mode batcher was in place.

---

## dcmesh Converter (new repo: `ninjadynamics/dcmesh`)

A PC-side offline tool that converts `.glb` files into `.dcmesh` sidecar files optimized for Dreamcast strip rendering.

**What it does:** Loads a glTF binary via cgltf, extracts position/UV/color per primitive, looks up the scene-graph node that owns each mesh and applies its world transform to positions (matching raylib's `LoadModel` behavior), optimizes index order with `meshopt_optimizeVertexCacheStrip`, stripifies with `meshopt_stripify` using `~0u` as restart index, then pre-expands strip vertices into a de-indexed 24-byte interleaved format (3 floats position, 2 floats UV, 1 uint32 BGRA color). Writes a binary `.dcmesh` file.

**Format (v2):** File header (`DCM1` magic, version, counts) followed by per-submesh blocks. Each block has a submesh header (material index, vertex count, strip count, opacity flag), then the vertex array, strip descriptors (`first_vertex` + `vertex_count`), and a `vertex_map` array (`uint16_t` per strip vertex → source vertex index). The vertex map was added in v2 to enable runtime sync of positions and colors from raylib mesh arrays back into strip vertices.

**Node transforms:** The converter walks `cgltf_node_transform_world()` for each mesh's owning node and multiplies positions by the resulting column-major 4×4 matrix. Without this, dcmesh vertices would be in raw accessor space instead of the world-space positions raylib produces at load time.

---

## DCMesh Runtime (in raylib `experimental` branch)

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

`dc_mesh.c` added to `src/Makefile` and `src/CMakeLists.txt` for Dreamcast builds. Feature toggles `ENABLE_STRIPS` and `ENABLE_PATCH_E` both default to 1 when `PLATFORM_DREAMCAST` is defined. Setting either to 0 at compile time disables the corresponding path.
