# SPDX-FileCopyrightText: 2026 Weld 3D Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Tree Graph operators: add a tree object, make a new graph. Every preset is a graph built from
# level nodes (Trunk, Branches, Leaves) linked from right to left into the Tree Output, with a Force
# linked into the stems it bends. The presets come from EZ-Tree, see _EZ_TREES.

import math
import os

import bpy
from bpy.types import Operator
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty
from bpy.app.translations import pgettext_data as data_
from bpy_extras import object_utils
from mathutils import Matrix, Vector


TREE_TYPE = "TreeGraphNodeTree"


def _set_input(socket, value):
    """Menu inputs take the name of an item, presets give its identifier."""
    try:
        socket.default_value = value
    except TypeError:
        if not isinstance(value, str):
            raise
        socket.default_value = value.replace("_", " ").title()


def _set_inputs(node, values):
    for name, value in values.items():
        _set_input(node.inputs[name], value)


def _set_curve(node, points):
    """Sets the Length Along Parent curve of a Branches node."""
    curve = node.mapping.curves[0]
    while len(curve.points) > 2:
        curve.points.remove(curve.points[1])
    curve.points[0].location = points[0]
    curve.points[1].location = points[-1]
    for x, y in points[1:-1]:
        curve.points.new(x, y)
    node.mapping.update()


# Inputs that presets vary for every branch or leaf, as "<input> Random" values, with whether
# the amount is a share of the value (True) or added to it (False).
_RANDOM_SPREAD = {"Length": True, "Angle": False}


class _GraphBuilder:
    def __init__(self, tree):
        self.tree = tree
        self.nodes = tree.nodes
        self.links = tree.links

    def add(self, idname, location, values=None):
        node = self.nodes.new(idname)
        node.location = location
        if values:
            values = dict(values)
            spread = {name: values.pop(name + " Random")
                      for name in _RANDOM_SPREAD if name + " Random" in values}
            _set_inputs(node, values)
            for i, (name, amount) in enumerate(spread.items()):
                if name in node.inputs and amount > 0.0:
                    self.add_random(node, name, values.get(name, node.inputs[name].default_value),
                                    amount, i)
        return node

    def add_random(self, node, name, value, amount, i):
        """Varies an input of every branch or leaf with a Random Value node."""
        relative = _RANDOM_SPREAD[name]
        low, high = (value * (1.0 - amount), value * (1.0 + amount)) if relative else             (value - amount, value + amount)
        rand = self.nodes.new("FunctionNodeRandomValue")
        rand.data_type = 'FLOAT'
        rand.label = name
        rand.hide = True
        rand.location = (node.location.x - 200.0, node.location.y - 40.0 - 50.0 * i)
        # Float Min, Max and Seed, the vector sockets come first.
        rand.inputs[2].default_value = max(low, 0.0) if relative else low
        rand.inputs[3].default_value = high
        rand.inputs[8].default_value = i
        self.links.new(rand.outputs[1], node.inputs[name])
        return rand

    def link(self, child, parent):
        """Grows the child node on the parent node."""
        is_output = parent.bl_idname == "TreeGraphNodeOutput"
        to_socket = parent.inputs["Tree"] if is_output else parent.inputs["Children"]
        self.links.new(child.outputs[0], to_socket)

    def link_force(self, force, level):
        """Bends or trims the level with a force or shape node."""
        self.links.new(force.outputs[0], level.inputs["Forces"])


PRESET_ITEMS = (
    ('OAK', "Oak", "A wide oak with a gnarled crown"),
    ('BROADLEAF', "Ash", "A tall ash with long open limbs"),
    ('DEAD', "Bare Tree", "The oak without its leaves"),
    ('PINE', "Pine", "A tall pine, its branches shorter toward the top"),
    ('BUSH', "Bush", "Stems from the ground with leaves"),
    ('GRASS', "Grass Tuft", "Blades from one point, on hidden stems"),
    ('PALM', "Palm", "A slim trunk with arched fronds"),
    ('FERN', "Fern", "Arched leaves of small leaflets, on hidden stems"),
    ('EMPTY', "Empty Graph", "Only a trunk, to build your own"),
)

