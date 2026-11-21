# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Panel, Menu, Operator
from bpy.app.translations import (
    pgettext_n as n_,
)


class ModifierButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "modifier"
    bl_options = {'HIDE_HEADER'}


class ModifierAddMenu:
    MODIFIER_TYPES_TO_LABELS = {
        enum_it.identifier: enum_it.name
        for enum_it in bpy.types.Modifier.bl_rna.properties["type"].enum_items_static
    }
    MODIFIER_TYPES_TO_ICONS = {
        enum_it.identifier: enum_it.icon
        for enum_it in bpy.types.Modifier.bl_rna.properties["type"].enum_items_static
    }
    MODIFIER_TYPES_I18N_CONTEXT = bpy.types.Modifier.bl_rna.properties["type"].translation_context

    @classmethod
    def operator_modifier_add(cls, layout, mod_type, text=None, no_icon=False):
        label = text if text else cls.MODIFIER_TYPES_TO_LABELS[mod_type]
        layout.operator(
            "object.modifier_add",
            text=label,
            # Although these are operators, the label actually comes from an (enum) property,
            # so the property's translation context must be used here.
            text_ctxt=cls.MODIFIER_TYPES_I18N_CONTEXT,
            icon='NONE' if no_icon else cls.MODIFIER_TYPES_TO_ICONS[mod_type],
        ).type = mod_type

    @classmethod
    def operator_modifier_add_new(cls, layout, mod_types):
        # Missing from builds without these modifiers, which may share these scripts.
        for mod_type in mod_types:
            if mod_type in cls.MODIFIER_TYPES_TO_LABELS:
                cls.operator_modifier_add(layout, mod_type)

    @classmethod
    def operator_modifier_add_asset(cls, layout, name, icon='NONE'):
        props = layout.operator(
            "object.modifier_add_node_group",
            text=name,
            text_ctxt=cls.MODIFIER_TYPES_I18N_CONTEXT,
            icon=icon,
        )
        props.asset_library_type = 'ESSENTIALS'
        props.asset_library_identifier = ""
        props.relative_asset_identifier = "nodes/geometry_nodes_essentials.blend/NodeTree/" + name


class DATA_PT_modifiers(ModifierButtonsPanel, Panel):
    bl_label = "Modifiers"

    @classmethod
    def poll(cls, context):
        ob = context.object
        return ob

    def draw(self, context):
        layout = self.layout
        space = context.space_data
        if space is None or space.type != 'PROPERTIES':
            layout.operator("wm.call_menu", text="Add Modifier", icon='ADD').name = "OBJECT_MT_modifier_add"
            layout.template_modifiers()
            return

        from bl_ui import properties_data_modifier_list as modifier_list
        modifier_list.draw_header(layout, context, space)
        if space.use_modifier_list:
            modifier_list.draw_list(layout, context, context.object)
            layout.template_modifiers(active_only=True)
        else:
            layout.template_modifiers()


class OBJECT_MT_modifier_add(ModifierAddMenu, Menu):
    bl_label = "Add Modifier"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, context):
        layout = self.layout
        ob = context.object
        if not ob:
            return
        ob_type = ob.type
        geometry_nodes_supported = ob_type in {
            'MESH', 'CURVE', 'CURVES',
            'FONT', 'VOLUME', 'POINTCLOUD', 'GREASEPENCIL',
        }

        if layout.operator_context == 'EXEC_REGION_WIN':
            layout.operator_context = 'INVOKE_REGION_WIN'
            layout.operator(
                "WM_OT_search_single_menu",
                text="Search...",
                icon='VIEWZOOM',
            ).menu_idname = "OBJECT_MT_modifier_add"
            layout.separator()

        layout.operator_context = 'INVOKE_REGION_WIN'

        if ob_type in {'MESH', 'CURVE', 'CURVES', 'FONT', 'SURFACE', 'LATTICE', 'GREASEPENCIL', 'POINTCLOUD'}:
            layout.menu("OBJECT_MT_modifier_add_edit")
        if ob_type in {'MESH', 'CURVE', 'CURVES', 'FONT', 'SURFACE', 'VOLUME', 'GREASEPENCIL'}:
            layout.menu("OBJECT_MT_modifier_add_generate")
        if ob_type in {'MESH', 'CURVE', 'CURVES', 'FONT', 'SURFACE', 'LATTICE', 'VOLUME', 'GREASEPENCIL'}:
            layout.menu("OBJECT_MT_modifier_add_deform")
        if ob_type in {'MESH'}:
            layout.menu("OBJECT_MT_modifier_add_normals")
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE', 'LATTICE'}:
            layout.menu("OBJECT_MT_modifier_add_physics")
        if ob_type in {'GREASEPENCIL'}:
            layout.menu("OBJECT_MT_modifier_add_color")

        if geometry_nodes_supported:
            layout.menu_contents("OBJECT_MT_modifier_add_root_catalogs")

        if geometry_nodes_supported:
            layout.separator()
            self.operator_modifier_add(layout, 'NODES')


