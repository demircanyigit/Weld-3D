# SPDX-FileCopyrightText: 2026 Weld 3D Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Library of the node editors: every node of the Add menu, by category, left of the toolbar.

The categories are read from the Add menus themselves, so the Library always lists the same nodes.
"""

import bpy
from bpy.app.translations import pgettext_iface as iface_
from bpy.props import StringProperty
from bpy.types import Operator, Panel

from .node_add_menu import menu_path_icon

ROOT_MENUS = {
    'GeometryNodeTree': "NODE_MT_geometry_node_add_all",
    'CompositorNodeTree': "NODE_MT_compositor_node_add_all",
    'ShaderNodeTree': "NODE_MT_shader_node_add_all",
    'TextureNodeTree': "NODE_MT_texture_node_add_all",
    'TextureGraphNodeTree': "NODE_MT_texture_graph_node_add_all",
    'TreeGraphNodeTree': "NODE_MT_tree_graph_node_add_all",
}
# Opened first, before a category is picked.
DEFAULT_CATEGORY = {
    'GeometryNodeTree': "Mesh",
    'ShaderNodeTree': "Shader",
    'CompositorNodeTree': "Filter",
    'TextureGraphNodeTree': "Texture/Noise",
    'TreeGraphNodeTree': "Tree",
}
# Node groups come from the asset system, they have no fixed list to show.
SKIP_MENUS = {"Group"}
TILE_WIDTH = 4.6
# Own icons for categories whose name alone is not enough.
PATH_ICONS = {
    "Curve/Primitives": 'NODE_LIB_CURVE_PRIMITIVES',
    "Mesh/Primitives": 'NODE_LIB_MESH_PRIMITIVES',
    "Volume/Primitives": 'NODE_LIB_VOLUME_PRIMITIVES',
}

_cache = {}
_active = {}


# -----------------------------------------------------------------------------
# Reading the Add menus

class _Props:
    """Stands in for operator properties while a menu is read."""

    def __init__(self):
        object.__setattr__(self, "_values", {})

    def __setattr__(self, name, value):
        self._values[name] = value

    def __getattr__(self, name):
        if name == "settings":
            return _Settings()
        return self._values.get(name)


class _Settings:
    def add(self):
        return _Props()


class _Recorder:
    """Stands in for a layout and notes what a menu draws, in order."""

    def __init__(self):
        object.__setattr__(self, "items", [])

    def __setattr__(self, name, value):
        pass

    def operator(self, operator, *, text="", icon='NONE', **_kw):
        props = _Props()
        self.items.append(("operator", operator, text, icon, props))
        return props

    def menu(self, menu, *, text="", icon='NONE', **_kw):
        self.items.append(("menu", menu, text, icon))

    def menu_contents(self, menu):
        self.items.append(("contents", menu))

    def separator(self, *_args, **_kw):
        self.items.append(("separator",))

    def __getattr__(self, _name):
        # Rows, columns, labels and the rest: nested parts are read into the same list.
        return lambda *_args, **_kw: self


class _MenuSelf:
    """The `self` of a menu's draw function: its class methods, with a recording layout."""

    def __init__(self, cls, layout):
        self._cls = cls
        self.layout = layout

    def __getattr__(self, name):
        return getattr(self._cls, name)


def _read_menu(context, idname):
    cls = getattr(bpy.types, idname, None)
    if cls is None or not hasattr(cls, "draw"):
        return []
    recorder = _Recorder()
    try:
        cls.draw(_MenuSelf(cls, recorder), context)
    except Exception as ex:
        print("Node Library: could not read menu", idname, ex)
    return recorder.items


def _icon_values():
    items = bpy.types.UILayout.bl_rna.functions["prop"].parameters["icon"].enum_items
    return {item.identifier: item.value for item in items}


_icons = {}


def _icon_table():
    if not _icons:
        _icons.update(_icon_values())
    return _icons