# Bark and leaf colors of each tree.
TREE_COLORS = {
    'OAK': ((0.17, 0.13, 0.10), (0.13, 0.30, 0.07)),
    'BROADLEAF': ((0.25, 0.21, 0.17), (0.16, 0.36, 0.08)),
    'DEAD': ((0.30, 0.27, 0.24), (0.30, 0.25, 0.15)),
    'PINE': ((0.45, 0.22, 0.12), (0.10, 0.24, 0.09)),
    'BUSH': ((0.22, 0.15, 0.10), (0.12, 0.35, 0.08)),
    'GRASS': ((0.22, 0.15, 0.10), (0.20, 0.42, 0.10)),
    'PALM': ((0.36, 0.29, 0.21), (0.16, 0.38, 0.10)),
    'FERN': ((0.22, 0.15, 0.10), (0.11, 0.33, 0.07)),
    'EMPTY': ((0.22, 0.15, 0.10), (0.12, 0.35, 0.08)),
}

# The trees, in the numbers of EZ-Tree (MIT license, Copyright (c) 2024 Daniel Greenheck), whose
# presets they are taken from: lists by level, the trunk first. Lengths and radii are in its units,
# a quarter of a meter. Angles are in degrees, from the parent.
_EZ_UNIT = 0.25
_EZ_TREES = {
    'OAK': {
        "form": 'BROADLEAF', "force": 0.02,
        "children": (6, 4, 3), "angle": (None, 54, 58, 32),
        "gnarl": (0.0, 0.1, 0.15, 0.09), "length": (37.24, 11.08, 12.39, 7.16),
        "radius": (1.41, 0.9, 0.69, 1.0), "sections": (8, 6, 3, 1), "sides": (7, 5, 3, 3),
        "start": (None, 0.49, 0.06, 0.12), "taper": (0.73, 0.42, 0.69, 0.75),
        "twist": (-0.23, 0.42, 0.0, 0.0),
        "leaves": {"angle": 42, "count": 18, "start": 0.16, "size": 2.5, "random": 0.7,
                   "cross": True},
    },
    'BROADLEAF': {
        "form": 'BROADLEAF', "force": 0.01,
        "children": (7, 4, 3), "angle": (None, 48, 75, 60),
        "gnarl": (0.03, 0.25, 0.2, 0.09), "length": (43.47, 27.14, 9.51, 4.6),
        "radius": (2.0, 0.63, 0.76, 0.7), "sections": (12, 8, 6, 4), "sides": (12, 6, 4, 3),
        "start": (None, 0.23, 0.33, 0.0), "taper": (0.7, 0.7, 0.7, 0.7),
        "twist": (0.09, -0.07, 0.0, 0.0),
        "leaves": {"angle": 55, "count": 16, "start": 0.0, "size": 2.67, "random": 0.72,
                   "cross": True},
    },
    'PINE': {
        "form": 'CONIFER', "force": -0.003,
        "children": (82,), "angle": (None, 110),
        "gnarl": (0.05, 0.08), "length": (50.0, 23.87),
        "radius": (1.05, 0.36), "sections": (12, 10), "sides": (8, 6),
        "start": (None, 0.27), "taper": (0.7, 0.7),
        "twist": (0.0, 0.0),
        "leaves": {"angle": 39, "count": 30, "start": 0.09, "size": 1.435, "random": 0.2,
                   "cross": True},
    },
    'BUSH': {
        "form": 'BROADLEAF', "force": 0.0,
        "children": (7, 3, 2), "angle": (None, 21.5, 62.6, 60),
        "gnarl": (0.11, 0.09, 0.05, 0.09), "length": (0.1, 15.3, 5.59, 4.6),
        "radius": (0.58, 0.95, 0.76, 0.7), "sections": (6, 6, 10, 10), "sides": (4, 4, 4, 3),
        "start": (None, 0.53, 0.33, 0.0), "taper": (0.7, 0.7, 0.7, 0.7),
        "twist": (0.3, -0.07, 0.0, 0.0),
        "leaves": {"angle": 55, "count": 12, "start": 0.0, "size": 2.45, "random": 0.72,
                   "cross": True},
    },
}
_EZ_TREES['DEAD'] = dict(_EZ_TREES['OAK'], leaves=None)


def _trunk_values(ez):
    return {"Length": ez["length"][0] * _EZ_UNIT, "Radius": ez["radius"][0] * _EZ_UNIT,
            "Taper": ez["taper"][0], "Gnarl": abs(ez["gnarl"][0]), "Twist": ez["twist"][0],
            "Sections": ez["sections"][0], "Sides": max(ez["sides"][0], 3)}


