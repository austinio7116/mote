# Mote 3D animation — how it works, and why it's header-only

3D model animation in Mote is **rigid-part (hierarchical) skeletal animation**, and it
lives entirely in a header (`sdk/mote_anim3d.h`) — no engine ABI, no firmware change.
This note explains what that means *for a developer*, then why it's the right design.

## What it looks like for development (the part that matters)

You never hand-write transforms. The workflow is:

1. **Model in parts.** Author your model as a multi-object OBJ — one object per moving
   part (`body`, `turret`, `barrel`, …) — plus a tiny `.rig` sidecar giving each part a
   parent and a pivot (the joint it rotates about). The Studio **Rig tab** edits the
   pivots/hierarchy with a 3-axis manipulator; `obj2rig` bakes it to a `MoteRig`.

2. **Animate on a timeline.** In the Rig tab you scrub a timeline, pose parts (drag the
   rotate rings / translate handles, or type values), and drop keyframes. Each keyframe is
   **linear** (smooth ease into the next key) or **snap** (hold this pose, then jump at the
   next key) — toggle it with the **key: linear / key: snap** pill; snap keys show as squares
   on the timeline, linear as diamonds. **Bake anim3d** writes a `<name>.anim3d.h` — a
   `const MoteModelClip` (per-part rotation + translation keyframe tracks) that lives in
   flash and costs no RAM.

   The manipulator gizmo aligns to each part's **parent frame**, so dragging a handle always
   moves/rotates the part the way the handle points — even for parts that hang off a rotated
   parent. Your keyframes are saved to an editable `<model>.anim` sidecar (next to the model)
   on Save/Bake and when you leave the tab, and reload when you reopen — so a clip survives
   tab switches and reopens. One clip per model: to ship several animations for one rig (walk,
   fire, idle…), bake each and keep the generated `.anim3d.h` headers, or hand-author extra
   `MoteModelClip`s over the same `MoteRig` (as the tanks example does for its recoil clip).

3. **Play it — ~3 lines.** Include the baked header and drive a player:

   ```c
   #include "mech.anim3d.h"          // MoteRig mech_rig; clips mech_walk, mech_fire…
   static MoteModelPlayer p;
   mote_rig_play(&p, &mech_walk);    // start (on spawn, or on a game event)
   // each frame, after scene_camera():
   mote_rig_tick(&p, dt);
   mote_rig_draw(mote, &mech_rig, &p, world_pos);
   ```

That's the whole API surface for the common case. **Game events trigger clips** by just
calling `mote_rig_play` when the event happens (a `MOTE_ANIM_ONCE` clip sets `done` when
it finishes) — e.g. the tanks example fires a baked barrel-recoil clip on each shot.

### Mixing baked clips with procedural motion

The runtime works in **per-part locals**, which is what makes this pleasant. Instead of
`mote_rig_draw`, evaluate the clip into a local-pose array, override the parts you want
to drive from game state, then compose:

```c
MoteRigLocal loc[P_COUNT];
mote_rig_eval(&tank_rig, &player, loc);                       // clip -> per-part locals
loc[P_TURRET].rot = mote_quat_axis(v3(0,1,0), aim);           // override turret from input
mote_rig_draw_locals(mote, &tank_rig, loc, pos, body, scale); // compose parent x local
```

A clip only "owns" the parts it has tracks for; everything else stays yours. Because
composition is **parent × local**, an overridden parent (the turret) automatically
carries its children (the barrel) — so a recoil clip on the barrel kicks straight back
along the *aimed* gun axis with no extra work. Author canned motion in the IDE, drive the
rest from code, on the same rig.

## Why header-only (no engine support) is the right design

Mote splits work in two:

- **Stateful / hardware subsystems go through the engine ABI** — the display DMA, the
  4-channel audio mixer, the physics solver, USB link, saves. They own hardware or a
  persistent loop, so the engine must run them.
- **Pure math + data are header-only** — `mote_vec.h` (Vec3/Mat3), the mesh/rig formats,
  easing curves, and this animation runtime. They compile *into the game* and need
  nothing from the engine at runtime.

Rigid-part animation is the second kind. Per frame it (a) **composes each part's
transform** — parent × local from a quaternion + translation, i.e. a handful of 3×3
matrix multiplies — and (b) **draws each part** through the engine's existing
`scene_add_object` path (depth-buffered, dual-core rastered). There is no hardware to own
and no persistent state the engine could manage better. A baked clip is **const flash
data**, exactly like a mesh or a texture; "no engine support" isn't a gap — the engine's
existing draw path is all that's needed.

What this buys the developer:

- **Flash-resident, zero-RAM** clips (const data in flash).
- **Full flexibility** — blend, retarget, IK, or mix procedural + baked, all in plain C,
  with no fixed engine animation model to fight.
- **No firmware dependency** — ship a game with new animation against any firmware that
  has the (stable) draw ABI; nothing to reflash.

**The honest tradeoff:** the game ticks the player itself (one line) and submits one draw
per part. That's the cost of not having an engine-side player, and it's negligible — the
triangle budget is the limit, not call count.

**When we'd reconsider:** *skinned* animation (per-vertex bone weights) is the one case
that genuinely wants engine support, because it's a per-vertex transform the rasteriser
would have to do. Mote deliberately chose **rigid parts** (no skinning) precisely so
animation stays header-only and fits the RP2350. If skinning ever becomes worth it,
*that's* when an engine ABI would be added — not for rigid-part animation.
