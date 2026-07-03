# voxel remesh -> decimate -> UV transfer from original. For soup meshes.
# usage: blender -b -P remesh.py -- in.obj out.obj target_verts voxel
import bpy, sys
argv = sys.argv[sys.argv.index('--')+1:]
src, dst, target, vox = argv[0], argv[1], int(argv[2]), float(argv[3])
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.obj(filepath=src)
meshes=[o for o in bpy.context.scene.objects if o.type=='MESH']
for o in meshes: o.select_set(True)
bpy.context.view_layer.objects.active=meshes[0]
if len(meshes)>1: bpy.ops.object.join()
obj=bpy.context.view_layer.objects.active
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
vs=obj.data.vertices
mins=[min(v.co[i] for v in vs) for i in range(3)]
maxs=[max(v.co[i] for v in vs) for i in range(3)]
ctr=[(mins[i]+maxs[i])/2 for i in range(3)]
half=max((maxs[i]-mins[i])/2 for i in range(3)) or 1.0
for v in vs:
    for i in range(3): v.co[i]=(v.co[i]-ctr[i])/half
ref=obj.copy(); ref.data=obj.data.copy()
bpy.context.collection.objects.link(ref)
obj.data.remesh_voxel_size=vox
bpy.ops.object.voxel_remesh()
print("REMESHED", len(obj.data.vertices), len(obj.data.polygons))
passes=0
while len(obj.data.vertices)>target and passes<15:
    m=obj.modifiers.new('d','DECIMATE'); m.ratio=max(0.05,min(0.95,target/len(obj.data.vertices))); m.use_collapse_triangulate=True
    bpy.ops.object.modifier_apply(modifier=m.name)
    passes+=1
m=obj.modifiers.new('t','TRIANGULATE'); bpy.ops.object.modifier_apply(modifier=m.name)
if ref.data.uv_layers:
    obj.data.uv_layers.new(name='UVMap')
    dt=obj.modifiers.new('uvx','DATA_TRANSFER'); dt.object=ref
    dt.use_loop_data=True; dt.data_types_loops={'UV'}; dt.loop_mapping='POLYINTERP_NEAREST'
    bpy.ops.object.modifier_apply(modifier=dt.name)
bpy.data.objects.remove(ref, do_unlink=True)
bpy.ops.object.select_all(action='DESELECT'); obj.select_set(True)
bpy.ops.export_scene.obj(filepath=dst, use_selection=True, use_materials=False,
                         use_normals=False, use_uvs=True, use_triangles=True)
print('REMESH-DEC %s: %d verts, %d faces'%(dst,len(obj.data.vertices),len(obj.data.polygons)))