class OBJECT_MT_modifier_add_edit(ModifierAddMenu, Menu):
    bl_label = "Edit"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, context):
        layout = self.layout
        ob_type = context.object.type
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'DATA_TRANSFER')
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE', 'LATTICE'}:
            self.operator_modifier_add(layout, 'MESH_CACHE')
        if ob_type in {'MESH', 'CURVE', 'CURVES', 'FONT', 'POINTCLOUD'}:
            self.operator_modifier_add(layout, 'MESH_SEQUENCE_CACHE')
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'UV_PROJECT')
            self.operator_modifier_add(layout, 'UV_WARP')
            self.operator_modifier_add_new(layout, ('UV_SCALE', 'UV_UNWRAP', 'UV_PACK'))
            self.operator_modifier_add(layout, 'VERTEX_WEIGHT_EDIT')
            self.operator_modifier_add(layout, 'VERTEX_WEIGHT_MIX')
            self.operator_modifier_add(layout, 'VERTEX_WEIGHT_PROXIMITY')
            self.operator_modifier_add_new(layout, (
                'AUTO_WEIGHTS', 'SMOOTH_WEIGHTS', 'TRANSFER_WEIGHTS', 'BONE_COLORS',
            ))
            self.operator_modifier_add_new(layout, (
                'UVW_MAP', 'TENSION', 'MASKS', 'VOLUME_SELECT', 'MATERIAL_BY_ELEMENT', 'MATERIALIZE', 'DATA_CHANNEL', 'PIECE_ID', 'EDIT_POLY', 'POLY_SELECT', 'POLY_EDIT',
                'ELEMENT_ORDER', 'ELEMENT_PIVOT', 'SMART_UV_PROJECT',
            ))
        if ob_type == 'GREASEPENCIL':
            self.operator_modifier_add(layout, 'GREASE_PENCIL_TEXTURE')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_TIME')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_VERTEX_WEIGHT_PROXIMITY')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_VERTEX_WEIGHT_ANGLE')
        layout.template_modifier_asset_menu_items(catalog_path=self.bl_label)


