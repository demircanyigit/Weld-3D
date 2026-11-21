# SPDX-FileCopyrightText: 2026 Weld 3D Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Texture Graph operators: make a new graph, update its images, and make a material that uses
# them. Each output of a graph is written to an image named "<Graph> <Output>".

import bpy
from bpy.types import Operator
from bpy.props import EnumProperty
from bpy.app.translations import pgettext_data as data_


TREE_TYPE = "TextureGraphNodeTree"


def image_name(tree, output_name):
    return "{:s} {:s}".format(tree.name, output_name)


def graph_outputs(tree):
    return [item for item in tree.interface.items_tree
            if item.item_type == 'SOCKET' and item.in_out == 'OUTPUT']


def active_texture_graph(context):
    snode = context.space_data
    if snode is None or snode.type != 'NODE_EDITOR' or snode.tree_type != TREE_TYPE:
        return None
    return snode.node_tree


# Outputs the material understands, by lower case output name.
# Value: (Principled BSDF input, how to connect).
MATERIAL_INPUTS = {
    "base color": ("Base Color", 'COLOR'),
    "color": ("Base Color", 'COLOR'),
    "albedo": ("Base Color", 'COLOR'),
    "diffuse": ("Base Color", 'COLOR'),
    "metallic": ("Metallic", 'VALUE'),
    "metalness": ("Metallic", 'VALUE'),
    "roughness": ("Roughness", 'VALUE'),
    "normal": ("Normal", 'NORMAL'),
    "height": (None, 'HEIGHT'),
    "displacement": (None, 'HEIGHT'),
    "opacity": ("Alpha", 'VALUE'),
    "alpha": ("Alpha", 'VALUE'),
    "emission": ("Emission Color", 'EMISSION'),
    "emissive": ("Emission Color", 'EMISSION'),
    "specular": ("Specular IOR Level", 'VALUE'),
    "subsurface": ("Subsurface Weight", 'VALUE'),
    "ambient occlusion": (None, 'AO'),
    "ao": (None, 'AO'),
}


def _build_pbr_template(tree):
    """A small working graph: noise drives the height, normal and colors."""
    interface = tree.interface
    sockets = [
        ("Base Color", 'NodeSocketColor'),
        ("Metallic", 'NodeSocketFloat'),
        ("Roughness", 'NodeSocketFloat'),
        ("Normal", 'NodeSocketColor'),
        ("Height", 'NodeSocketFloat'),
    ]
    for name, socket_type in sockets:
        interface.new_socket(name=name, in_out='OUTPUT', socket_type=socket_type)

    nodes = tree.nodes
    links = tree.links

    output = nodes.new("NodeGroupOutput")
    output.location = (700, 0)

    noise = nodes.new("ShaderNodeTexNoise")
    noise.location = (-500, 0)
    noise.inputs["Scale"].default_value = 6.0
    noise.inputs["Detail"].default_value = 8.0

    levels = nodes.new("TextureGraphNodeLevels")
    levels.location = (-250, 0)

    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.location = (50, 250)
    ramp.color_ramp.elements[0].color = (0.08, 0.06, 0.05, 1.0)
    ramp.color_ramp.elements[1].color = (0.55, 0.45, 0.35, 1.0)

    normal = nodes.new("TextureGraphNodeHeightToNormal")
    normal.location = (50, -150)

    roughness = nodes.new("ShaderNodeValue")
    roughness.location = (300, 50)
    roughness.label = "Roughness"
    roughness.outputs[0].default_value = 0.6

    metallic = nodes.new("ShaderNodeValue")
    metallic.location = (300, 150)
    metallic.label = "Metallic"
    metallic.outputs[0].default_value = 0.0

    links.new(noise.outputs["Fac"], levels.inputs["Image"])
    links.new(levels.outputs["Image"], ramp.inputs["Fac"])
    links.new(levels.outputs["Image"], normal.inputs["Height"])
    links.new(ramp.outputs["Color"], output.inputs["Base Color"])
    links.new(metallic.outputs[0], output.inputs["Metallic"])
    links.new(roughness.outputs[0], output.inputs["Roughness"])
    links.new(normal.outputs["Normal"], output.inputs["Normal"])
    links.new(levels.outputs["Image"], output.inputs["Height"])


def _build_mask_template(tree):
    tree.interface.new_socket(name="Mask", in_out='OUTPUT', socket_type='NodeSocketFloat')
    output = tree.nodes.new("NodeGroupOutput")
    output.location = (300, 0)
    noise = tree.nodes.new("ShaderNodeTexNoise")
    noise.location = (0, 0)
    tree.links.new(noise.outputs["Fac"], output.inputs["Mask"])


def _build_empty_template(tree):
    tree.interface.new_socket(name="Base Color", in_out='OUTPUT', socket_type='NodeSocketColor')
    output = tree.nodes.new("NodeGroupOutput")
    output.location = (300, 0)