def _branch_values(ez, n):
    return {"Count": ez["children"][n - 1], "Start": ez["start"][n],
            "Angle": math.radians(ez["angle"][n]), "Length": ez["length"][n] * _EZ_UNIT,
            "Radius": min(ez["radius"][n], 1.0), "Taper": ez["taper"][n],
            "Gnarl": abs(ez["gnarl"][n]), "Twist": ez["twist"][n],
            "Sections": ez["sections"][n], "Sides": max(ez["sides"][n], 3)}


def _leaf_values(leaves):
    return {"Count": leaves["count"], "Start": leaves["start"],
            "Angle": math.radians(leaves["angle"]), "Size": leaves["size"] * _EZ_UNIT,
            "Size Random": leaves["random"], "Cards": 'CROSS' if leaves["cross"] else 'SINGLE'}


_DOWN = (0.0, 0.0, -1.0)


def _build_grass(g, output):
    trunk = g.add("TreeGraphNodeTrunk", (0.0, 0.0), {
        "Skin": "Spine Only", "Length": 0.03, "Radius": 0.01, "Sections": 2, "Gnarl": 0.0,
    })
    g.link(trunk, output)
    blades = g.add("TreeGraphNodeBranches", (-360.0, 0.0), {
        "Skin": "Spine Only", "Count": 36, "Start": 0.0, "Angle": math.radians(22.0),
        "Length": 0.55, "Length Random": 0.45, "Gnarl": 0.25, "Sections": 6, "Spread": 1.0,
    })
    g.link(blades, trunk)
    droop = g.add("TreeGraphNodeForce", (-360.0, -480.0),
                  {"Mode": 'DIRECTION', "Direction": _DOWN, "Strength": 0.05})
    g.link_force(droop, blades)
    fronds = g.add("TreeGraphNodeFronds", (-720.0, 0.0), {
        "Width": 0.035, "Width Random": 0.3, "Tip": 0.6, "Fold": math.radians(20.0),
        "Roll Random": 0.3, "Mode": 'UP', "Blend": 0.6, "Sections": 6, "Across": 2,
    })
    g.link(fronds, blades)


def _build_palm(g, output):
    trunk = g.add("TreeGraphNodeTrunk", (0.0, 0.0), {
        "Length": 6.0, "Radius": 0.2, "Taper": 0.35, "Gnarl": 0.04, "Sections": 16, "Sides": 10,
    })
    g.link(trunk, output)
    stalks = g.add("TreeGraphNodeBranches", (-360.0, 0.0), {
        "Skin": "Spine Only", "Count": 14, "Start": 0.96, "Angle": math.radians(65.0),
        "Length": 2.6, "Length Random": 0.2, "Gnarl": 0.03, "Sections": 10, "Spread": 1.0,
    })
    g.link(stalks, trunk)
    arch = g.add("TreeGraphNodeForce", (-360.0, -480.0),
                 {"Mode": 'DIRECTION', "Direction": _DOWN, "Strength": 0.06})
    g.link_force(arch, stalks)
    leaflets = g.add("TreeGraphNodeBranches", (-720.0, 0.0), {
        "Skin": "Spine Only", "Count": 36, "Start": 0.12, "Angle": math.radians(65.0),
        "Length": 0.75, "Length Random": 0.15, "Gnarl": 0.05, "Sections": 4, "Arrange": 'OPPOSITE',
    })
    g.link(leaflets, stalks)
    g.link_force(arch, leaflets)
    fronds = g.add("TreeGraphNodeFronds", (-1080.0, 0.0), {
        "Width": 0.07, "Width Random": 0.15, "Tip": 0.5, "Fold": math.radians(15.0),
        "Mode": 'UP', "Blend": 0.4, "Sections": 4, "Across": 2,
    })
    g.link(fronds, leaflets)


def _build_fern(g, output):
    trunk = g.add("TreeGraphNodeTrunk", (0.0, 0.0), {
        "Skin": "Spine Only", "Length": 0.05, "Radius": 0.01, "Sections": 2, "Gnarl": 0.0,
    })
    g.link(trunk, output)
    stalks = g.add("TreeGraphNodeBranches", (-360.0, 0.0), {
        "Skin": "Spine Only", "Count": 11, "Start": 0.0, "Angle": math.radians(38.0),
        "Length": 1.0, "Length Random": 0.25, "Gnarl": 0.08, "Sections": 10, "Spread": 1.0,
    })
    g.link(stalks, trunk)
    arch = g.add("TreeGraphNodeForce", (-360.0, -480.0),
                 {"Mode": 'DIRECTION', "Direction": _DOWN, "Strength": 0.05})
    g.link_force(arch, stalks)
    leaflets = g.add("TreeGraphNodeBranches", (-720.0, 0.0), {
        "Skin": "Spine Only", "Count": 28, "Start": 0.15, "Angle": math.radians(70.0),
        "Length": 0.22, "Gnarl": 0.05, "Sections": 3, "Arrange": 'OPPOSITE',
    })
    g.link(leaflets, stalks)
    fronds = g.add("TreeGraphNodeFronds", (-1080.0, 0.0), {
        "Width": 0.05, "Width Random": 0.1, "Tip": 0.45, "Tip Shape": 'ROUND',
        "Curl": math.radians(30.0), "Mode": 'UP', "Blend": 0.4, "Sections": 3, "Across": 2,
    })
    g.link(fronds, leaflets)


