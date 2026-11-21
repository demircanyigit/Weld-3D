# Bone Colors

<img src="images/icons/mod_bone_colors.png" width="32" height="32" alt="Bone Colors">

Colors the mesh by the bones it follows, so you can see every bone's area at once instead of one
vertex group at a time. Each bone gets its own color, picked from its name, so it stays the same
every time. Where bones share a point, their colors blend by weight. Points that follow no bone
are dark grey.

Left and right bones get the same color. These names count as a pair: `Arm.L` and `Arm.R`,
`Hand_L` and `Hand_R`, `Leg-L` and `Leg-R`, `L.Foot` and `R.Foot`, `Left` and `Right`, and the
same with lower case and spaces.

Solid shading shows the colors by itself, whatever its Color setting is. The modifier is on in the
viewport and off in renders when you add it. Put it after [Auto Weights](AUTO_WEIGHTS.md),
[Smooth Weights](SMOOTH_WEIGHTS.md) or [Transfer Weights](TRANSFER_WEIGHTS.md) to see their
result as you change them.

Add it from **Add Modifier ▸ Edit**. It works on meshes. In the
[Modifier List](MODIFIER_LIST.md)'s Add menu it's in the **Weights** column.

## Settings

- **Seed**: picks other colors.
- **Mirror Match**: left and right bones share a color. Off gives every bone its own.

Bone groups are the groups named after a deform bone of the armature, from the object's Armature
modifier or its parent. Without an armature, every vertex group gets a color.

The colors are stored in a color attribute named `bone_colors`.

## In Python

```python
mod = obj.modifiers.new("Bone Colors", 'BONE_COLORS')
mod.seed = 3
mod.use_mirror = True
```
