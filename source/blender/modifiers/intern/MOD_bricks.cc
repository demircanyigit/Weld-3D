/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Bricks: cut the mesh into solid blocks laid in courses, the way a stone wall is built (Cut), or
 * rebuild a closed mesh from bricks on a grid, laid in rows (Stepped). Each brick is its own piece
 * with a gap around it.
 */

#include <algorithm>
#include <cfloat>

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "GEO_mesh_bricks.hh"
#include "GEO_mesh_sim_cache.hh"
#include "GEO_randomize.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

namespace blender {

static void init_data(ModifierData *md)
{
  BricksModifierData *data = reinterpret_cast<BricksModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

static constexpr int64_t MAX_CELLS = 4000000;
static constexpr int MAX_BRICKS = 200000;

/** True when the point is inside the closed mesh: the nearest face looks away from it. */
static bool point_inside(const bke::BVHTreeFromMesh &bvh, const float3 &point)
{
  BVHTreeNearest nearest;
  nearest.index = -1;
  nearest.dist_sq = FLT_MAX;
  BLI_bvhtree_find_nearest(
      bvh.tree, point, &nearest, bvh.nearest_callback, const_cast<bke::BVHTreeFromMesh *>(&bvh));
  if (nearest.index == -1) {
    return false;
  }
  return math::dot(point - float3(nearest.co), float3(nearest.no)) < 0.0f;
}

struct Brick {
  float3 min;
  float3 max;
};

/** Internal face attribute used to find the cut faces for the interior material. */
static const char *BRICKS_INTERIOR_ATTRIBUTE = ".bricks_interior";

/** Cut mode: the mesh itself cut into blocks. */
static Mesh *modify_mesh_cut(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const BricksModifierData &bmd = *reinterpret_cast<BricksModifierData *>(md);
  if (mesh->faces_num == 0) {
    return mesh;
  }
  const bool use_interior_material = bmd.flag & MOD_BRICKS_INTERIOR_MATERIAL;
  const int interior_material = use_interior_material ?
                                    MOD_material_slot(*mesh,
                                                      bmd.interior_material_pick,
                                                      bmd.interior_material) :
                                    0;
  geometry::MeshBricksParams params;
  params.pattern = geometry::BricksPattern(std::clamp(int(bmd.pattern), 0, 4));
  params.size = float3(bmd.size);
  params.gap = bmd.gap;
  params.random_length = bmd.random_length;
  params.random_height = bmd.random_height;
  params.uneven = bmd.uneven;
  params.bevel = bmd.bevel;
  params.bevel_segments = bmd.bevel_segments;
  params.seed = bmd.seed;
  if (use_interior_material) {
    params.interior_id = BRICKS_INTERIOR_ATTRIBUTE;
  }
  if (bmd.flag & MOD_BRICKS_PIECE_DATA) {
    params.piece_index_id = "Piece Index";
    params.piece_random_id = "Piece Random";
  }

  geometry::MeshBricksError error;
  Mesh *result = geometry::mesh_bricks(*mesh, params, error);
  if (error == geometry::MeshBricksError::TooManyCells) {
    BKE_modifier_set_error(ctx->object, md, "The bricks are too small for this mesh");
  }
  else if (error == geometry::MeshBricksError::TooManyBlocks) {
    BKE_modifier_set_error(
        ctx->object, md, "Too many bricks, made the first %d", geometry::mesh_bricks_max_blocks);
  }
  if (result == nullptr) {
    return mesh;
  }
  MOD_material_slots_from(*result, *mesh);

  if (use_interior_material) {
    bke::MutableAttributeAccessor attributes = result->attributes_for_write();
    {
      const VArraySpan<bool> interior = *attributes.lookup_or_default<bool>(
          BRICKS_INTERIOR_ATTRIBUTE, bke::AttrDomain::Face, false);
      bke::SpanAttributeWriter<int> material_index =
          attributes.lookup_or_add_for_write_span<int>("material_index", bke::AttrDomain::Face);
      if (material_index) {
        for (const int face : interior.index_range()) {
          if (interior[face]) {
            material_index.span[face] = interior_material;
          }
        }
        material_index.finish();
      }
    }
    attributes.remove(BRICKS_INTERIOR_ATTRIBUTE);
  }

  geometry::debug_randomize_mesh_order(result);
  return result;
}

/** Stepped mode: the mesh rebuilt from whole bricks on a grid. */
static Mesh *modify_mesh_stepped(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const BricksModifierData &bmd = *reinterpret_cast<BricksModifierData *>(md);
  if (mesh->faces_num == 0) {
    return mesh;
  }
  const std::optional<Bounds<float3>> bounds = mesh->bounds_min_max();
  if (!bounds) {
    return mesh;
  }
  /* The grid is in half bricks along X, so rows can be offset by half a brick. */
  const float3 brick = math::max(float3(bmd.size), float3(1e-4f));
  const float3 cell(brick.x * 0.5f, brick.y, brick.z);
  const float3 size = bounds->max - bounds->min;
  const int3 dims(std::max(int(std::ceil(size.x / cell.x)), 1),
                  std::max(int(std::ceil(size.y / cell.y)), 1),
                  std::max(int(std::ceil(size.z / cell.z)), 1));
  const int64_t cells_num = int64_t(dims.x) * dims.y * dims.z;
  if (cells_num > MAX_CELLS) {
    BKE_modifier_set_error(ctx->object, md, "The bricks are too small for this mesh");
    return mesh;
  }
  /* Center the grid on the mesh. */
  const float3 origin = (bounds->min + bounds->max) * 0.5f - float3(dims) * cell * 0.5f;
  const auto index = [&](const int x, const int y, const int z) {
    return (int64_t(z) * dims.y + y) * dims.x + x;
  };

  Array<bool> filled(cells_num, false);
  {
    const bke::BVHTreeFromMesh bvh = mesh->bvh_corner_tris();
    if (bvh.tree == nullptr) {
      return mesh;
    }
    threading::parallel_for(IndexRange(dims.z), 1, [&](const IndexRange range) {
      for (const int z : range) {
        for (const int y : IndexRange(dims.y)) {
          for (const int x : IndexRange(dims.x)) {
            const float3 center = origin + (float3(float(x), float(y), float(z)) + 0.5f) * cell;
            filled[index(x, y, z)] = point_inside(bvh, center);
          }
        }
      }
    });
  }

  /* Hollow: only the cells with an empty cell beside them. */
  Array<bool> used(filled);
  if (bmd.flag & MOD_BRICKS_HOLLOW) {
    for (const int z : IndexRange(dims.z)) {
      for (const int y : IndexRange(dims.y)) {
        for (const int x : IndexRange(dims.x)) {
          if (!filled[index(x, y, z)]) {
            continue;
          }
          const auto empty = [&](const int nx, const int ny, const int nz) {
            if (nx < 0 || ny < 0 || nz < 0 || nx >= dims.x || ny >= dims.y || nz >= dims.z) {
              return true;
            }
            return !filled[index(nx, ny, nz)];
          };
          used[index(x, y, z)] = empty(x - 1, y, z) || empty(x + 1, y, z) ||
                                 empty(x, y - 1, z) || empty(x, y + 1, z) ||
                                 empty(x, y, z - 1) || empty(x, y, z + 1);
        }
      }
    }
  }

  /* Lay each row: runs of cells cut into whole bricks of two cells, every other row starting
   * with a half brick. */
  Vector<Brick> bricks;
  const float half_gap = std::max(bmd.gap, 0.0f) * 0.5f;
  for (const int z : IndexRange(dims.z)) {
    const bool shifted = (bmd.flag & MOD_BRICKS_OFFSET) && (z % 2 == 1);
    for (const int y : IndexRange(dims.y)) {
      int x = 0;
      while (x < dims.x) {
        if (!used[index(x, y, z)]) {
          x++;
          continue;
        }
        /* Brick joints line up on the grid of whole bricks, moved by one cell on shifted rows. */
        const bool joint_after_one = ((x + (shifted ? 1 : 0)) % 2) == 1;
        int length = 1;
        if (!joint_after_one && x + 1 < dims.x && used[index(x + 1, y, z)]) {
          length = 2;
        }
        const float3 min = origin + float3(float(x), float(y), float(z)) * cell;
        const float3 max = origin + float3(float(x + length), float(y + 1), float(z + 1)) * cell;
        bricks.append({min + float3(half_gap), max - float3(half_gap)});
        x += length;
        if (bricks.size() >= MAX_BRICKS) {
          break;
        }
      }
    }
  }
  if (bricks.size() >= MAX_BRICKS) {
    BKE_modifier_set_error(ctx->object, md, "Too many bricks, made the first %d", MAX_BRICKS);
  }
  if (bricks.is_empty()) {
    BKE_modifier_set_error(ctx->object, md, "No bricks fit, the mesh must be closed");
    return mesh;
  }

  const int bricks_num = bricks.size();
  Mesh *result = BKE_mesh_new_nomain(bricks_num * 8, 0, bricks_num * 6, bricks_num * 24);
  BKE_mesh_copy_parameters_for_eval(result, mesh);
  MutableSpan<float3> positions = result->vert_positions_for_write();
  MutableSpan<int> offsets = result->face_offsets_for_write();
  MutableSpan<int> corner_verts = result->corner_verts_for_write();
  static const int box_faces[6][4] = {
      {0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  threading::parallel_for(IndexRange(bricks_num), 4096, [&](const IndexRange range) {
    for (const int b : range) {
      const Brick &brick_box = bricks[b];
      for (const int i : IndexRange(8)) {
        positions[b * 8 + i] = float3(i & 1 ? brick_box.max.x : brick_box.min.x,
                                      i & 2 ? brick_box.max.y : brick_box.min.y,
                                      i & 4 ? brick_box.max.z : brick_box.min.z);
      }
      for (const int f : IndexRange(6)) {
        offsets[b * 6 + f] = (b * 6 + f) * 4;
        for (const int k : IndexRange(4)) {
          corner_verts[(b * 6 + f) * 4 + k] = b * 8 + box_faces[f][k];
        }
      }
    }
  });
  offsets.last() = bricks_num * 24;
  bke::mesh_calc_edges(*result, false, false);
  bke::mesh_smooth_set(*result, false);

  if (bmd.flag & MOD_BRICKS_PIECE_DATA) {
    bke::MutableAttributeAccessor attributes = result->attributes_for_write();
    bke::SpanAttributeWriter<int> piece = attributes.lookup_or_add_for_write_only_span<int>(
        "Piece Index", bke::AttrDomain::Point);
    bke::SpanAttributeWriter<float> random = attributes.lookup_or_add_for_write_only_span<float>(
        "Piece Random", bke::AttrDomain::Point);
    if (piece && random) {
      for (const int b : IndexRange(bricks_num)) {
        const float3 c = (bricks[b].min + bricks[b].max) * 0.5f;
        const float value = noise::hash_float_to_float(
            float4(c.x, c.y, c.z, float(bmd.seed) * 1.618f));
        piece.span.slice(b * 8, 8).fill(b);
        random.span.slice(b * 8, 8).fill(value);
      }
    }
    piece.finish();
    random.finish();
  }
  result->tag_positions_changed();
  return result;
}

/* Cut mode is taken from the cache when nothing it depends on changed, see
 * #MOD_cached_modify_mesh. */
static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const BricksModifierData &bmd = *reinterpret_cast<BricksModifierData *>(md);
  if (bmd.mode != MOD_BRICKS_MODE_CUT) {
    return modify_mesh_stepped(md, ctx, mesh);
  }
  if (bmd.flag & MOD_BRICKS_INTERIOR_MATERIAL) {
    MOD_material_slot(*mesh, bmd.interior_material_pick, 0);
  }
  return MOD_cached_modify_mesh(md, ctx, mesh, modify_mesh_cut);
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  BricksModifierData *pick_data = reinterpret_cast<BricksModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&pick_data->interior_material_pick), IDWALK_CB_USER);
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  layout.prop(ptr, "mode", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.use_property_split_set(true);
  const bool cut = RNA_enum_get(ptr, "mode") == MOD_BRICKS_MODE_CUT;
  if (cut) {
    layout.prop(ptr, "pattern", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "size", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "gap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (cut) {
    layout.prop(ptr, "random_length", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "random_height", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "uneven", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "bevel", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    ui::Layout &segments = layout.row(true);
    segments.active_set(RNA_float_get(ptr, "bevel") > 0.0f);
    segments.prop(ptr, "bevel_segments", UI_ITEM_NONE, IFACE_("Segments"), ICON_NONE);
    layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    ui::Layout &row = layout.row(true, IFACE_("Interior Material"));
    row.prop(ptr, "use_interior_material", UI_ITEM_NONE, "", ICON_NONE);
    ui::Layout &sub = row.row(true);
    sub.active_set(RNA_boolean_get(ptr, "use_interior_material"));
    modifier_material_ui(sub, ptr, "interior_material_pick", "interior_material", "");
    layout.prop(ptr, "use_piece_data", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else {
    layout.prop(ptr, "use_offset_rows", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "use_hollow", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "use_piece_data", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    if (RNA_boolean_get(ptr, "use_piece_data")) {
      layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
  }
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Bricks, panel_draw);
}

ModifierTypeInfo modifierType_Bricks = {
    /*idname*/ "Bricks",
    /*name*/ N_("Bricks"),
    /*struct_name*/ "BricksModifierData",
    /*struct_size*/ sizeof(BricksModifierData),
    /*srna*/ &RNA_BricksModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_BRICKS,

    /*copy_data*/ BKE_modifier_copydata_generic,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ nullptr,
    /*free_data*/ nullptr,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