class OBJECT_MT_modifier_add_generate(ModifierAddMenu, Menu):
    bl_label = "Generate"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, context):
        layout = self.layout
        ob_type = context.object.type
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE'}:
            self.operator_modifier_add_asset(layout, 'Array', icon='MOD_ARRAY')
            self.operator_modifier_add(layout, 'ARRAY', text=n_("Array (Legacy)"), no_icon=True)
            self.operator_modifier_add(layout, 'BEVEL')
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'BOOLEAN')
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE'}:
            self.operator_modifier_add(layout, 'BUILD')
            self.operator_modifier_add_asset(layout, n_('Curve to Tube'), icon='MOD_CURVE_TO_TUBE')
            self.operator_modifier_add(layout, 'DECIMATE')
            self.operator_modifier_add(layout, 'EDGE_SPLIT')
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'MASK')
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE'}:
            self.operator_modifier_add(layout, 'MIRROR')
        if ob_type == 'VOLUME':
            self.operator_modifier_add(layout, 'MESH_TO_VOLUME')
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'MULTIRES')
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE'}:
            self.operator_modifier_add(layout, 'REMESH')
        if ob_type == 'MESH':
            # Missing from builds without these modifiers, which may share these scripts.
            for mod_type in ('ISOTROPIC_REMESH', 'QUADRIFLOW_REMESH'):
                if mod_type in self.MODIFIER_TYPES_TO_LABELS:
                    self.operator_modifier_add(layout, mod_type)
            self.operator_modifier_add_new(layout, (
                'SLICE', 'CAP_HOLES', 'RADIAL_ARRAY', 'FACE_EXTRUDE', 'DELETE_MESH', 'QUADRANGULATE',
                'PANELIZE', 'INSET', 'POKE', 'BEAUTIFY_FACES', 'ROTATE_EDGES', 'SYMMETRIZE',
                'VORONOI_FRACTURE', 'DICE', 'COLLAPSE', 'DRAPE', 'SHATTER', 'MASONRY', 'STONEWORK', 'DISTRIBUTE', 'PLANKS', 'ROOF', 'EDGE_DAMAGE', 'DIVIDE', 'JOIN', 'BLAST', 'DISSOLVE', 'CLEANUP',
                'SWEEP', 'TRIM', 'TILE', 'PATCH', 'PACK', 'ROCK', 'SNOW', 'TREE', 'RESAMPLE', 'LEAN', 'CHISEL', 'FACESET', 'WRAP',
                'PLACE', 'SCATTER',
                'DENT', 'CONTACT', 'CABLE', 'CRACKS', 'COLLISION_HULL', 'BRICKS', 'UV_ROTATE',
            ))
        if ob_type == 'CURVES':
            self.operator_modifier_add_new(layout, ('NORMALIZE_SPLINE', 'CHAMFER_SPLINE', 'SWEEP'))
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE'}:
            self.operator_modifier_add_asset(layout, n_('Scatter on Surface'), icon='MOD_SCATTER_ON_SURFACE')
            self.operator_modifier_add(layout, 'SCREW')
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'SKIN')
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE'}:
            self.operator_modifier_add(layout, 'SOLIDIFY')
            self.operator_modifier_add(layout, 'SUBSURF')
            self.operator_modifier_add(layout, 'TRIANGULATE')
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'VOLUME_TO_MESH')
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE'}:
            self.operator_modifier_add(layout, 'WELD')
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'WIREFRAME')
        if ob_type == 'GREASEPENCIL':
            self.operator_modifier_add(layout, 'GREASE_PENCIL_ARRAY')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_BUILD')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_DASH')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_ENVELOPE')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_LENGTH')
            self.operator_modifier_add(layout, 'LINEART')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_MIRROR')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_MULTIPLY')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_OUTLINE')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_SIMPLIFY')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_SUBDIV')
        layout.template_modifier_asset_menu_items(catalog_path=self.bl_label, skip_essentials=True)