def _category(context, idname, name, path, icons, depth=0):
    category = {"name": name, "path": path, "nodes": [], "children": []}
    if depth > 6:
        return category
    seen = set()
    for item in _read_menu(context, idname):
        kind = item[0]
        if kind == "menu":
            sub = getattr(bpy.types, item[1], None)
            if sub is None:
                continue
            label = sub.bl_label
            if label in SKIP_MENUS:
                continue
            child = _category(context, item[1], label, (path + "/" if path else "") + label, icons, depth + 1)
            if child["nodes"] or child["children"]:
                category["children"].append(child)
        elif kind == "contents":
            inner = _category(context, item[1], name, path, icons, depth + 1)
            category["nodes"] += [n for n in inner["nodes"] if (n["type"], n["label"]) not in seen]
            category["children"] += inner["children"]
        elif kind == "operator" and item[1] == "node.add_node":
            node_type = item[4].type
            if not node_type:
                continue
            bl_rna = bpy.types.Node.bl_rna_get_subclass(node_type)
            label = item[2] or (bl_rna.name if bl_rna else node_type)
            if (node_type, label) in seen:
                continue
            seen.add((node_type, label))
            category["nodes"].append({
                "type": node_type,
                "label": label,
                "icon": icons.get(item[3], 0),
                "description": bl_rna.description if bl_rna else "",
            })
    return category


def _tree_key(snode):
    return (snode.tree_type, getattr(snode, "shader_type", ""))


def _library(context):
    snode = context.space_data
    key = _tree_key(snode)
    lib = _cache.get(key)
    if lib is None:
        root = ROOT_MENUS.get(snode.tree_type)
        lib = _category(context, root, "", "", _icon_values())["children"] if root else []
        _cache[key] = lib
    return lib


def _find(lib, path):
    for category in lib:
        if category["path"] == path:
            return category
        hit = _find(category["children"], path)
        if hit:
            return hit
    return None


def _all_nodes(lib, out=None):
    out = [] if out is None else out
    for category in lib:
        for node in category["nodes"]:
            out.append((node, category["path"]))
        _all_nodes(category["children"], out)
    return out


def _active_path(snode, lib):
    key = _tree_key(snode)
    path = _active.get(key)
    if path is None or _find(lib, path) is None:
        path = DEFAULT_CATEGORY.get(snode.tree_type)
        if path is None or _find(lib, path) is None:
            path = lib[0]["path"] if lib else ""
        _active[key] = path
    return path


def _category_icon(path, icons):
    """The category's own icon, by its path or its name."""
    name = PATH_ICONS.get(path) or "NODE_LIB_" + path.rpartition("/")[2].upper().replace(" ", "_")
    value = icons.get(name)
    if value is None:
        value = icons.get(menu_path_icon(path), icons.get('DOT', 0))
    return value


def _tree_items(categories, active, icons, depth=0, out=None):
    """One line per category for `template_node_library_tree`."""
    out = [] if out is None else out
    for category in categories:
        path = category["path"]
        name = iface_(category["name"]).replace("\t", " ")
        out.append("\t".join((
            str(depth),
            path,
            name,
            str(_category_icon(path, icons)),
            "1" if path == active else "0",
            "1" if active.startswith(path + "/") else "0",
        )))
        _tree_items(category["children"], active, icons, depth + 1, out)
    return out


# -----------------------------------------------------------------------------
# Operators

class NODE_OT_library_category(Operator):
    """Show the nodes of this category"""
    bl_idname = "node.library_category"
    bl_label = "Show Category"
    bl_options = {'INTERNAL'}

    path: StringProperty()

    def execute(self, context):
        _active[_tree_key(context.space_data)] = self.path
        context.window_manager.node_library_search = ""
        context.area.tag_redraw()
        return {'FINISHED'}


class NODE_OT_library_refresh(Operator):
    """Read the node menus again, for nodes added by add-ons"""
    bl_idname = "node.library_refresh"
    bl_label = "Refresh Library"
    bl_options = {'INTERNAL'}

    def execute(self, context):
        _cache.clear()
        context.area.tag_redraw()
        return {'FINISHED'}


# -----------------------------------------------------------------------------
# Panels

def _poll(context):
    snode = context.space_data
    return snode is not None and snode.tree_type in ROOT_MENUS


def _draw_nodes(layout, nodes, as_list):
    if as_list:
        col = layout.column(align=True)
        for node in nodes:
            col.template_node_tile(
                node_type=node["type"],
                text=iface_(node["label"]),
                icon_value=node["icon"],
                tooltip=node["description"],
                width=10.0,
                as_row=True,
            )
        return
    grid = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=True, align=True)
    for node in nodes:
        grid.template_node_tile(
            node_type=node["type"],
            text=iface_(node["label"]),
            icon_value=node["icon"],
            tooltip=node["description"],
            width=TILE_WIDTH,
        )


