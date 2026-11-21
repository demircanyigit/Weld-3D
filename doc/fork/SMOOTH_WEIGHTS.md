# Smooth Weights

<img src="images/icons/mod_smooth_weights.png" width="32" height="32" alt="Smooth Weights">

Blurs vertex group weights, like the Blur brush in Weight Paint mode, but as a modifier. The blur
travels along the edges of the mesh, so it doesn't jump across gaps, such as between the legs or
from an arm to the body.

Add it from **Add Modifier ▸ Edit**. It works on meshes. In the
[Modifier List](MODIFIER_LIST.md)'s Add menu it's in the **Weights** column.

## Settings

- **Groups**: which vertex groups to smooth.
  - **All Groups**: every group.
  - **Bone Groups**: groups named after a deform bone of the armature. The armature is the one of
    the object's Armature modifier, or its parent. Without an armature, every group is smoothed.
  - **Selected Bones**: the groups of the bones selected in the armature.
  - **One Group**: the group you pick.
- **Factor**: how much each step blends.
- **Iterations**: steps of smoothing. More steps reach farther.
- **Normalize**: scales each point's smoothed weights so they add up to one.
- **Mask**: smooths only where this vertex group has weight, less where the weight is low.
  **Invert** smooths where it has none.

Weights below 0.001 are removed, so the blur doesn't leave faint weights everywhere.

## In Python

```python
mod = obj.modifiers.new("Smooth Weights", 'SMOOTH_WEIGHTS')
mod.groups = 'BONES'
mod.factor = 0.5
mod.iterations = 5
```
