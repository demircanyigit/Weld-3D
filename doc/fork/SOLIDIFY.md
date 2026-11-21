# Solidify additions

The Solidify modifier has a **Random** panel and a shading setting for the rim.

## Random

- **Thickness**: makes the thickness up to this much thinner, at random. At 0 the thickness is
  the same everywhere.
- **Scale**: size of the thick and thin areas. Small values give patchy walls, big values slow
  changes.
- **Per Island**: every loose part gets one random thickness of its own instead, for example
  leaves, shards or panels in one object. Scale isn't used then.
- **Seed**: changes the random thickness.

It works in both modes, Simple and Complex, and with a vertex group: the random factor goes on top
of the weights.

## Rim Shading

**Rim > Shading** sets how the faces of the rim are shaded:

- **Keep**: like the faces next to the rim, as before.
- **Flat**: flat shaded.
- **Smooth**: smooth shaded.
- **Hard**: smooth shaded, with sharp edges where the rim meets the outside and the inside, so
  both the convex and the concave edges stay crisp.

All of it is off by default, so files made before look the same.

## In Python

```python
mod = obj.modifiers.new("Solidify", 'SOLIDIFY')
mod.random_thickness = 0.6
mod.random_scale = 0.5
mod.use_random_per_island = False
mod.seed = 1
mod.rim_shading = 'HARD'
```

Official Blender doesn't know these settings. A file that uses them gives an even thickness and the
usual rim shading there.
