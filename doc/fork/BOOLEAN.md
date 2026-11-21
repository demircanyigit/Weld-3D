# Boolean additions

The Boolean modifier has three additions.

## Self operand

**Operand Type > Self** merges the loose parts of the mesh with each other, with no other object.
Overlapping parts become one closed piece and the faces inside are removed. It always unions, with
the Exact solver, so Operation and Solver are hidden. Self Intersection and Hole Tolerant still
show.

## Auto solver

**Solver > Auto** uses Manifold, which is fast, and falls back to Exact when Manifold can't take the
meshes, for example when a cutter has a hole. Auto goes straight to Exact when Self Intersection
or Hole Tolerant is on, since only Exact has those.

## Clean

**Clean** tidies the result: points at the same place are merged, then flat faces that the cut
split up are joined again. Faces stay apart across material, seam and sharp edges.