_CUSTOM_GRAPHS = {'GRASS': _build_grass, 'PALM': _build_palm, 'FERN': _build_fern}


def build_tree_graph(tree, preset):
    tree.nodes.clear()
    g = _GraphBuilder(tree)
    output = g.add("TreeGraphNodeOutput", (300.0, 0.0))
    if preset in _CUSTOM_GRAPHS:
        _CUSTOM_GRAPHS[preset](g, output)
        return
    ez = _EZ_TREES.get(preset)
    if ez is None:
        trunk = g.add("TreeGraphNodeTrunk", (0.0, 0.0))
        g.link(trunk, output)
        return
    _set_input(output.inputs["Form"], ez["form"])
    trunk = g.add("TreeGraphNodeTrunk", (0.0, 0.0), _trunk_values(ez))
    g.link(trunk, output)
    stems = [trunk]
    for n in range(1, len(ez["children"]) + 1):
        node = g.add("TreeGraphNodeBranches", (-360.0 * n, 0.0), _branch_values(ez, n))
        if n > 1:
            node.label = data_("Branches {:d}").format(n)
        g.link(node, stems[-1])
        stems.append(node)
    if ez["force"] != 0.0:
        force = g.add("TreeGraphNodeForce", (0.0, -480.0), {"Strength": ez["force"]})
        for stem in stems:
            g.link_force(force, stem)
    if ez["leaves"]:
        leaves = g.add("TreeGraphNodeLeaves", (-360.0 * len(stems), 0.0), _leaf_values(ez["leaves"]))
        g.link(leaves, stems[-1])


def new_tree_graph(preset, name=None):
    tree = bpy.data.node_groups.new(name=name or data_("Tree Graph"), type=TREE_TYPE)
    build_tree_graph(tree, preset)
    return tree


