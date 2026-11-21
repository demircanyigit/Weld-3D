# Patch

<img src="images/icons/mod_patch.png" width="32" height="32" alt="Patch">

Cuts a surface into pieces and turns them into sewn-on patches. Good for patched sails, tents,
cloaks, flags, quilts and worn clothing.

Add it from **Add Modifier ▸ Generate**. It works on meshes with faces. In the
[Modifier List](MODIFIER_LIST.md)'s Add menu it's in the **Build** column.

## Modes

- **Overlay**: the surface stays whole, and some of the pieces become patches lying on top of it.
  Each patch is a little bigger than its piece, so it overlaps the cloth around it.
- **Split**: the whole surface is cut into pieces, like a quilt, with stitches along the seams.

The borders are cut straight through the faces, so the pieces have clean edges even on a coarse
mesh. UVs and other attributes carry over to the patches.

## Pieces

- **Layout**:
  - **Voronoi**: irregular pieces around random points on the surface. **Count** sets how many,
    and **Relax** evens out their size. **Scale** stretches the pieces along X, Y and Z, for long
    or flat pieces.
  - **Grid**: boxes of a set **Size** in X, Y and Z. A size of zero leaves that axis uncut.
  - **Strips**: bands across one **Axis**, of a set **Width**.
  - **Object**: cut the pieces with a **Cut Object**, like a boolean. Each closed part of it, such
    as a box or a sphere, cuts out the surface inside it. Concave shapes work too. A flat, open part
    such as a plane works like a cookie cutter instead: each face cuts out what it covers along the
    object's Z axis. Edit or move the cut object and the pieces follow. In Split mode the rest of
    the surface is one more piece. In Overlay mode every shape becomes a patch.
- **Seed**: changes the layout and which pieces become patches.
- **Per Island**: cuts each loose part on its own, so no piece reaches across two parts, and every
  part gets at least one piece.
- **Vertex Group**: where patches may go. Lower weights make patches rarer. In Split mode the
  surface outside the group stays whole. **Invert** flips it.

## Shape

- **Amount** (Overlay): the share of the pieces that become patches.
- **Margin** (Overlay): how far each patch reaches past its piece.
- **Cut Hole** (Overlay): removes the cloth under each patch.
- **Gap** (Split): space between the pieces. Below zero they overlap.
- **Lift**: distance of the patches above the surface, so they don't flicker with it.
  **Random Lift** adds a different amount to each patch.
- **Thickness**: gives the patches thickness, with a rim around them.
- **Thicken Cloth**: gives the rest of the surface the same thickness. In Overlay mode it grows
  toward the back, so the patches stay on top.
- **Jitter**: shifts the border points at random, for ragged, hand-cut edges.

Patches face the same way as the surface. If they end up on the wrong side, flip the normals or
use a negative Lift.

## Stitches

Stitches go across the border of each patch, spread evenly along each straight side, so there are
none on the corners. In Split mode they sit on the seam between two pieces. Borders that are the
edge of the original mesh get no stitches. Where two patches meet, only one row of stitches is
laid along the shared side.

- **Object**: an object used for each stitch instead of the built-in box. Its rotation and scale
  are used. Its X axis goes across the border, Y along it, and Z up.
- **Collection**: several stitch objects, picked at random.
- **Spacing**: distance between the stitches.
- **Length**, **Width** and **Thickness**: size of the built-in box across the border, along it,
  and above the patch. The box reaches down to the cloth, so it doesn't float.
- **Amount**: the share of the borders that get stitches. In Split mode, this picks which seams
  are sewn.
- **Random Size** and **Random Angle**: make each stitch a little different, so a row doesn't
  look stamped.

## Materials

- **Patch**: the material index of the patches. **Random Materials** picks at random from this
  many, starting at Patch, for patches or pieces of different colors. It works in both modes.
- **Stitch**: the material index of the stitches.

The cloth keeps its own materials. A `patch_id` face attribute holds the number of each patch,
and -1 on the cloth and the stitches, for [Materialize](MATERIALIZE.md) or a shader.

## In Python

```python
mod = obj.modifiers.new("Patch", 'PATCH')
mod.mode = 'OVERLAY'
mod.count = 30
mod.amount = 0.25
mod.thickness = 0.01
mod.stitch_spacing = 0.08
```
