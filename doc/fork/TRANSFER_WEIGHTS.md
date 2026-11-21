# Transfer Weights

<img src="images/icons/mod_transfer_weights.png" width="32" height="32" alt="Transfer Weights">

Copies the weights of a body that's already weighted onto clothes, armor or anything else that
should follow it. Each point takes the weights of the nearest point on the body's surface, blended
across that face. It updates as you go: move or edit the armor and its weights follow.

You don't have to make the vertex groups first. The modifier adds the groups the body has.

Add it from **Add Modifier ▸ Edit**. It works on meshes. In the
[Modifier List](MODIFIER_LIST.md)'s Add menu it's in the **Weights** column.

## Settings

- **Source**: the weighted mesh to copy from. Its weights after its own modifiers are used, so a
  body with [Auto Weights](AUTO_WEIGHTS.md) works too.
- **Groups**: which of the source's groups to copy.
  - **All Groups**: every group.
  - **Bone Groups**: groups named after a deform bone of the armature, found from this object's
    Armature modifier or parent, else from the source's. Without an armature, every group.
  - **Selected Bones**: the groups of the bones selected in the armature.
- **Max Distance**: points farther than this from the source don't copy from it. They take the
  weights of the points around them instead, so the tip of a cape or a spike on a shoulder plate
  follows the part it hangs from rather than whatever body part happens to be nearest. Zero has no
  limit.
- **Smooth**: steps of smoothing after copying. Helps plates that sit over a joint.
- **Max Bones**: the most bones a point may follow. The strongest are kept. Zero has no limit.
- **Normalize**: scales each point's copied weights so they add up to one.

The copied groups replace groups of the same name on the mesh. Other groups are left alone.

## A stack for armor

1. Transfer Weights, with the body as the source.
2. [Bone Colors](BONE_COLORS.md), to check it.
3. Armature.

## In Python

```python
mod = armor.modifiers.new("Transfer Weights", 'TRANSFER_WEIGHTS')
mod.object = bpy.data.objects["Body"]
mod.max_distance = 0.1
mod.smooth = 2
```
