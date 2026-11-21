# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from bpy.types import Operator
from bpy.props import (
    EnumProperty,
    FloatProperty,
    IntProperty,
)


class MeshSelectNext(Operator):
    """Select the next element (using selection order)"""
    bl_idname = "mesh.select_next_item"
    bl_label = "Select Next Element"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return (context.mode == 'EDIT_MESH')

    def execute(self, context):
        import bmesh
        from .bmesh import find_adjacent

        obj = context.active_object
        me = obj.data
        bm = bmesh.from_edit_mesh(me)

        if find_adjacent.select_next(bm, self.report):
            bm.select_flush_mode()
            bmesh.update_edit_mesh(me, loop_triangles=False)

        return {'FINISHED'}


class MeshSelectPrev(Operator):
    """Select the previous element (using selection order)"""
    bl_idname = "mesh.select_prev_item"
    bl_label = "Select Previous Element"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return (context.mode == 'EDIT_MESH')

    def execute(self, context):
        import bmesh
        from .bmesh import find_adjacent

        obj = context.active_object
        me = obj.data
        bm = bmesh.from_edit_mesh(me)

        if find_adjacent.select_prev(bm, self.report):
            bm.select_flush_mode()
            bmesh.update_edit_mesh(me, loop_triangles=False)

        return {'FINISHED'}


class MeshSelectRandomIslands(Operator):
    """Select or deselect a random share of the loose parts"""
    bl_idname = "mesh.select_random_islands"
    bl_label = "Select Random Islands"
    bl_options = {'REGISTER', 'UNDO'}

    ratio: FloatProperty(
        name="Ratio",
        description="Share of the parts to select or deselect",
        min=0.0, max=1.0,
        default=0.5,
        subtype='FACTOR',
    )
    seed: IntProperty(
        name="Random Seed",
        description="Picks other parts",
        min=0,
    )
    action: EnumProperty(
        name="Action",
        items=(
            ('SELECT', "Select", "Select the picked parts"),
            ('DESELECT', "Deselect", "Deselect the picked parts"),
        ),
    )

    @classmethod
    def poll(cls, context):
        return (context.mode == 'EDIT_MESH')

    def execute(self, context):
        import bmesh
        import random

        select = self.action == 'SELECT'
        for index, obj in enumerate(context.objects_in_mode_unique_data):
            me = obj.data
            bm = bmesh.from_edit_mesh(me)
            verts = [v for v in bm.verts if not v.hide]
            if not verts:
                continue
            # Parts: points joined by visible edges.
            parent = {v: v for v in verts}

            def root(v):
                while parent[v] is not v:
                    parent[v] = parent[parent[v]]
                    v = parent[v]
                return v

            for e in bm.edges:
                if e.hide:
                    continue
                a, b = root(e.verts[0]), root(e.verts[1])
                if a is not b:
                    parent[a] = b
            parts = {}
            for v in verts:
                parts.setdefault(root(v), []).append(v)
            parts = sorted(parts.values(), key=lambda part: min(v.index for v in part))
            rng = random.Random(self.seed * 1000003 + index)
            rng.shuffle(parts)
            picked = parts[:round(len(parts) * self.ratio)]
            for part in picked:
                for v in part:
                    v.select_set(select)
                    for e in v.link_edges:
                        if not e.hide:
                            e.select_set(select)
                    for f in v.link_faces:
                        if not f.hide:
                            f.select_set(select)
            bm.select_flush_mode()
            bmesh.update_edit_mesh(me, loop_triangles=False, destructive=False)
        return {'FINISHED'}


classes = (
    MeshSelectNext,
    MeshSelectPrev,
    MeshSelectRandomIslands,
)