class NODE_PT_library(Panel):
    """The categories, as a tree."""
    bl_space_type = 'NODE_EDITOR'
    bl_region_type = 'CHANNELS'
    bl_label = "Library"
    bl_options = {'HIDE_HEADER'}

    @classmethod
    def poll(cls, context):
        return _poll(context)

    def draw(self, context):
        layout = self.layout
        lib = _library(context)
        icons = _icon_table()

        row = layout.row(align=True)
        row.label(text="Library", icon_value=icons.get('NODE_LIBRARY', 0))
        row.operator(NODE_OT_library_refresh.bl_idname, text="", icon='FILE_REFRESH', emboss=False)

        if not lib:
            layout.label(text="No nodes to list here")
            return
        active = _active_path(context.space_data, lib)
        layout.template_node_library_tree(categories="\n".join(_tree_items(lib, active, icons)))


class NODE_PT_library_nodes(Panel):
    """The nodes of the category picked in the tree, or the search results."""
    bl_space_type = 'NODE_EDITOR'
    bl_region_type = 'NAVIGATION_BAR'
    bl_label = "Nodes"
    bl_options = {'HIDE_HEADER'}

    @classmethod
    def poll(cls, context):
        return _poll(context)

    def draw(self, context):
        layout = self.layout
        snode = context.space_data
        wm = context.window_manager
        lib = _library(context)
        as_list = wm.node_library_view == 'LIST'

        row = layout.row(align=True)
        row.prop(wm, "node_library_search", text="", icon='VIEWZOOM')
        row.separator(factor=0.5)
        row.prop(wm, "node_library_view", expand=True, icon_only=True)

        if not lib:
            return

        query = wm.node_library_search.strip().lower()
        if query:
            words = query.split()
            hits = [
                node for node, path in _all_nodes(lib)
                if all(w in node["label"].lower() or w in path.lower() for w in words)
            ]
            hits.sort(key=lambda n: (not n["label"].lower().startswith(query), n["label"]))
            unique = []
            seen = set()
            for node in hits:
                if (node["type"], node["label"]) not in seen:
                    seen.add((node["type"], node["label"]))
                    unique.append(node)
            layout.label(text=iface_("{:d} results").format(len(unique)))
            if unique:
                _draw_nodes(layout, unique, as_list)
            else:
                layout.label(text="No nodes match the search", icon='INFO')
            return

        category = _find(lib, _active_path(snode, lib))
        if category is None:
            return
        layout.label(text=iface_(category["name"]))
        if category["nodes"]:
            _draw_nodes(layout, category["nodes"], as_list)

        def sections(children, prefix):
            for child in children:
                name = prefix + iface_(child["name"])
                if child["nodes"]:
                    layout.separator()
                    layout.label(text=name)
                    _draw_nodes(layout, child["nodes"], as_list)
                sections(child["children"], name + " › ")
        sections(category["children"], "")


def _redraw_editors(_self, context):
    for window in context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'NODE_EDITOR':
                area.tag_redraw()


classes = (
    NODE_OT_library_category,
    NODE_OT_library_refresh,
    NODE_PT_library,
    NODE_PT_library_nodes,
)


def register_props():
    from bpy.props import EnumProperty
    bpy.types.WindowManager.node_library_search = StringProperty(
        name="Search",
        description="Find nodes by name or category",
        options={'TEXTEDIT_UPDATE', 'SKIP_SAVE'},
        update=_redraw_editors,
    )
    icons = _icon_values()
    bpy.types.WindowManager.node_library_view = EnumProperty(
        name="View",
        description="How the nodes of the Library are shown",
        items=(
            ('GRID', "Grid", "Show the nodes as tiles with large icons",
             icons.get('NODE_LIBRARY_VIEW_GRID', 0), 0),
            ('LIST', "List", "Show the nodes as a list of names",
             icons.get('NODE_LIBRARY_VIEW_LIST', 0), 1),
        ),
        default='GRID',
        update=_redraw_editors,
    )


if __name__ == "__main__":  # only for live edit.
    register_props()
    for cls in classes:
        bpy.utils.register_class(cls)
