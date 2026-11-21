# SPDX-FileCopyrightText: 2026 Weld 3D Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Add and Swap menus of the Tree Graph editor. Tree nodes are levels of the tree, shape nodes bend,
# trim and prune them, profiles change their numbers along a curve.

from bl_ui import node_add_menu
from bpy.app.translations import (
    contexts as i18n_contexts,
)


class NODE_MT_tree_graph_node_output_base(node_add_menu.NodeMenu):
    bl_label = "Output"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TreeGraphNodeOutput")


class NODE_MT_tree_graph_node_tree_base(node_add_menu.NodeMenu):
    bl_label = "Tree"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TreeGraphNodeTrunk")
        self.node_operator(layout, "TreeGraphNodeBranches")
        self.node_operator(layout, "TreeGraphNodeLeaves")
        self.node_operator(layout, "TreeGraphNodeFronds")
        self.node_operator(layout, "TreeGraphNodeIvy")
        self.node_operator(layout, "TreeGraphNodeRoots")
        layout.separator()
        self.node_operator(layout, "TreeGraphNodeDrawn")


class NODE_MT_tree_graph_node_shape_base(node_add_menu.NodeMenu):
    bl_label = "Shape"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TreeGraphNodeForce")
        self.node_operator(layout, "TreeGraphNodeAvoid")
        self.node_operator(layout, "TreeGraphNodeCrown")
        self.node_operator(layout, "TreeGraphNodePrune")


class NODE_MT_tree_graph_node_surface_base(node_add_menu.NodeMenu):
    bl_label = "Surface"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TreeGraphNodeBark")


class NODE_MT_tree_graph_node_input_base(node_add_menu.NodeMenu):
    bl_label = "Input"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "NodeGroupInput")
        layout.separator()
        self.node_operator(layout, "TreeGraphNodeBranchInfo")
        self.node_operator(layout, "TreeGraphNodeObjectInfo")
        layout.separator()
        self.node_operator(layout, "FunctionNodeInputBool")
        self.node_operator(layout, "FunctionNodeInputInt")
        self.node_operator(layout, "ShaderNodeValue")
        self.node_operator(layout, "FunctionNodeInputVector")


class NODE_MT_tree_graph_node_utilities_base(node_add_menu.NodeMenu):
    bl_label = "Utilities"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TreeGraphNodeProfile")
        self.node_operator(layout, "ShaderNodeFloatCurve")
        self.node_operator(layout, "FunctionNodeRandomValue")
        layout.separator()
        self.node_operator(layout, "ShaderNodeMath")
        self.node_operator(layout, "ShaderNodeVectorMath")
        self.node_operator(layout, "ShaderNodeMapRange")
        self.node_operator(layout, "ShaderNodeClamp")
        self.node_operator(layout, "ShaderNodeMix")
        layout.separator()
        self.node_operator(layout, "ShaderNodeCombineXYZ")
        self.node_operator(layout, "ShaderNodeSeparateXYZ")
        layout.separator()
        self.node_operator(layout, "FunctionNodeCompare")
        self.node_operator(layout, "FunctionNodeBooleanMath")
        self.node_operator(layout, "FunctionNodeFloatToInt")


class NODE_MT_tree_graph_node_texture_base(node_add_menu.NodeMenu):
    bl_label = "Texture"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "ShaderNodeTexNoise")
        self.node_operator(layout, "ShaderNodeTexWhiteNoise")


class NODE_MT_tree_graph_node_all_base(node_add_menu.NodeMenu):
    bl_label = ""
    menu_path = "Root"
    bl_translation_context = i18n_contexts.operator_default

    def draw(self, context):
        del context
        layout = self.layout
        self.draw_menu(layout, "Input")
        self.draw_menu(layout, "Tree")
        self.draw_menu(layout, "Shape")
        self.draw_menu(layout, "Surface")
        self.draw_menu(layout, "Texture")
        self.draw_menu(layout, "Utilities")
        self.draw_menu(layout, "Output")
        layout.separator()
        self.draw_menu(layout, "Layout")


add_menus = node_add_menu.generate_menus(
    {
        "NODE_MT_category_tree_graph_output": NODE_MT_tree_graph_node_output_base,
        "NODE_MT_category_tree_graph_tree": NODE_MT_tree_graph_node_tree_base,
        "NODE_MT_category_tree_graph_shape": NODE_MT_tree_graph_node_shape_base,
        "NODE_MT_category_tree_graph_surface": NODE_MT_tree_graph_node_surface_base,
        "NODE_MT_category_tree_graph_utilities": NODE_MT_tree_graph_node_utilities_base,
        "NODE_MT_category_tree_graph_input": NODE_MT_tree_graph_node_input_base,
        "NODE_MT_category_tree_graph_texture": NODE_MT_tree_graph_node_texture_base,
        "NODE_MT_tree_graph_node_add_all": NODE_MT_tree_graph_node_all_base,
    },
    template=node_add_menu.AddNodeMenu,
    base_dict=node_add_menu.add_base_pathing_dict,
)

swap_menus = node_add_menu.generate_menus(
    {
        "NODE_MT_tree_graph_node_output_swap": NODE_MT_tree_graph_node_output_base,
        "NODE_MT_tree_graph_node_tree_swap": NODE_MT_tree_graph_node_tree_base,
        "NODE_MT_tree_graph_node_shape_swap": NODE_MT_tree_graph_node_shape_base,
        "NODE_MT_tree_graph_node_surface_swap": NODE_MT_tree_graph_node_surface_base,
        "NODE_MT_tree_graph_node_utilities_swap": NODE_MT_tree_graph_node_utilities_base,
        "NODE_MT_tree_graph_node_input_swap": NODE_MT_tree_graph_node_input_base,
        "NODE_MT_tree_graph_node_texture_swap": NODE_MT_tree_graph_node_texture_base,
        "NODE_MT_tree_graph_node_swap_all": NODE_MT_tree_graph_node_all_base,
    },
    template=node_add_menu.SwapNodeMenu,
    base_dict=node_add_menu.swap_base_pathing_dict,
)


classes = (
    *add_menus,
    *swap_menus,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