class OBJECT_MT_modifier_add_deform(ModifierAddMenu, Menu):
    bl_label = "Deform"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, context):
        layout = self.layout
        ob_type = context.object.type
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE', 'LATTICE'}:
            self.operator_modifier_add(layout, 'ARMATURE')
            self.operator_modifier_add(layout, 'CAST')
            self.operator_modifier_add(layout, 'CURVE')
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'DISPLACE')
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE', 'LATTICE'}:
            self.operator_modifier_add(layout, 'HOOK')
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'LAPLACIANDEFORM')
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE', 'LATTICE'}:
            self.operator_modifier_add(layout, 'LATTICE')
            self.operator_modifier_add(layout, 'MESH_DEFORM')
            self.operator_modifier_add(layout, 'SHRINKWRAP')
            self.operator_modifier_add(layout, 'SIMPLE_DEFORM')
            self.operator_modifier_add_new(layout, (
                'BEND', 'TWIST', 'TAPER', 'STRETCH', 'NOISE', 'FFD_2', 'FFD_3', 'FFD_4', 'MELT',
            ))
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE'}:
            self.operator_modifier_add(layout, 'SMOOTH')
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'CORRECTIVE_SMOOTH')
            self.operator_modifier_add(layout, 'LAPLACIANSMOOTH')
            self.operator_modifier_add(layout, 'SURFACE_DEFORM')
            self.operator_modifier_add_new(layout, (
                'RELAX', 'SHAPE_KEY_INBETWEEN', 'RANDOMIZE_ELEMENTS', 'EXPLODED_VIEW', 'PUSH', 'CARVE',
                'CONFORM',
            ))
        if ob_type == 'CURVES':
            self.operator_modifier_add_new(layout, ('RELAX_SPLINE', 'SPLINE_MORPH'))
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE', 'LATTICE'}:
            self.operator_modifier_add(layout, 'WARP')
            self.operator_modifier_add(layout, 'WAVE')
        if ob_type == 'VOLUME':
            self.operator_modifier_add(layout, 'VOLUME_DISPLACE')
        if ob_type == 'GREASEPENCIL':
            self.operator_modifier_add(layout, 'GREASE_PENCIL_ARMATURE')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_HOOK')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_LATTICE')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_NOISE')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_OFFSET')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_SHRINKWRAP')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_SMOOTH')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_THICKNESS')
        layout.template_modifier_asset_menu_items(catalog_path=self.bl_label)


class OBJECT_MT_modifier_add_normals(ModifierAddMenu, Menu):
    bl_label = "Normals"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, context):
        layout = self.layout
        ob_type = context.object.type
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'NORMAL_EDIT')
            self.operator_modifier_add(layout, 'WEIGHTED_NORMAL')
            self.operator_modifier_add_new(layout, (
                'NORMAL_TRANSFER', 'NORMAL_SMOOTH', 'NORMAL_FACE_SETS', 'NORMAL_SHAPE', 'MARK_BY_ANGLE',
            ))
        layout.template_modifier_asset_menu_items(catalog_path=self.bl_label)


class OBJECT_MT_modifier_add_physics(ModifierAddMenu, Menu):
    bl_label = "Physics"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, context):
        layout = self.layout
        ob_type = context.object.type
        if ob_type == 'MESH':
            self.operator_modifier_add(layout, 'CLOTH')
            self.operator_modifier_add(layout, 'COLLISION')
            self.operator_modifier_add(layout, 'DYNAMIC_PAINT')
            self.operator_modifier_add(layout, 'EXPLODE')
            self.operator_modifier_add(layout, 'FLUID')
            self.operator_modifier_add(layout, 'OCEAN')
            self.operator_modifier_add(layout, 'PARTICLE_INSTANCE')
            self.operator_modifier_add(layout, 'PARTICLE_SYSTEM')
        if ob_type in {'MESH', 'CURVE', 'FONT', 'SURFACE', 'LATTICE'}:
            self.operator_modifier_add(layout, 'SOFT_BODY')
        layout.template_modifier_asset_menu_items(catalog_path=self.bl_label)


class OBJECT_MT_modifier_add_color(ModifierAddMenu, Menu):
    bl_label = "Color"
    bl_options = {'SEARCH_ON_KEY_PRESS'}

    def draw(self, context):
        layout = self.layout
        ob_type = context.object.type
        if ob_type == 'GREASEPENCIL':
            self.operator_modifier_add(layout, 'GREASE_PENCIL_COLOR')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_OPACITY')
            self.operator_modifier_add(layout, 'GREASE_PENCIL_TINT')
        layout.template_modifier_asset_menu_items(catalog_path=self.bl_label)


def _collider_modifier_from_context(context, name):
    owner = context.object
    modifier = owner.modifiers.get(name) if owner else None
    return modifier if modifier and modifier.type in {'DRAPE', 'COLLAPSE', 'LEAN'} else None


