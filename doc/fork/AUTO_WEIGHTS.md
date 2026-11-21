# Auto Weights

<img src="images/icons/mod_auto_weights.png" width="32" height="32" alt="Auto Weights">

Gives each deform bone of an armature its own vertex group, the way parenting with automatic
weights does, but as a modifier. The weights are worked out once, when you press **Bind**, and
saved with the file. Put an Armature modifier after it and the mesh follows the bones.

Add it from **Add Modifier ▸ Edit**. It works on meshes. In the
[Modifier List](MODIFIER_LIST.md)'s Add menu it's in the **Weights** column.

## Settings

- **Armature**: the armature whose deform bones get groups.
- **Method**:
  - **Heat** spreads each bone's weight over the surface from the parts that face it, for smooth
    bends at the joints. Loose parts that face no bone, such as buttons, eyes or parts inside
    others, take the weights of their nearest bones, so binding doesn't fail on them.
  - **Distance** weighs nearer bones more. It's quicker and works on any mesh, but bends less
    smoothly.
- **Only Selected Bones**: binds only the bones selected in the armature. The groups of the other
  bones are kept as they are, so you can redo a few bones without touching the rest.
- **Max Bones**: the most bones a point may follow. The strongest are kept. Zero has no limit.
- **Normalize**: scales each point's bone weights so they add up to one.
- **Bind** works out the weights. **Unbind** clears them.

Armature, Method and Only Selected Bones are used when binding. Max Bones and Normalize change the
result at once.

The bind belongs to the points of the mesh at that moment. If you add or delete points, the
modifier asks you to bind again. Moving points doesn't.

The groups it writes replace groups of the same name on the mesh. Other groups are left alone,
so groups you painted by hand still work, and [Smooth Weights](SMOOTH_WEIGHTS.md) or
[Bone Colors](BONE_COLORS.md) after it see the new groups. Applying the modifier makes them real
vertex groups.

## A stack for a body

1. Auto Weights, bound to the armature.
2. [Smooth Weights](SMOOTH_WEIGHTS.md), to soften the joints.
3. [Bone Colors](BONE_COLORS.md), to see which bone moves what.
4. Armature.

## In Python

```python
mod = obj.modifiers.new("Auto Weights", 'AUTO_WEIGHTS')
mod.object = bpy.data.objects["Armature"]
mod.method = 'HEAT'
mod.max_bones = 4
with bpy.context.temp_override(object=obj):
    bpy.ops.object.auto_weights_bind(modifier=mod.name)
```