def _material(name, color, roughness):
    mat = bpy.data.materials.new(name=name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = (*color, 1.0)
        bsdf.inputs["Roughness"].default_value = roughness
    mat.diffuse_color = (*color, 1.0)
    mat.use_backface_culling = False
    return mat


def _active_tree_modifier(context):
    ob = context.object
    if ob is None:
        return None
    md = ob.modifiers.active
    if md is not None and md.type == 'TREE':
        return md
    for md in ob.modifiers:
        if md.type == 'TREE':
            return md
    return None


class OBJECT_OT_tree_add(Operator, object_utils.AddObjectHelper):
    """Add a tree, grown from its own Tree Graph"""
    bl_idname = "object.tree_add"
    bl_label = "Add Tree"
    bl_options = {'REGISTER', 'UNDO'}

    preset: EnumProperty(
        name="Tree",
        items=PRESET_ITEMS,
        default='OAK',
    )
    scale_tree: FloatProperty(
        name="Scale",
        description="Size of the whole tree",
        default=1.0,
        min=0.001,
        soft_min=0.1,
        soft_max=10.0,
    )
    seed: IntProperty(
        name="Seed",
        description="Grows a different tree from the same graph",
        default=0,
    )
    use_leaves: BoolProperty(
        name="Leaves",
        description="Grow the leaves and fronds",
        default=True,
    )

    def draw(self, _context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        layout.prop(self, "preset")
        layout.prop(self, "scale_tree")
        layout.prop(self, "seed")
        layout.prop(self, "use_leaves")
        layout.separator()
        layout.prop(self, "align")
        layout.prop(self, "location")
        layout.prop(self, "rotation")

    def execute(self, context):
        label = dict((item[0], item[1]) for item in PRESET_ITEMS)[self.preset]
        name = data_("Tree") if self.preset == 'EMPTY' else data_(label)
        mesh = bpy.data.meshes.new(name)
        colors = TREE_COLORS.get(self.preset, TREE_COLORS['OAK'])
        mesh.materials.append(_material(data_("Bark"), colors[0], 0.9))
        mesh.materials.append(_material(data_("Leaves"), colors[1], 0.6))
        if len(colors) > 2:
            mesh.materials.append(_material(data_("Blossoms"), colors[2], 0.5))
        ob = object_utils.object_data_add(context, mesh, operator=self)

        md = ob.modifiers.new(name=data_("Tree"), type='TREE')
        md.tree_graph = new_tree_graph(self.preset, name="{:s} {:s}".format(name, data_("Graph")))
        md.scale = self.scale_tree
        md.seed = self.seed
        md.use_leaves = self.use_leaves
        return {'FINISHED'}


def _plane_normal(tree):
    """The side the Plane force of a graph faces, front view when it has none."""
    if tree is not None:
        for node in tree.nodes:
            if node.bl_idname == "TreeGraphNodePlane" and not node.mute:
                normal = Vector(node.inputs["Normal"].default_value)
                if normal.length > 1e-6:
                    return normal.normalized()
    return Vector((0.0, -1.0, 0.0))


def _outline(points, margin):
    """An eight sided outline around 2D points, a little bigger by \\a margin."""
    dirs = [Vector((math.cos(i * math.pi / 4.0), math.sin(i * math.pi / 4.0))) for i in range(8)]
    reach = [max(p.dot(d) for p in points) + margin for d in dirs]
    corners = []
    for i in range(8):
        d0, d1 = dirs[i], dirs[(i + 1) % 8]
        r0, r1 = reach[i], reach[(i + 1) % 8]
        det = d0.x * d1.y - d0.y * d1.x
        corners.append(Vector(((r0 * d1.y - r1 * d0.y) / det, (d0.x * r1 - d1.x * r0) / det)))
    return corners


def _bleed(pixels, width, height, steps=8):
    """Spreads the colors of the drawn pixels out into the empty ones around them, so the edges
    of cards do not turn dark when the texture is made smaller."""
    import numpy as np
    img = pixels.reshape(height, width, 4)
    filled = img[:, :, 3] > 0.01
    rgb = img[:, :, :3].copy()
    for _ in range(steps):
        total = np.zeros_like(rgb)
        count = np.zeros((height, width))
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            shifted_mask = np.roll(filled, (dy, dx), axis=(0, 1))
            shifted_rgb = np.roll(rgb, (dy, dx), axis=(0, 1))
            total += shifted_rgb * shifted_mask[:, :, None]
            count += shifted_mask
        grow = (~filled) & (count > 0)
        rgb[grow] = total[grow] / count[grow][:, None]
        filled = filled | grow
    img[:, :, :3] = rgb
    return img.reshape(-1)


class OBJECT_OT_tree_bake_cards(Operator):
    """Render this tree flat from the front of its plane into a color and a normal image, """ \
        """with a material and card objects to use on the leaves of other trees"""
    bl_idname = "object.tree_bake_cards"
    bl_label = "Bake Cards"
    bl_options = {'REGISTER', 'UNDO'}

    resolution: EnumProperty(
        name="Resolution",
        description="Size of the images",
        items=(
            ('512', "512", ""),
            ('1024', "1024", ""),
            ('2048', "2048", ""),
            ('4096', "4096", ""),
        ),
        default='1024',
    )
    variations: IntProperty(
        name="Variations",
        description="Cards grown from different seeds, side by side in the same images",
        default=1,
        min=1,
        max=16,
    )

    @classmethod
    def poll(cls, context):
        md = _active_tree_modifier(context)
        return md is not None and md.tree_graph is not None

    def invoke(self, context, _event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        import numpy as np

        ob = context.object
        md = _active_tree_modifier(context)
        name = ob.name
        normal = _plane_normal(md.tree_graph)
        up_ref = Vector((0.0, 0.0, 1.0)) if abs(normal.z) < 0.95 else Vector((0.0, 1.0, 0.0))
        up = (up_ref - normal * normal.dot(up_ref)).normalized()
        right = up.cross(normal)

        res = int(self.resolution)
        count = self.variations
        grid = math.ceil(math.sqrt(count))
        cell_px = res // grid

        # The mesh of every variation, and where its points lie on the plane.
        seed = md.seed
        meshes = []
        flat = []
        try:
            for v in range(count):
                md.seed = seed + v
                context.view_layer.update()
                evaluated = ob.evaluated_get(context.evaluated_depsgraph_get())
                mesh = bpy.data.meshes.new_from_object(evaluated)
                meshes.append(mesh)
                flat.append([Vector((p.co.dot(right), p.co.dot(up))) for p in mesh.vertices])
        finally:
            md.seed = seed
        if not all(flat):
            for mesh in meshes:
                bpy.data.meshes.remove(mesh)
            self.report({'ERROR'}, "The tree has no mesh to bake")
            return {'CANCELLED'}

        boxes = []
        for points in flat:
            lo = Vector((min(p.x for p in points), min(p.y for p in points)))
            hi = Vector((max(p.x for p in points), max(p.y for p in points)))
            boxes.append((lo, hi))
        size = max(max(hi.x - lo.x, hi.y - lo.y) for lo, hi in boxes) * 1.08
        margin = size * 0.02

        # A scene of its own, with a camera looking straight at the plane.
        scene = bpy.data.scenes.new("Card Bake")
        scene.render.engine = 'BLENDER_WORKBENCH'
        scene.render.resolution_x = cell_px
        scene.render.resolution_y = cell_px
        scene.render.resolution_percentage = 100
        scene.render.film_transparent = True
        scene.render.image_settings.file_format = 'PNG'
        scene.render.image_settings.color_mode = 'RGBA'
        scene.render.image_settings.color_depth = '8'
        shading = scene.display.shading
        shading.show_object_outline = False
        shading.show_cavity = False
        shading.show_shadows = False
        shading.show_specular_highlight = False
        shading.show_backface_culling = False
        camera = bpy.data.objects.new("Card Camera", bpy.data.cameras.new("Card Camera"))
        camera.data.type = 'ORTHO'
        camera.data.ortho_scale = size
        scene.collection.objects.link(camera)
        scene.camera = camera
        basis = Matrix((right, up, normal)).transposed().to_4x4()

        tmp_dir = bpy.app.tempdir
        cells_color = []
        cells_normal = []
        temp_objects = []
        try:
            for v, mesh in enumerate(meshes):
                lo, hi = boxes[v]
                center = (lo + hi) * 0.5
                # The normals as colors, seen from the front: right, up and toward the camera.
                normals = mesh.attributes.get("Card Normal") or mesh.attributes.new(
                    "Card Normal", 'FLOAT_COLOR', 'CORNER')
                corner_normals = mesh.corner_normals
                for i, item in enumerate(normals.data):
                    n = Vector(corner_normals[i].vector)
                    if n.dot(normal) < 0.0:
                        n = -n
                    item.color = (n.dot(right) * 0.5 + 0.5, n.dot(up) * 0.5 + 0.5,
                                  n.dot(normal) * 0.5 + 0.5, 1.0)
                mesh.color_attributes.active_color = normals
                part = bpy.data.objects.new("Card Part", mesh)
                scene.collection.objects.link(part)
                temp_objects.append(part)
                depth = max(p.co.dot(normal) for p in mesh.vertices) + size
                location = right * center.x + up * center.y + normal * depth
                camera.matrix_world = Matrix.Translation(location) @ basis
                camera.data.clip_end = depth * 2.0 + size

                for pass_name, cells in (("color", cells_color), ("normal", cells_normal)):
                    if pass_name == "color":
                        shading.light = 'FLAT'
                        shading.color_type = 'MATERIAL'
                        scene.view_settings.view_transform = 'Standard'
                    else:
                        shading.light = 'FLAT'
                        shading.color_type = 'VERTEX'
                        scene.view_settings.view_transform = 'Raw'
                    path = os.path.join(tmp_dir, "card_bake_{:s}_{:d}.png".format(pass_name, v))
                    scene.render.filepath = path
                    bpy.ops.render.render(write_still=True, scene=scene.name)
                    image = bpy.data.images.load(path)
                    cells.append(np.array(image.pixels[:], dtype=np.float32))
                    bpy.data.images.remove(image)
                part.hide_render = True
        finally:
            for part in temp_objects:
                bpy.data.objects.remove(part)
            cam_data = camera.data
            bpy.data.objects.remove(camera)
            bpy.data.cameras.remove(cam_data)
            bpy.data.scenes.remove(scene)

        # Put the cells together into one image each.
        def atlas(cells, empty):
            out = np.zeros((res, res, 4), dtype=np.float32)
            out[:, :, :3] = empty
            for v, cell in enumerate(cells):
                col, row = v % grid, v // grid
                block = cell.reshape(cell_px, cell_px, 4)
                y0, x0 = row * cell_px, col * cell_px
                drawn = block[:, :, 3] > 0.0
                target = out[y0:y0 + cell_px, x0:x0 + cell_px]
                target[drawn] = block[drawn]
            return out.reshape(-1)

        color_pixels = _bleed(atlas(cells_color, (0.0, 0.0, 0.0)), res, res)
        normal_pixels = atlas(cells_normal, (0.5, 0.5, 1.0))
        normal_pixels.reshape(res, res, 4)[:, :, 3] = 1.0

        images = []
        for suffix, pixels, non_color in (("Color", color_pixels, False), ("Normal", normal_pixels, True)):
            image_name = "{:s} Cards {:s}".format(name, suffix)
            image = bpy.data.images.get(image_name)
            if image is not None and tuple(image.size) != (res, res):
                bpy.data.images.remove(image)
                image = None
            if image is None:
                image = bpy.data.images.new(image_name, res, res, alpha=True)
            if non_color:
                image.colorspace_settings.is_data = True
            image.pixels.foreach_set(pixels)
            if bpy.data.filepath:
                folder = bpy.path.abspath("//cards")
                os.makedirs(folder, exist_ok=True)
                image.filepath_raw = os.path.join(folder, bpy.path.clean_name(image_name) + ".png")
                image.file_format = 'PNG'
                image.save()
            else:
                image.pack()
            images.append(image)

        material = _card_material("{:s} Cards".format(name), images[0], images[1])

        # One card for every variation: an outline around its points, from its base up.
        collection = bpy.data.collections.get("{:s} Cards".format(name))
        if collection is None:
            collection = bpy.data.collections.new("{:s} Cards".format(name))
            context.scene.collection.children.link(collection)
        collection.hide_render = True
        cards = []
        for v, points in enumerate(flat):
            lo, hi = boxes[v]
            center = (lo + hi) * 0.5
            length = max(hi.y, 1e-4)
            col, row = v % grid, v // grid
            corners = _outline(points, margin)
            verts = [(c.x / length, c.y / length, 0.0) for c in corners]
            mesh = bpy.data.meshes.new("{:s} Card {:d}".format(name, v + 1))
            mesh.from_pydata(verts, [], [list(range(len(verts)))])
            uv = mesh.uv_layers.new(name="UVMap")
            for loop in mesh.loops:
                c = corners[loop.vertex_index]
                local = Vector(((c.x - center.x) / size + 0.5, (c.y - center.y) / size + 0.5))
                uv.data[loop.index].uv = ((col + local.x) / grid, (row + local.y) / grid)
            mesh.materials.append(material)
            card = bpy.data.objects.new(mesh.name, mesh)
            card.matrix_world = ob.matrix_world @ Matrix.Translation(
                right * ((v + 1) * size * 1.1)) @ basis @ Matrix.Diagonal((length, length, length, 1.0))
            collection.objects.link(card)
            cards.append(card)

        for mesh in meshes:
            bpy.data.meshes.remove(mesh)
        self.report({'INFO'}, "Baked {:d} card(s) into \"{:s}\"".format(len(cards), material.name))
        return {'FINISHED'}


def _card_material(name, color, normal):
    """A material for baked cards: the color image with its see-through edges, and the normals."""
    material = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    material.use_nodes = True
    material.use_backface_culling = False
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    out = nodes.new("ShaderNodeOutputMaterial")
    out.location = (500.0, 0.0)
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.location = (200.0, 0.0)
    bsdf.inputs["Roughness"].default_value = 0.6
    tex = nodes.new("ShaderNodeTexImage")
    tex.location = (-300.0, 100.0)
    tex.image = color
    tex_n = nodes.new("ShaderNodeTexImage")
    tex_n.location = (-300.0, -250.0)
    tex_n.image = normal
    normal_map = nodes.new("ShaderNodeNormalMap")
    normal_map.location = (0.0, -250.0)
    links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
    links.new(tex_n.outputs["Color"], normal_map.inputs["Color"])
    links.new(normal_map.outputs["Normal"], bsdf.inputs["Normal"])
    links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    return material


class OBJECT_OT_tree_graph_new(Operator):
    """Make a new Tree Graph for the Tree modifier"""
    bl_idname = "object.tree_graph_new"
    bl_label = "New Tree Graph"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return _active_tree_modifier(context) is not None

    def execute(self, context):
        md = _active_tree_modifier(context)
        md.tree_graph = new_tree_graph('BROADLEAF')
        return {'FINISHED'}


class NODE_OT_new_tree_graph(Operator):
    """Make a new Tree Graph"""
    bl_idname = "node.new_tree_graph"
    bl_label = "New Tree Graph"
    bl_options = {'REGISTER', 'UNDO'}

    preset: EnumProperty(
        name="Start With",
        items=PRESET_ITEMS,
        default='OAK',
    )

    def execute(self, context):
        tree = new_tree_graph(self.preset)
        snode = context.space_data
        if snode and snode.type == 'NODE_EDITOR':
            snode.node_tree = tree
        return {'FINISHED'}


# Ready-made settings for one level node: (name, description, input values, curve).
# Inputs left out keep what they have, so links and objects stay as they are. They are taken from
# the trees, see _EZ_TREES.
NODE_PRESETS = {}


def _add_tree_node_presets():
    labels = {item[0]: item[1] for item in PRESET_ITEMS}
    for tree_id in ('OAK', 'BROADLEAF', 'PINE', 'BUSH'):
        ez = _EZ_TREES[tree_id]
        label = labels[tree_id]
        description = "From the {:s} tree".format(label)
        presets = [("TreeGraphNodeTrunk", label, _trunk_values(ez))]
        for n in range(1, len(ez["children"]) + 1):
            name = label if len(ez["children"]) == 1 else "{:s} {:d}".format(label, n)
            presets.append(("TreeGraphNodeBranches", name, _branch_values(ez, n)))
        presets.append(("TreeGraphNodeLeaves", label, _leaf_values(ez["leaves"])))
        for idname, name, values in presets:
            NODE_PRESETS[idname] = NODE_PRESETS.get(idname, ()) + ((name, description, values, None),)


_add_tree_node_presets()


def _context_node(context):
    node = getattr(context, "node", None)
    if node is None:
        node = context.active_node
    return node


class NODE_MT_tree_graph_node_presets(bpy.types.Menu):
    bl_label = "Presets"

    def draw(self, context):
        layout = self.layout
        node = _context_node(context)
        if node is None or node.bl_idname not in NODE_PRESETS:
            layout.label(text="No presets")
            return
        for index, (name, description, _values, _curve) in enumerate(NODE_PRESETS[node.bl_idname]):
            props = layout.operator("node.tree_graph_node_preset", text=name)
            props.node_name = node.name
            props.index = index


class NODE_OT_tree_graph_node_preset(Operator):
    """Fill in the settings of this node with ready-made ones"""
    bl_idname = "node.tree_graph_node_preset"
    bl_label = "Use Preset"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    node_name: bpy.props.StringProperty(options={'SKIP_SAVE'})
    index: IntProperty(options={'SKIP_SAVE'})

    @classmethod
    def description(cls, context, properties):
        snode = context.space_data
        tree = getattr(snode, "edit_tree", None)
        node = tree.nodes.get(properties.node_name) if tree else None
        presets = NODE_PRESETS.get(node.bl_idname, ()) if node else ()
        if 0 <= properties.index < len(presets):
            return presets[properties.index][1]
        return None

    @classmethod
    def poll(cls, context):
        snode = context.space_data
        return snode and snode.type == 'NODE_EDITOR' and snode.edit_tree is not None

    def execute(self, context):
        tree = context.space_data.edit_tree
        node = tree.nodes.get(self.node_name) or _context_node(context)
        if node is None or node.bl_idname not in NODE_PRESETS:
            return {'CANCELLED'}
        presets = NODE_PRESETS[node.bl_idname]
        if not 0 <= self.index < len(presets):
            return {'CANCELLED'}
        _name, _description, values, curve = presets[self.index]
        for name, value in values.items():
            socket = node.inputs.get(name)
            if socket is not None and hasattr(socket, "default_value"):
                _set_input(socket, value)
        builder = _GraphBuilder(tree)
        for i, name in enumerate(n for n in _RANDOM_SPREAD if n + " Random" in values):
            socket = node.inputs.get(name)
            if socket is not None and not socket.is_linked and values[name + " Random"] > 0.0:
                builder.add_random(node, name, socket.default_value, values[name + " Random"], i)
        if curve and hasattr(node, "mapping"):
            _set_curve(node, curve)
        return {'FINISHED'}


classes = (
    OBJECT_OT_tree_bake_cards,
    NODE_MT_tree_graph_node_presets,
    NODE_OT_tree_graph_node_preset,
    OBJECT_OT_tree_add,
    OBJECT_OT_tree_graph_new,
    NODE_OT_new_tree_graph,
)