class OBJECT_OT_collider_source_add(Operator):
    bl_idname = "object.collider_source_add"
    bl_label = "Add Collider"
    bl_description = "Add an object or collection to land on"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    modifier: bpy.props.StringProperty(options={'HIDDEN'})
    source_type: bpy.props.EnumProperty(items=(
        ('OBJECT', "Object", "One mesh object", 0),
        ('COLLECTION', "Collection", "Every mesh object in a collection", 1),
    ))

    def execute(self, context):
        modifier = _collider_modifier_from_context(context, self.modifier)
        if modifier is None:
            return {'CANCELLED'}
        if len(modifier.colliders) >= 512:
            self.report({'WARNING'}, "At most 512 colliders")
            return {'CANCELLED'}
        modifier.colliders.new(source_type=self.source_type)
        return {'FINISHED'}


class OBJECT_OT_collider_source_remove(Operator):
    bl_idname = "object.collider_source_remove"
    bl_label = "Remove Collider"
    bl_description = "Remove this collider, keeping its object or collection"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    modifier: bpy.props.StringProperty(options={'HIDDEN'})
    index: bpy.props.IntProperty(default=-1, options={'HIDDEN'})

    def execute(self, context):
        modifier = _collider_modifier_from_context(context, self.modifier)
        if modifier is None or not 0 <= self.index < len(modifier.colliders):
            return {'CANCELLED'}
        modifier.colliders.remove(modifier.colliders[self.index])
        return {'FINISHED'}


class OBJECT_OT_collider_source_add_selected(Operator):
    bl_idname = "object.collider_source_add_selected"
    bl_label = "Add Selected Colliders"
    bl_description = "Add the selected mesh objects as colliders, without the active object"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    modifier: bpy.props.StringProperty(options={'HIDDEN'})

    def execute(self, context):
        modifier = _collider_modifier_from_context(context, self.modifier)
        if modifier is None:
            return {'CANCELLED'}
        single = modifier.collider if modifier.type == 'DRAPE' else modifier.target
        existing = {source.object for source in modifier.colliders
                    if source.source_type == 'OBJECT' and source.object is not None}
        existing.add(single)
        added = 0
        for obj in context.selected_objects:
            if obj == context.object or obj.type != 'MESH' or obj in existing:
                continue
            if len(modifier.colliders) >= 512:
                self.report({'WARNING'}, "At most 512 colliders")
                break
            source = modifier.colliders.new(source_type='OBJECT')
            source.object = obj
            existing.add(obj)
            added += 1
        if not added:
            self.report({'INFO'}, "No other selected mesh objects")
            return {'CANCELLED'}
        return {'FINISHED'}


def _join_modifier_from_context(context, name):
    owner = context.object
    modifier = owner.modifiers.get(name) if owner else None
    return modifier if modifier and modifier.type == 'JOIN' else None


class OBJECT_OT_join_modifier_source_add(Operator):
    bl_idname = "object.join_modifier_source_add"
    bl_label = "Add Join Source"
    bl_description = "Add an object or collection to this Join modifier"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    modifier: bpy.props.StringProperty(options={'HIDDEN'})
    source_type: bpy.props.EnumProperty(items=(
        ('OBJECT', "Object", "Join one evaluated mesh object", 0),
        ('COLLECTION', "Collection", "Join mesh objects in a collection", 1),
    ))

    def execute(self, context):
        modifier = _join_modifier_from_context(context, self.modifier)
        if modifier is None:
            return {'CANCELLED'}
        if len(modifier.sources) >= 512:
            self.report({'WARNING'}, "Join supports at most 512 source entries")
            return {'CANCELLED'}
        modifier.sources.new(source_type=self.source_type)
        return {'FINISHED'}


class OBJECT_OT_join_modifier_source_remove(Operator):
    bl_idname = "object.join_modifier_source_remove"
    bl_label = "Remove Join Source"
    bl_description = "Remove this source from Join without deleting its object or collection"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    modifier: bpy.props.StringProperty(options={'HIDDEN'})
    index: bpy.props.IntProperty(default=-1, options={'HIDDEN'})

    def execute(self, context):
        modifier = _join_modifier_from_context(context, self.modifier)
        if modifier is None or not 0 <= self.index < len(modifier.sources):
            return {'CANCELLED'}
        modifier.sources.remove(modifier.sources[self.index])
        return {'FINISHED'}


