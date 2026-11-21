# Bevel additions

The Bevel modifier has a **Random** panel, for bevels that look worn or hand made instead of
machine perfect.

- **Width**: makes every edge up to this much narrower, at random. At 0 every edge has the full
  Amount. With **Affect > Vertices** it works on every vertex. It also works with the Weight and
  Vertex Group limit methods: the random factor goes on top of the weights.
- **Segments**: gives every loose part up to this many more segments than **Segments**, at
  random. All edges of one part have the same number, so their corners still meet.
- **Seed**: changes the random widths and segments.

Both are off by default, so files made before look the same.

## In Python

```python
mod = obj.modifiers.new("Bevel", 'BEVEL')
mod.random_width = 0.5
mod.random_segments = 2
mod.seed = 3
```

Official Blender doesn't know these settings. A file that uses them gives plain bevels there.
