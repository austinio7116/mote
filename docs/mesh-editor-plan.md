# Plan: Blender-style mesh modeling in Mote Studio

> **Status (2026-06-28):** Phases 1–2 **done**.
> - **Phase 1 (Foundations):** `EObject` editable scene, edge derivation with face
>   adjacency, cube/plane primitives, edit-mode toggle (Tab / "Model editor" button),
>   filled-face + wireframe + vertex rendering, `.mmesh` save/load, exact
>   (non-decimated) single-object bake. Added alongside the importer behind a
>   non-destructive toggle.
> - **Phase 2 (Selection):** VERT/EDGE/FACE modes (keys 1/2/3, pills) with Blender
>   mode-conversion; click-pick (frontmost wins), Shift-toggle, box-select (B then
>   drag), select-all / deselect-all (A / Alt+A), hover highlight. Mouse scheme:
>   LMB-on-geometry selects, LMB-on-empty-bg **or** MMB orbits, wheel zooms. Auto-
>   rotate removed in edit mode (model holds still unless orbiting).
>
> - **Phase 3 (Modal core):** Grab (**G**) + Scale (**S**) modal operators — move
>   the mouse to transform live, **X/Y/Z** axis constraints, type a number for an
>   exact value, **Enter/LMB** confirm, **Esc/RMB** cancel, Blender-style header
>   readout (`Move Y: 0.70`). Plus a click-drag **3-axis gizmo** at the selection
>   centroid (per the artist's pick — both keyboard *and* handles). Snapshot
>   **undo** stack (whole-scene deep copy, **Ctrl+Z**).
>
> - **Phase 4 (Topology ops):** Extrude (**E**) and Inset (**I**) on selected faces
>   (Face mode), across all objects. Extrude duplicates the region's verts, bridges
>   boundary edges (used by exactly one selected face) with side quads, re-points the
>   selected faces onto the lid, and starts a move default-constrained to the averaged
>   face normal (press X/Y/Z to free it). Inset builds a shrunk inner copy per face +
>   a ring of bridging quads, amount driven by the mouse. Both rebuild edges; the
>   baker recomputes normals from winding so no explicit recalc is needed. Card
>   buttons + keys; verified bake-compiles after extrude (8v6f → 12v10f).
>
> Phases 5–6 below remain (multi-object/rig-join, polish — more primitives,
> duplicate/delete/merge, per-face colour paint).

## Goal
Add vertex/edge/face select modes and modal operators — **G**rab, **S**cale,
**E**xtrude, **I**nset — with **X/Y/Z** axis constraints and numeric entry,
working identically in the MESH (and RIG) tabs, plus the ability to create
separate objects and join them into a rig.

## Key findings from the codebase
- **Studio is pure C + SDL2** with a hand-rolled software rasterizer
  (`draw_mesh` / `draw_rig` in `studio/main.c`). No ImGui, no GPU.
- The **MESH tab today is an importer/decimator/baker** — it loads OBJ/STL into
  a triangle soup (`g_raw`, `studio/main.c:1117`), vertex-cluster decimates to a
  budget (`mesh_reprocess`, `:1184`), previews, and bakes a quantized header
  (`mesh_emit`/`mesh_bake`, `:1151`/`:1208`). There is **no editable topology** —
  no persistent vertices/edges/faces you can select and operate on.
- The **RIG tab already has half of what we want**: multi-object parts
  (`RigPart`), parent hierarchy, per-part pivots, a 3-axis manipulator gizmo,
  keyframes, and bake to `anim3d.h` (`studio/main.c:1404-1774`).
- The **runtime mesh & rig formats already exist**: `Mesh`/`MoteModel`
  (`engine/assets/mote_mesh.h`, ABI v35 textured meshes), `MoteRig`/
  `MoteModelClip` (`sdk/mote_anim3d.h`). **This feature needs zero firmware/ABI
  change** — it is entirely a Studio tooling addition.
- Baked mesh format: quantized `int8` verts + `uint8`-indexed faces, chunked to
  ≤255 verts, per-face normals, optional per-face colors, optional texture+UVs.

## Core architectural idea: one shared editable scene
Today the mesh tab and rig tab load geometry independently (`g_raw` vs `g_rp[]`).
Unify them around a single in-memory **editable scene of objects**. The MESH tab
edits object *geometry*; the RIG tab assigns *hierarchy + animation* to those
same objects. "Create separate objects then join with the rig" falls out
naturally: model objects in MESH, switch to RIG, set each object's parent +
pivot, animate, bake.

## Data model (new, in `studio/main.c`)
```c
typedef struct { V3 p; uint8_t sel; } EVert;
typedef struct { int v[4]; int nv;   uint8_t sel; uint16_t color; } EFace;  // tri or quad
typedef struct { int a,b; uint8_t sel; int f0,f1; } EEdge;                   // derived; f0/f1 = adjacent faces
typedef struct {
    char  name[28];
    EVert *v; int nv,vcap;
    EFace *f; int nf,fcap;
    EEdge *e; int ne;            // rebuilt by edges_rebuild() after any topology change
    V3    origin;                // object origin
    int   parent;  V3 pivot;     // rig fields (consumed by RIG tab)
} EObject;
static EObject g_obj[MAX_OBJ]; static int g_nobj, g_objsel;
```
- **Edges are derived** from faces (`edges_rebuild()` after every topology
  change), with adjacency so extrude can find boundary loops.
- Faces support **tris and quads** (good low-poly modeling); a triangulation
  pass feeds the baker.
- Selection lives on verts/faces/edges; the active **select mode**
  (VERT/EDGE/FACE) decides which set is authoritative, with Blender-style
  conversion when you switch modes (e.g. an edge is "selected" iff both its
  verts are).

## The modal operator engine (must feel exactly like Blender)
A single FSM, polled in the SDL event loop when MESH/RIG is in edit mode:
```c
enum { OP_NONE, OP_MOVE, OP_SCALE, OP_EXTRUDE, OP_INSET };
static struct {
    int   op, axis;          // axis: 0=free 1=X 2=Y 3=Z
    V3   *backup; int nbk;    // snapshot of all vert positions at op start
    V3    center;             // selection centroid (scale pivot / readout origin)
    int   ax, ay;             // mouse anchor
    char  num[16]; int hasnum;// numeric entry buffer
} g_op;
```
Flow, matching Blender 1:1:
1. Press **G/S/E/I** with a non-empty selection → snapshot all positions, compute
   selection centroid; for E/I first build the new geometry (below); grab mouse.
2. **Mouse motion** drives the op live (translate / scale-factor /
   extrude-distance / inset-amount).
3. Press **X/Y/Z** → toggle that axis constraint (re-applies from the backup each
   time, so constraints are non-destructive).
4. **Type digits / `.` / `-`** → exact numeric value overrides the mouse.
5. **Confirm** = LMB or Enter; **Cancel** = RMB or Esc (Esc/RMB also deletes
   geometry created by E/I).
6. Header readout mirrors Blender: `Move X: 0.42`, `Scale: 1.30`,
   `Extrude Z: 0.50`, `Inset: 0.10`.

Screen→world delta uses the existing yaw/pitch/perspective basis already computed
in `draw_mesh` (camera-right/up scaled by world-per-pixel at the selection
depth). Axis-constrained motion projects onto the chosen world axis.

## The four operators
- **Move (G):** add world delta to selected verts. The reference op for the FSM.
- **Scale (S):** factor about the centroid; per-axis when constrained.
- **Extrude (E):** *face mode* — duplicate the selected region's verts, bridge
  boundary edges (edges used by exactly one selected face) with side quads,
  re-point the original faces to the new verts, select the new faces,
  default-constrain motion to the averaged face normal. *Edge mode* — new edge +
  connecting quad. *Vert mode* — new vert + edge. Most topology-heavy; needs
  `edges_rebuild()` + normal recompute after.
- **Inset (I):** *per-face* first (Blender's `I,I`): inner shrunk copy per face +
  ring of bridging quads, amount = mouse distance along the face plane. *Region
  inset* (shared boundary) as a follow-up.

## Mesh creation
**Shift+A** primitive menu → cube / plane / cylinder / cone / UV-sphere
generators, each creating a new `EObject`. Plus **Shift+D** duplicate, **X**
delete menu (verts/edges/faces), **M** merge.

## Rendering changes (extend `draw_mesh`)
Add an edit viewport on top of the existing software rasterizer: filled faces
(active object lit, others dimmed), **wireframe edge overlay**, **vertex dots**,
selection highlight colors (selected verts/edges/faces in orange), pivot/centroid
marker, and the modal header text. Reuse the RIG tab's gizmo drawing for the
pivot indicator.

## Input / camera changes
Resolve the LMB conflict (LMB currently orbits): in **edit mode** LMB =
select / box-select, **MMB (or Alt+LMB)** = orbit, wheel = zoom — Blender's
scheme. Route `SDL_KEYDOWN` to a new `mesh_edit_key()` when MESH/RIG is in edit
mode, and enable `SDL_TEXTINPUT` capture during a modal op for numeric entry.

## Persistence & baking
- **Native editor format** (`.mmesh`, text/JSON next to the project): stores
  objects with verts/faces/colors/origins/parent/pivot. Needed because the
  current OBJ loader triangulates and would destroy quads + topology.
- **Bake — exact path:** emit the edited topology *directly* to
  `MeshVert`/`MeshFace` (triangulating quads, chunking to ≤255 verts),
  **bypassing the decimator** so the artist's exact low-poly mesh and per-face
  colors survive. Reuse the chunk-packing loop from `mesh_emit`. (Keep the
  existing decimate-import path for STL/photogrammetry.)
- **Bake — rig path:** for multi-object scenes, emit a `MoteRig` header (per-part
  `Mesh` chunks + parent/pivot) straight from the `EObject` list, reusing
  `obj2rig` logic in-memory.

## Rig join
Make the `EObject` list the shared model the RIG tab consumes as parts
(replacing the current `rig_load`-into-`RigPart[]`). Then the rig tab's existing
parent-cycling, pivot steppers, centroid button, gizmo, keyframes and `anim3d`
bake all work on hand-modeled objects unchanged.

## Suggested phasing
1. **Foundations** — `EObject` model, edge derivation, cube/plane primitives,
   edit-mode toggle, wireframe+vertex rendering, `.mmesh` save/load, exact
   single-object bake.
2. **Selection** — VERT/EDGE/FACE modes (keys 1/2/3), click-pick + box-select
   (B), select-all (A), mode-conversion, hover highlight.
3. **Modal core** — G + S with X/Y/Z constraints, numeric entry, confirm/cancel,
   header readout, **undo stack** (snapshot-based; meshes are small). This is
   where the "feel" is proven.
4. **Topology ops** — E (vert/edge/face) and I (per-face → region), with
   edge/normal rebuild.
5. **Multi-object + rig join** — object list UI, per-object origin, unify rig tab
   onto `EObject`, bake `MoteRig`, keep the animation editor working.
6. **Polish** — more primitives, duplicate/delete/merge, recalc normals,
   snapping, per-face color paint.

## Risks / decisions to make up front
- **OBJ can't round-trip quads/topology** → hence the native `.mmesh` format
  (decision: adopt it).
- **Exact bake vs decimator** → add a dedicated exact emitter rather than reusing
  the clustering path.
- **LMB select vs orbit** → adopt Blender's MMB-orbit (needs SDL middle-button
  handling).
- **Undo** is essential for usability — fold into Phase 3, not "polish."
- **Picking accuracy** — pick against per-frame projected vertex/edge screen
  positions with depth disambiguation; reuse the existing projection.

## Notes
- No firmware/ABI change required; all work is in `studio/main.c` plus the tool
  bakers (`tools/obj2mesh.c`, `tools/stl2mesh.c`, `tools/obj2rig.c`).
- Reuses the rig tab's animation machinery unchanged.