class OBJECT_OT_join_modifier_source_add_selected(Operator):
    bl_idname = "object.join_modifier_source_add_selected"
    bl_label = "Add Selected Objects to Join"
    bl_description = "Add selected mesh objects as individual sources, excluding the receiving object"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    modifier: bpy.props.StringProperty(options={'HIDDEN'})

    def execute(self, context):
        modifier = _join_modifier_from_context(context, self.modifier)
        if modifier is None:
            return {'CANCELLED'}
        existing = {source.object for source in modifier.sources
                    if source.source_type == 'OBJECT' and source.object is not None}
        added = 0
        for obj in context.selected_objects:
            if obj == context.object or obj.type != 'MESH' or obj in existing:
                continue
            if len(modifier.sources) >= 512:
                self.report({'WARNING'}, "Join source-entry limit reached")
                break
            source = modifier.sources.new(source_type='OBJECT')
            source.object = obj
            existing.add(obj)
            added += 1
        if not added:
            self.report({'INFO'}, "No additional selected mesh objects")
            return {'CANCELLED'}
        return {'FINISHED'}


def _pack_modifier_from_context(context, name):
    owner = context.object
    modifier = owner.modifiers.get(name) if owner else None
    return modifier if modifier and modifier.type == 'PACK' else None


class OBJECT_OT_pack_object_add(Operator):
    bl_idname = "object.pack_object_add"
    bl_label = "Add Object"
    bl_description = "Add a slot for one more object to pack"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    modifier: bpy.props.StringProperty(options={'HIDDEN'})

    def execute(self, context):
        modifier = _pack_modifier_from_context(context, self.modifier)
        if modifier is None:
            return {'CANCELLED'}
        modifier.objects.new()
        return {'FINISHED'}


class OBJECT_OT_pack_object_remove(Operator):
    bl_idname = "object.pack_object_remove"
    bl_label = "Remove Object"
    bl_description = "Remove this object from the Pack modifier, without deleting it"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    modifier: bpy.props.StringProperty(options={'HIDDEN'})
    index: bpy.props.IntProperty(default=-1, options={'HIDDEN'})

    def execute(self, context):
        modifier = _pack_modifier_from_context(context, self.modifier)
        if modifier is None or not 0 <= self.index < len(modifier.objects):
            return {'CANCELLED'}
        modifier.objects.remove(modifier.objects[self.index])
        return {'FINISHED'}


def _array_modifier_from_context(context, name):
    owner = context.object
    modifier = owner.modifiers.get(name) if owner else None
    return modifier if modifier and modifier.type in {'ARRAY', 'RADIAL_ARRAY', 'PLACE', 'SCATTER'} else None


class OBJECT_OT_array_object_add(Operator):
    bl_idname = "object.array_object_add"
    bl_label = "Add Object"
    bl_description = "Add a slot for one more object to place"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    modifier: bpy.props.StringProperty(options={'HIDDEN'})

    def execute(self, context):
        modifier = _array_modifier_from_context(context, self.modifier)
        if modifier is None:
            return {'CANCELLED'}
        modifier.objects.new()
        return {'FINISHED'}


class OBJECT_OT_array_object_remove(Operator):
    bl_idname = "object.array_object_remove"
    bl_label = "Remove Object"
    bl_description = "Remove this object from the list, without deleting it"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    modifier: bpy.props.StringProperty(options={'HIDDEN'})
    index: bpy.props.IntProperty(default=-1, options={'HIDDEN'})

    def execute(self, context):
        modifier = _array_modifier_from_context(context, self.modifier)
        if modifier is None or not 0 <= self.index < len(modifier.objects):
            return {'CANCELLED'}
        modifier.objects.remove(modifier.objects[self.index])
        return {'FINISHED'}


class AddModifierMenu(Operator):
    bl_idname = "object.add_modifier_menu"
    bl_label = "Add Modifier"

    @classmethod
    def poll(cls, context):
        # NOTE: This operator only exists to add a poll to the add modifier shortcut in the property editor.
        space = context.space_data
        return space and space.type == 'PROPERTIES' and space.context == 'MODIFIER'

    def invoke(self, _context, _event):
        return bpy.ops.wm.call_menu(name="OBJECT_MT_modifier_add")


