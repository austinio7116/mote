# Changelog

## 0.3-alpha

This release updates the device firmware — reflash `firmware_mote_os.uf2`, and rebuild
your games so they pick up the smaller launcher icons.

### New and improved

- **3D model animation.** Models can now be rigged into moving parts and animated: build
  a rig, pose it on a keyframe timeline, bake a clip, and trigger it from game events
  (e.g. a gun firing) — all authored in the Studio Rig tab. See the new "Creating rigs
  and animations" guide.
- **Rig editor.** A 3-axis on-model manipulator to move/rotate parts and place pivots, a
  scrubbable keyframe timeline with draggable keys, and ROTATE/MOVE inputs. "View as mesh"
  opens a rigged model in the Mesh tab.
- **Tanks example.** Rebuilt with detailed 3D tanks (tracks + road wheels, sloped hull,
  rounded turret, muzzle), team colours with dark tracks/gun, and a barrel that recoils
  via a baked animation clip on every shot.
- **Smaller launcher icons.** Icons are compressed in flash — roughly half the space
  (much less for simple ones), with no visible change.
- **Better Open Project screen.** Each game shows its icon, an estimated memory bar, and
  a proper scrollbar.
- **Edit the game icon in the IDE.** Draw or import the launcher icon right in the Pixel
  Art editor (Assets ▸ Edit Icon) — it bakes automatically.
- **Pixel/Texture sizes.** More canvas sizes (including 60 for icons) plus a −/+ for any
  size, keeping your art when you resize.
- **Optional frame-rate cap** a game can set, honoured by both the device and the emulator.
- **File manager in the tree.** Right-click to New File / New Folder / Rename / Delete
  (with a confirm), and double-click a folder to collapse/expand it. The tree shows
  subfolders; clicking a `.sfx` opens the Audio tab and a `.rig` the Rig tab.
- **Mousewheel zoom** in the Mesh and Rig 3D previews.
- **Consistent naming dialog** for New/Rename/Save As (click-to-select-all), and the file
  picker opens in the current project's folder.
- **Windows fast-update** dev workflow (`scripts/sync-windows.sh`) — update an unzipped
  bundle in place instead of re-unzipping.

### Fixed

- The Mesh/Rig 3D preview is sharp again, no longer squashes on wide windows, and the
  left/right orbit drag is no longer inverted; the rig view no longer auto-spins.
- The rig editor is editable the moment a model loads (a rest keyframe is seeded), so
  pose values and the manipulator work right away.
- "VS Code", and "Reveal in Files" / "Open Folder", now work on Windows.
- Launcher icons are handled by the build — a game no longer needs to include anything,
  so they can't go missing (the tanks icon was hitting this).
- Assets placed in subfolders are now built too.

## 0.2-alpha

This release updates the device firmware — reflash `firmware_mote_os.uf2`, and
rebuild your games so they pick up the new per-game icons.

### New and improved

- Each game now carries its own launcher icon, so adding a game no longer means updating the firmware.
- The device holds and shows many more games — up to 56, was 24.
- Creating a game gives you a wizard: pick a starter template (3D, physics, or 2D) with sensible memory settings already filled in.
- The **fling** example is now a full game — procedurally generated levels, large rolling terrain, and taller, more varied forts.
- Animation files are much smaller: only the frames a clip actually uses are saved.
- More games have proper icons, taken from a screenshot of the game.
- Mote Studio shows its version under Help ▸ About.

### Fixed

- Switching between projects now refreshes the Tiles, Anim, and Mesh tabs.
- Listing games on the device no longer cuts off when you have a lot of them.
- **fling** forts no longer fall over before you take a shot, and the aim dots are now easy to see against the sky.

## 0.1-alpha

First public alpha: the native C engine, console OS, and Studio IDE for the Thumby Color.
