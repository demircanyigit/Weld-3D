# Voronoi Fracture

<img src="images/icons/mod_voronoi_fracture.png" width="32" height="32" alt="Voronoi Fracture">

Cuts a closed mesh into solid pieces, one for the area around each point. The points are random
points in the bounds of the mesh, or the points of another object, such as particles, a point
cloud or the vertices of a mesh.

Each piece is closed, so it can be used for rigid body simulation or pushed apart with
[Exploded View](EXPLODED_VIEW.md).

![Voronoi Fracture with Exploded View, Poly Select with four Poly Edits, Panelize and Radial Array](images/modifiers_modeling.png)

*From left to right: Voronoi Fracture with Exploded View, Poly Select with four Poly Edits, Panelize and Radial Array.*

Add it from **Add Modifier ▸ Generate**. It works on meshes. In the [Modifier List](MODIFIER_LIST.md)'s Add menu it's in the **Edit** column.

## Settings

- **Points**: Where the cell points come from.
  - **Random**: Random points in the bounds of the mesh.
  - **Object**: The points, vertices, curve points or instances of another object.
- **Object**: Object whose points are the cell points.
- **Count**: Number of random points, at most one piece each.
- **Per**: What Count is for.
  - **Whole Mesh**: Count points in the bounds of the whole mesh.
  - **Island**: Every loose part breaks on its own, into about Count pieces, so small parts break
    as much as big ones and no piece spans two parts.
  - **Face Set**: Count points under the faces of every face set, so each area breaks into about
    Count pieces whatever its size. The mesh needs face sets.
- **By Size**: With Island or Face Set. Count is for the biggest part, and smaller parts break into
  fewer pieces by their volume (islands) or area (face sets), so the pieces are about the same
  size everywhere. A part too small for two pieces stays whole.
- **Seed**: Changes where the random points are.
- **Cut Gap**: Distance between the inside faces of neighboring pieces.
- **Separate Islands**: Make a separate piece of every part of a cell that isn't connected to the rest.
- **Interior Material**: Give the faces made by the cuts their own material.
- **Interior Material Index**: Material index of the faces made by the cuts.

## Good to know

- The mesh should be closed. Open parts are cut too, but their pieces aren't solid.
- **Cut Gap** shrinks every piece a little, so the pieces don't touch.
- On meshes with face sets, the cut faces take the face set that covers most of the outside of
  their piece, so no new face sets are added.
- For pieces of a material (stone, glass, wood), see [Shatter](SHATTER.md).

## As a node

Geometry Nodes has a **Voronoi Fracture** node too, described in [Modeling nodes in detail](../../README.md#voronoi-fracture).

## In Python

```python
mod = obj.modifiers.new("Voronoi Fracture", 'VORONOI_FRACTURE')
```

The Python type is `VoronoiFractureModifier`. Official Blender doesn't know this modifier, so a file that uses it won't give the same result there.