class NODE_OT_new_texture_graph(Operator):
    """Make a new Texture Graph"""
    bl_idname = "node.new_texture_graph"
    bl_label = "New Texture Graph"
    bl_options = {'REGISTER', 'UNDO'}

    template: EnumProperty(
        name="Start With",
        items=(
            ('PBR', "Material", "Base color, metallic, roughness, normal and height outputs"),
            ('MASK', "Mask", "A single grayscale output"),
            ('EMPTY', "Empty", "Only a base color output"),
        ),
        default='PBR',
    )

    def execute(self, context):
        tree = bpy.data.node_groups.new(name=data_("Texture Graph"), type=TREE_TYPE)
        if self.template == 'PBR':
            _build_pbr_template(tree)
        elif self.template == 'MASK':
            _build_mask_template(tree)
        else:
            _build_empty_template(tree)

        snode = context.space_data
        if snode and snode.type == 'NODE_EDITOR':
            snode.node_tree = tree
        tree.update_textures()
        return {'FINISHED'}


class NODE_OT_texture_graph_update(Operator):
    """Compute the graph and write its output images"""
    bl_idname = "node.texture_graph_update"
    bl_label = "Update Images"
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        return active_texture_graph(context) is not None

    def execute(self, context):
        active_texture_graph(context).update_textures()
        return {'FINISHED'}


def _image_node(nodes, image, location, is_data):
    node = nodes.new("ShaderNodeTexImage")
    node.image = image
    node.location = location
    node.hide = True
    if is_data:
        image.colorspace_settings.is_data = True
    return node


class NODE_OT_texture_graph_create_material(Operator):
    """Make a material that uses the images of this graph, and give it to the active object"""
    bl_idname = "node.texture_graph_create_material"
    bl_label = "Create Material"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return active_texture_graph(context) is not None

    def execute(self, context):
        tree = active_texture_graph(context)
        tree.update_textures()

        material = bpy.data.materials.new(name=tree.name)
        if material.node_tree is None:
            material.use_nodes = True
        nodes = material.node_tree.nodes
        links = material.node_tree.links

        bsdf = next((n for n in nodes if n.type == 'BSDF_PRINCIPLED'), None)
        out = next((n for n in nodes if n.type == 'OUTPUT_MATERIAL'), None)
        if bsdf is None:
            bsdf = nodes.new("ShaderNodeBsdfPrincipled")
        if out is None:
            out = nodes.new("ShaderNodeOutputMaterial")
            links.new(bsdf.outputs[0], out.inputs["Surface"])
        out.location = (bsdf.location.x + 400, bsdf.location.y)

        x = bsdf.location.x - 600
        y = bsdf.location.y + 100
        missing = []
        used = 0
        for item in graph_outputs(tree):
            image = bpy.data.images.get(image_name(tree, item.name))
            if image is None:
                missing.append(item.name)
                continue
            target, kind = MATERIAL_INPUTS.get(item.name.lower(), (None, None))
            if kind is None:
                continue
            is_data = kind not in {'COLOR', 'EMISSION'}
            tex = _image_node(nodes, image, (x, y), is_data)
            y -= 60
            used += 1

            if kind in {'COLOR', 'VALUE', 'EMISSION'}:
                links.new(tex.outputs["Color"], bsdf.inputs[target])
                if kind == 'EMISSION':
                    bsdf.inputs["Emission Strength"].default_value = 1.0
                if target == "Alpha":
                    material.surface_render_method = 'DITHERED'
            elif kind == 'NORMAL':
                normal_map = nodes.new("ShaderNodeNormalMap")
                normal_map.location = (x + 300, y + 60)
                links.new(tex.outputs["Color"], normal_map.inputs["Color"])
                links.new(normal_map.outputs["Normal"], bsdf.inputs["Normal"])
            elif kind == 'HEIGHT':
                displacement = nodes.new("ShaderNodeDisplacement")
                displacement.location = (out.location.x - 200, out.location.y - 300)
                displacement.inputs["Midlevel"].default_value = 0.5
                displacement.inputs["Scale"].default_value = 0.05
                links.new(tex.outputs["Color"], displacement.inputs["Height"])
                links.new(displacement.outputs["Displacement"], out.inputs["Displacement"])
                material.displacement_method = 'BOTH'
            elif kind == 'AO':
                # Kept in the material for reference and export, not wired: AO is computed by the
                # renderer.
                tex.label = item.name

        obj = context.object
        if obj is not None and obj.type in {'MESH', 'CURVE', 'SURFACE', 'FONT', 'META'}:
            if obj.active_material_index < len(obj.material_slots) and obj.material_slots:
                obj.material_slots[obj.active_material_index].material = material
            else:
                obj.data.materials.append(material)
            self.report({'INFO'}, "Made material \"{:s}\" for {:s}".format(material.name, obj.name))
        else:
            self.report({'INFO'}, "Made material \"{:s}\"".format(material.name))

        if missing:
            self.report({'WARNING'}, "No image for: " + ", ".join(missing))
        if used == 0:
            self.report({'WARNING'}, "No graph output has a name the material understands")
        return {'FINISHED'}


