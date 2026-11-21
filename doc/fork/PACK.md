# Pack

<img src="images/icons/mod_pack.png" width="32" height="32" alt="Pack">

Fills the volume of the mesh with copies of other objects, fitted close together. Put it on a box,
a crate, a round tank or any closed shape, and pick the objects to pack: rocks for a gabion wall,
stones for a dry stone wall, boxes for a crate.

The volume is measured on a grid of small cells. Each piece is turned into the cells it fills, and
pieces go in one at a time, each trying several places and turns, where they fit without overlapping
anything. The pieces fit by their real shape, not their bounding box, so round stones pack against
each other and against curved walls.

A closed mesh is filled by its real shape. A mesh with holes fills its bounding box instead, and
the panel says so.

Add it from **Add Modifier ▸ Generate**. It works on meshes.

## Settings

- **Object**, **Collection**: the objects to pack. **Add Object** adds more, one slot each, and the
  X removes a slot. Pieces are picked evenly from all of them. The object with the modifier is
  never packed into itself.
- **Output**:
  - **Geometry**: one mesh that can be edited and works with the modifiers after it.
  - **Instances**: copies of the objects, much lighter with thousands of pieces.
- **Mode**:
  - **Fill**: each piece goes where it touches the walls and the other pieces the most, so the
    volume fills evenly from the walls in.
  - **Settle**: each piece drops to the lowest place it fits, like things dropped into a crate.
- **Rotation**:
  - **None**: the pieces keep the turn of their objects.
  - **Upright**: they turn around Z only.
  - **Free**: they try several turns and keep the one that fits best.
  - **Follow Walls**: near a wall, a piece turns its flattest side to it, so pieces along a curved
    wall follow the curve. With Settle the floor counts as a wall, so pieces lie flat on it.
- **Count**: how many pieces. 0 fills until nothing more fits.
- **Gap**: space kept between the pieces.
- **Detail**: how many cells along the longest side of the volume. Higher fits tighter but is
  slower. Gap is measured in whole cells, so a Gap smaller than a cell has no effect without Fill
  Gaps.
- **Shake**: passes that move each piece down and towards its neighbours, try a small turn, then
  put more pieces into the space this frees. More passes pack tighter but take longer.
- **Random Scale**: makes the pieces smaller at random, up to this share.
- **Seed**: picks other pieces and turns.
- **Keep Container**: keeps the mesh around the pieces.

## Fill Gaps

Swells each piece into the empty space around it, so the pieces fit each other like the stones of a
dry stone wall. The leftover space is shared out: every empty cell belongs to the piece closest to
it, and each piece grows into its share, stopping Gap away from its neighbours and at the walls.

- **Amount**: how far the pieces swell. 0 keeps their shape, 1 closes the space up to the Gap.
- **Shape**:
  - **Keep Shape**: the pieces swell evenly and stay rounded.
  - **Blocky**: they flatten fully against each other, like cut stone.

The pieces need enough points to bend: a plain cube can't swell much, rocks with normal detail
can. Fill Gaps only works with Output set to Geometry, since instances share one mesh.