class OBJECT_OT_wrap_add(Operator):
    bl_idname = "object.wrap_add"
    bl_label = "Wrap"
    bl_description = "Add straps wrapped round the active object. Draw or edit their lines as edges"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return context.mode == 'OBJECT' and ob is not None and ob.type == 'MESH'

    def execute(self, context):
        target = context.active_object
        mesh = bpy.data.meshes.new("Wrap")
        ob = bpy.data.objects.new("Wrap", mesh)
        ob.matrix_world = target.matrix_world.copy()
        collections = target.users_collection or (context.collection,)
        for collection in collections:
            collection.objects.link(ob)
        modifier = ob.modifiers.new("Wrap", 'WRAP')
        modifier.target = target
        modifier.lines = 'BOTH'
        for selected in context.selected_objects:
            selected.select_set(False)
        ob.select_set(True)
        context.view_layer.objects.active = ob
        return {'FINISHED'}


class OBJECT_OT_wrap_to_edges(Operator):
    bl_idname = "object.wrap_to_edges"
    bl_label = "To Edges"
    bl_description = "Turn the pattern lines into edges of the mesh, to edit them by hand"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    def execute(self, context):
        import bmesh
        ob = context.object
        modifier = getattr(context, "modifier", None)
        if modifier is None and ob is not None:
            modifier = ob.modifiers.active
        if ob is None or ob.type != 'MESH' or modifier is None or modifier.type != 'WRAP':
            return {'CANCELLED'}
        was_edit = ob.mode == 'EDIT'
        if was_edit:
            bpy.ops.object.mode_set(mode='OBJECT')

        # Evaluate up to the wrap only, with the wrap giving its pattern lines.
        hidden = []
        after = False
        for other in ob.modifiers:
            if after and other.show_viewport:
                other.show_viewport = False
                hidden.append(other)
            if other == modifier:
                after = True
        modifier.use_lines_only = True
        depsgraph = context.evaluated_depsgraph_get()
        depsgraph.update()
        ob_eval = ob.evaluated_get(depsgraph)
        lines = ob_eval.to_mesh()
        coords = [v.co.copy() for v in lines.vertices]
        edges = [tuple(e.vertices) for e in lines.edges]
        ob_eval.to_mesh_clear()
        modifier.use_lines_only = False
        for other in hidden:
            other.show_viewport = True

        if edges:
            bm = bmesh.new()
            bm.from_mesh(ob.data)
            verts = [bm.verts.new(co) for co in coords]
            for a, b in edges:
                bm.edges.new((verts[a], verts[b]))
            bm.to_mesh(ob.data)
            bm.free()
            ob.data.update()
            modifier.lines = 'EDGES'
        else:
            self.report({'WARNING'}, "No pattern lines to turn into edges")

        if was_edit:
            bpy.ops.object.mode_set(mode='EDIT')
        return {'FINISHED'}


classes = (
    OBJECT_OT_pack_object_add,
    OBJECT_OT_pack_object_remove,
    OBJECT_OT_array_object_add,
    OBJECT_OT_array_object_remove,
    OBJECT_OT_wrap_add,
    OBJECT_OT_wrap_to_edges,
    OBJECT_OT_join_modifier_source_add,
    OBJECT_OT_join_modifier_source_remove,
    OBJECT_OT_join_modifier_source_add_selected,
    OBJECT_OT_collider_source_add,
    OBJECT_OT_collider_source_remove,
    OBJECT_OT_collider_source_add_selected,
    DATA_PT_modifiers,
    OBJECT_MT_modifier_add,
    OBJECT_MT_modifier_add_edit,
    OBJECT_MT_modifier_add_generate,
    OBJECT_MT_modifier_add_deform,
    OBJECT_MT_modifier_add_normals,
    OBJECT_MT_modifier_add_physics,
    OBJECT_MT_modifier_add_color,
    AddModifierMenu,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class

    for cls in classes:
        register_class(cls)
