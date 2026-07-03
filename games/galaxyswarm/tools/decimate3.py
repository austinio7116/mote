# Blender 3.0 headless: island-cull -> strip UVs -> decimate hard -> re-project UVs
# from the original surface (Data Transfer POLYINTERP_NEAREST). For meshes whose
# UV seams block ordinary collapse decimation.
# usage: blender -b -P decimate3.py -- in.obj out.obj target_verts [max_islands] [nouv]
import bpy, sys

argv = sys.argv[sys.argv.index('--') + 1:]
src, dst, target = argv[0], argv[1], int(argv[2])
max_islands = int(argv[3]) if len(argv) > 3 else 25
want_uv = not (len(argv) > 4 and argv[4] == 'nouv')

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.obj(filepath=src)
meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
bpy.ops.object.select_all(action='DESELECT')
for o in meshes:
    o.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
if len(meshes) > 1:
    bpy.ops.object.join()
obj = bpy.context.view_layer.objects.active
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

def normalize(o):
    vs = o.data.vertices
    mins = [min(v.co[i] for v in vs) for i in range(3)]
    maxs = [max(v.co[i] for v in vs) for i in range(3)]
    ctr = [(mins[i] + maxs[i]) / 2 for i in range(3)]
    half = max((maxs[i] - mins[i]) / 2 for i in range(3)) or 1.0
    for v in vs:
        for i in range(3):
            v.co[i] = (v.co[i] - ctr[i]) / half

normalize(obj)
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.remove_doubles(threshold=0.0003)
bpy.ops.mesh.delete_loose(use_verts=True, use_edges=True, use_faces=False)
bpy.ops.object.mode_set(mode='OBJECT')

# island culling (see decimate2.py: small greebles impose a per-island vert floor)
bpy.ops.mesh.separate(type='LOOSE')
parts = [o for o in bpy.context.scene.objects if o.type == 'MESH']
def diag(o):
    vs = o.data.vertices
    if not vs: return 0.0
    mins = [min(v.co[i] for v in vs) for i in range(3)]
    maxs = [max(v.co[i] for v in vs) for i in range(3)]
    return sum((maxs[i] - mins[i]) ** 2 for i in range(3)) ** 0.5
parts.sort(key=diag, reverse=True)
for o in parts[max_islands:]:
    bpy.data.objects.remove(o, do_unlink=True)
keep = parts[:max_islands]
bpy.ops.object.select_all(action='DESELECT')
for o in keep:
    o.select_set(True)
bpy.context.view_layer.objects.active = keep[0]
if len(keep) > 1:
    bpy.ops.object.join()
obj = bpy.context.view_layer.objects.active
normalize(obj)

# keep a UV-bearing copy of the surface to transfer back from
ref = None
if want_uv and obj.data.uv_layers:
    ref = obj.copy()
    ref.data = obj.data.copy()
    bpy.context.collection.objects.link(ref)

# strip UVs so seams don't pin every edge, then decimate freely
while obj.data.uv_layers:
    obj.data.uv_layers.remove(obj.data.uv_layers[0])
passes = 0
while len(obj.data.vertices) > target and passes < 15:
    ratio = max(0.05, min(0.95, target / len(obj.data.vertices)))
    m = obj.modifiers.new('dec', 'DECIMATE')
    m.ratio = ratio
    m.use_collapse_triangulate = True
    bpy.ops.object.modifier_apply(modifier=m.name)
    passes += 1

m = obj.modifiers.new('tri', 'TRIANGULATE')
bpy.ops.object.modifier_apply(modifier=m.name)

if ref is not None:
    obj.data.uv_layers.new(name='UVMap')
    dt = obj.modifiers.new('uvx', 'DATA_TRANSFER')
    dt.object = ref
    dt.use_loop_data = True
    dt.data_types_loops = {'UV'}
    dt.loop_mapping = 'POLYINTERP_NEAREST'
    bpy.ops.object.modifier_apply(modifier=dt.name)
    bpy.data.objects.remove(ref, do_unlink=True)

bpy.ops.object.select_all(action='DESELECT')
obj.select_set(True)
bpy.ops.export_scene.obj(filepath=dst, use_selection=True, use_materials=False,
                         use_normals=False, use_uvs=want_uv, use_triangles=True)
print('DECIMATED3 %s -> %s: %d verts, %d faces (%d passes)'
      % (src, dst, len(obj.data.vertices), len(obj.data.polygons), passes))
