# Rock

<img src="images/icons/mod_rock.png" width="32" height="32" alt="Rock">

Turns a shape into a low poly rock. You model a rough shape, such as a box you scaled, cut or
extruded, and the modifier splits corners and edges off it with flat cuts in random directions.
What you modeled stays the base of the rock.

Add it from **Add Modifier ▸ Generate**. It works on meshes. In the [Modifier List](MODIFIER_LIST.md)'s Add menu it's in the **Build** column.

## Settings

- **Cuts**: how many flat cuts.
- **Depth**: how deep the cuts go. Each cut takes off up to this share of the shape in its
  direction, so a high Depth leaves a smaller, more broken rock.
- **Corners**: how much the cuts go through the corners of the shape. At 1 they cut the
  corners off, at 0 they come from any direction.
- **Seed**: changes the cuts.
- **Per Island**: every loose part is its own rock, with its own cuts. Model a few boxes in one
  object to get a pile of different rocks. Off cuts all the parts together, as one rock.
- **Cut Material**: gives the faces made by the cuts their own material, for example a lighter,
  fresher stone. The number is the material slot.

The faces made by the cuts are flat shaded, the rest keep their shading. On meshes with face
sets, the cut faces take the face set that covers most of their rock, so no new face sets are
added.

## Good to know

- The mesh must be closed. Open parts can't be cut and give an error.
- Every cut keeps the side with the middle of the shape, so the rock is always the shape with
  pieces taken off, never bigger than it.
- For a pile of rocks, follow it with [Collapse](COLLAPSE.md) to drop them onto the ground.

## In Python

```python
mod = obj.modifiers.new("Rock", 'ROCK')
mod.cuts = 24
mod.depth = 0.45
```

The Python type is `RockModifier`. Official Blender doesn't know this modifier, so a file that uses it won't give the same result there.