# Marks graphs made as the function of a Pixel or Value Processor. They are used inside other
# graphs and make no images of their own.
FUNCTION_KEY = "texture_graph_function"


def build_processor(kind):
    """
    A new graph to use as a function: it runs once for every pixel (PIXEL), or once for the whole
    image (VALUE). It starts reading its input at the pixel's own position, or in the middle.
    """
    is_pixel = kind == 'PIXEL'
    group = bpy.data.node_groups.new(
        name=data_("Pixel Processor") if is_pixel else data_("Value Processor"), type=TREE_TYPE)
    group[FUNCTION_KEY] = kind
    interface = group.interface
    interface.new_socket(name="Input", in_out='INPUT', socket_type='NodeSocketColor')
    if is_pixel:
        interface.new_socket(name="Color", in_out='OUTPUT', socket_type='NodeSocketColor')
    else:
        interface.new_socket(name="Value", in_out='OUTPUT', socket_type='NodeSocketFloat')

    nodes = group.nodes
    links = group.links
    group_input = nodes.new("NodeGroupInput")
    group_input.location = (-450, 0)
    sample = nodes.new("TextureGraphNodeSample")
    sample.location = (-50, 0)
    group_output = nodes.new("NodeGroupOutput")
    group_output.is_active_output = True
    group_output.location = (250, 0)
    links.new(group_input.outputs["Input"], sample.inputs["Image"])

    if is_pixel:
        position = nodes.new("TextureGraphNodePosition")
        position.location = (-450, -150)
        links.new(position.outputs["Position"], sample.inputs["Position"])
        links.new(sample.outputs["Color"], group_output.inputs["Color"])
    else:
        links.new(sample.outputs["Value"], group_output.inputs["Value"])
    for node in nodes:
        node.select = False
    return group


class NODE_OT_texture_graph_add_processor(Operator):
    """Add a node that runs a function made of nodes, for every pixel or once for the image"""
    bl_idname = "node.texture_graph_add_processor"
    bl_label = "Add Processor"
    bl_options = {'REGISTER', 'UNDO'}

    kind: EnumProperty(
        name="Kind",
        items=(
            ('PIXEL', "Pixel Processor", "The function runs for every pixel and makes an image"),
            ('VALUE', "Value Processor", "The function runs once and makes a single value"),
        ),
        default='PIXEL',
    )
    use_transform: bpy.props.BoolProperty(
        name="Use Transform",
        description="Move the new node with the mouse after adding it",
        default=False,
    )

    @classmethod
    def description(cls, _context, properties):
        if properties.kind == 'VALUE':
            return "Add a node that runs a function made of nodes once, giving a single value"
        return "Add a node that runs a function made of nodes for every pixel"

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return (space and space.type == 'NODE_EDITOR' and space.edit_tree and
                space.edit_tree.bl_idname == TREE_TYPE and space.edit_tree.is_editable)

    def invoke(self, context, event):
        from bl_operators.node import NodeAddOperator
        NodeAddOperator.store_mouse_cursor(context, event)
        result = self.execute(context)
        if self.use_transform and 'FINISHED' in result:
            bpy.ops.node.translate_attach_remove_on_cancel('INVOKE_DEFAULT')
        return result

    def execute(self, context):
        space = context.space_data
        tree = space.edit_tree
        group = build_processor(self.kind)
        for n in tree.nodes:
            n.select = False
        node = tree.nodes.new("TextureGraphNodeGroup")
        node.node_tree = group
        node.label = group.name
        node.location = space.cursor_location
        node.select = True
        tree.nodes.active = node
        return {'FINISHED'}


@bpy.app.handlers.persistent
def _texture_graphs_load_post(_):
    # The output images are generated images, they come back empty when a file is opened.
    for tree in bpy.data.node_groups:
        if tree.bl_idname == TREE_TYPE and not tree.library and FUNCTION_KEY not in tree:
            try:
                tree.update_textures()
            except Exception as ex:
                print("Texture Graph \"{:s}\" could not be updated: {!s}".format(tree.name, ex))


def register():
    bpy.app.handlers.load_post.append(_texture_graphs_load_post)


def unregister():
    if _texture_graphs_load_post in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_texture_graphs_load_post)


classes = (
    NODE_OT_new_texture_graph,
    NODE_OT_texture_graph_update,
    NODE_OT_texture_graph_create_material,
    NODE_OT_texture_graph_add_processor,
)
