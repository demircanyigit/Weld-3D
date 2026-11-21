/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Voronoi Fracture. Cuts a closed mesh into solid pieces, one for the area around each point.
 * The cutting is done by shared code in the geometry module, which the Voronoi Fracture node
 * uses too.
 */

#include <algorithm>
#include <atomic>
#include <cmath>

#include "BLI_disjoint_set.hh"
#include "BLI_hash.h"
#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_memory_utils.hh"
#include "BLI_noise.hh"
#include "BLI_rand.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_instances.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "GEO_join_geometries.hh"
#include "GEO_mesh_copy_selection.hh"
#include "GEO_mesh_voronoi_fracture.hh"
#include "GEO_randomize.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_util.hh"

namespace blender {

/** Internal face attribute used to find the cut faces for the interior material. */
static const char *INTERIOR_ATTRIBUTE = ".voronoi_fracture_interior";

static void init_data_VoronoiFractureModifierData(ModifierData *md)
{
  VoronoiFractureModifierData *data = reinterpret_cast<VoronoiFractureModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

/** Where the random points gather, in the space of the mesh. */
struct Impact {
  bool use = false;
  float3 center = float3(0.0f);
  float radius = 0.0f;
  float share = 0.0f;
};

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const VoronoiFractureModifierData *vmd = reinterpret_cast<VoronoiFractureModifierData *>(md);
  return vmd->source == MOD_VORONOI_SOURCE_OBJECT && vmd->points_object == nullptr;
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  VoronoiFractureModifierData *vmd = reinterpret_cast<VoronoiFractureModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&vmd->points_object), IDWALK_CB_NOP);
  walk(user_data, ob, reinterpret_cast<ID **>(&vmd->impact_object), IDWALK_CB_NOP);
  VoronoiFractureModifierData *pick_data = reinterpret_cast<VoronoiFractureModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&pick_data->interior_material_pick), IDWALK_CB_USER);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  VoronoiFractureModifierData *vmd = reinterpret_cast<VoronoiFractureModifierData *>(md);
  if (vmd->source == MOD_VORONOI_SOURCE_OBJECT && vmd->points_object != nullptr) {
    DEG_add_object_relation(
        ctx->node, vmd->points_object, DEG_OB_COMP_GEOMETRY, "Voronoi Fracture Modifier");
    DEG_add_object_relation(
        ctx->node, vmd->points_object, DEG_OB_COMP_TRANSFORM, "Voronoi Fracture Modifier");
    DEG_add_depends_on_transform_relation(ctx->node, "Voronoi Fracture Modifier");
  }
  if (vmd->source == MOD_VORONOI_SOURCE_RANDOM && vmd->impact_object != nullptr &&
      vmd->impact_object != ctx->object)
  {
    DEG_add_object_relation(
        ctx->node, vmd->impact_object, DEG_OB_COMP_TRANSFORM, "Voronoi Fracture Modifier");
    DEG_add_depends_on_transform_relation(ctx->node, "Voronoi Fracture Modifier");
  }
}

/** Random points in the bounds of the mesh. Cells of points outside the mesh make no piece. */
static Vector<float3> random_points(const Mesh &mesh,
                                    const int count,
                                    const int seed,
                                    const Impact &impact = {})
{
  Vector<float3> points;
  const std::optional<Bounds<float3>> bounds = mesh.bounds_min_max();
  if (!bounds) {
    return points;
  }
  RandomNumberGenerator rng(uint32_t(seed) * 2654435761u + 7u);
  const float3 size = bounds->max - bounds->min;
  points.reserve(count);
  for ([[maybe_unused]] const int i : IndexRange(count)) {
    const float3 t(rng.get_float(), rng.get_float(), rng.get_float());
    points.append(bounds->min + size * t);
  }
  /* Only when the impact reaches this mesh, so islands far from it keep even pieces. */
  if (impact.use && impact.radius > 0.0f &&
      math::distance(math::clamp(impact.center, bounds->min, bounds->max), impact.center) <
          impact.radius)
  {
    /* A share of the points in a ball around the impact, denser in the middle, so the pieces
     * get smaller toward it. */
    const int gathered = int(std::round(float(count) * std::clamp(impact.share, 0.0f, 1.0f)));
    for (const int i : IndexRange(std::min(gathered, int(points.size())))) {
      const float3 dir = rng.get_unit_float3();
      const float r = impact.radius * rng.get_float() * rng.get_float();
      points[i] = math::clamp(impact.center + dir * r, bounds->min, bounds->max);
    }
  }
  return points;
}

/** Count for a part of this share of the size of the biggest part, at least 1. */
static int sized_count(const int count, const float share)
{
  return std::max(1, int(std::round(float(count) * share)));
}

/** Random points under the faces of every face set: on a face, then moved in along its normal
 * by up to a third of the size of the area. Empty without face sets. */
static Vector<float3> face_set_points(const Mesh &mesh,
                                      const int count,
                                      const int seed,
                                      const bool by_size,
                                      bool &r_has_face_sets)
{
  Vector<float3> points;
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<int> face_sets = *attributes.lookup<int>(".sculpt_face_set",
                                                            bke::AttrDomain::Face);
  r_has_face_sets = !face_sets.is_empty();
  if (!r_has_face_sets) {
    return points;
  }
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> tris = mesh.corner_tris();
  const Span<int> tri_faces = mesh.corner_tri_faces();
  const Span<float3> face_normals = mesh.face_normals();

  Map<int, Vector<int>> tris_of_set;
  for (const int tri : tris.index_range()) {
    tris_of_set.lookup_or_add_default(face_sets[tri_faces[tri]]).append(tri);
  }
  Vector<int> sets;
  for (const int set : tris_of_set.keys()) {
    sets.append(set);
  }
  /* The same points every time, whatever order the map has. */
  std::sort(sets.begin(), sets.end());

  /* With By Size, Count is for the biggest face set and the others get fewer by area. */
  float max_area = 0.0f;
  if (by_size) {
    for (const int set : sets) {
      float area = 0.0f;
      for (const int tri_index : tris_of_set.lookup(set)) {
        const int3 &tri = tris[tri_index];
        area += area_tri_v3(positions[corner_verts[tri[0]]],
                            positions[corner_verts[tri[1]]],
                            positions[corner_verts[tri[2]]]);
      }
      max_area = std::max(max_area, area);
    }
  }

  for (const int set : sets) {
    const Span<int> set_tris = tris_of_set.lookup(set);
    Array<float> area_sum(set_tris.size());
    float total = 0.0f;
    for (const int i : set_tris.index_range()) {
      const int3 &tri = tris[set_tris[i]];
      total += area_tri_v3(positions[corner_verts[tri[0]]],
                           positions[corner_verts[tri[1]]],
                           positions[corner_verts[tri[2]]]);
      area_sum[i] = total;
    }
    if (total <= 0.0f) {
      continue;
    }
    const float depth = std::sqrt(total) / 3.0f;
    const int set_count = max_area > 0.0f ? sized_count(count, total / max_area) : count;
    RandomNumberGenerator rng(BLI_hash_int_2d(uint32_t(seed), uint32_t(set)));
    for ([[maybe_unused]] const int i : IndexRange(set_count)) {
      const float pick = rng.get_float() * total;
      const int index = int(std::lower_bound(area_sum.begin(), area_sum.end(), pick) -
                            area_sum.begin());
      const int tri_index = set_tris[std::min<int>(index, set_tris.size() - 1)];
      const int3 &tri = tris[tri_index];
      float u = rng.get_float(), v = rng.get_float();
      if (u + v > 1.0f) {
        u = 1.0f - u;
        v = 1.0f - v;
      }
      const float3 &a = positions[corner_verts[tri[0]]];
      const float3 &b = positions[corner_verts[tri[1]]];
      const float3 &c = positions[corner_verts[tri[2]]];
      const float3 on_face = a + (b - a) * u + (c - a) * v;
      points.append(on_face - face_normals[tri_faces[tri_index]] * (rng.get_float() * depth));
    }
  }
  return points;
}

/** Every loose part broken on its own, into about #count pieces. */
static Mesh *fracture_per_island(const Mesh &mesh,
                                 const VoronoiFractureModifierData &vmd,
                                 const geometry::MeshVoronoiFractureParams &params,
                                 const Impact &impact,
                                 geometry::MeshVoronoiFractureError *r_error)
{
  DisjointSet<int> sets(mesh.verts_num);
  for (const int2 edge : mesh.edges()) {
    sets.join(edge[0], edge[1]);
  }
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  Array<int> island_of_root(mesh.verts_num, -1);
  Array<int> face_island(mesh.faces_num);
  int islands_num = 0;
  for (const int face : faces.index_range()) {
    const int root = sets.find_root(corner_verts[faces[face].first()]);
    if (island_of_root[root] == -1) {
      island_of_root[root] = islands_num++;
    }
    face_island[face] = island_of_root[root];
  }
  if (islands_num <= 1) {
    return geometry::mesh_voronoi_fracture(
        mesh, random_points(mesh, vmd.count, vmd.seed, impact), params, r_error);
  }

  /* With By Size, Count is for the biggest island and the others get fewer by volume. */
  Array<int> island_count(islands_num, vmd.count);
  if (vmd.flag & MOD_VORONOI_BY_SIZE) {
    const Span<float3> positions = mesh.vert_positions();
    const Span<int3> tris = mesh.corner_tris();
    const Span<int> tri_faces = mesh.corner_tri_faces();
    Array<double> volume(islands_num, 0.0);
    for (const int tri : tris.index_range()) {
      const float3 &a = positions[corner_verts[tris[tri][0]]];
      const float3 &b = positions[corner_verts[tris[tri][1]]];
      const float3 &c = positions[corner_verts[tris[tri][2]]];
      volume[face_island[tri_faces[tri]]] += double(math::dot(a, math::cross(b, c))) / 6.0;
    }
    double max_volume = 0.0;
    for (double &v : volume) {
      v = std::abs(v);
      max_volume = std::max(max_volume, v);
    }
    if (max_volume > 0.0) {
      for (const int island : island_count.index_range()) {
        island_count[island] = sized_count(vmd.count, float(volume[island] / max_volume));
      }
    }
  }

  Array<Mesh *> pieces(islands_num, nullptr);
  std::atomic<int> error{int(geometry::MeshVoronoiFractureError::NoError)};
  threading::parallel_for(IndexRange(islands_num), 1, [&](const IndexRange range) {
    for (const int island : range) {
      Array<bool> selection(mesh.faces_num);
      for (const int face : selection.index_range()) {
        selection[face] = face_island[face] == island;
      }
      const std::optional<Mesh *> copy = geometry::mesh_copy_selection(
          mesh, VArray<bool>::from_span(selection), bke::AttrDomain::Face);
      if (!copy || *copy == nullptr) {
        continue;
      }
      if (island_count[island] <= 1) {
        /* One cell is the island as it is. */
        pieces[island] = *copy;
        continue;
      }
      geometry::MeshVoronoiFractureError piece_error =
          geometry::MeshVoronoiFractureError::NoError;
      Mesh *broken = geometry::mesh_voronoi_fracture(
          **copy,
          random_points(**copy,
                        island_count[island],
                        int(BLI_hash_int_2d(uint32_t(vmd.seed), uint32_t(island))),
                        impact),
          params,
          &piece_error);
      BKE_id_free(nullptr, *copy);
      if (broken == nullptr) {
        error = int(piece_error);
        continue;
      }
      pieces[island] = broken;
    }
  });
  if (error != int(geometry::MeshVoronoiFractureError::NoError)) {
    for (Mesh *piece : pieces) {
      if (piece) {
        BKE_id_free(nullptr, piece);
      }
    }
    *r_error = geometry::MeshVoronoiFractureError(int(error));
    return nullptr;
  }
  Array<bke::GeometrySet> geometries(islands_num);
  for (const int island : pieces.index_range()) {
    if (pieces[island]) {
      geometries[island] = bke::GeometrySet::from_mesh(pieces[island]);
    }
  }
  bke::GeometrySet joined = geometry::join_geometries(geometries, {});
  Mesh *result = joined.get_component_for_write<bke::MeshComponent>().release();
  return result ? result : BKE_mesh_new_nomain(0, 0, 0, 0);
}

/** Points, mesh vertices, curve points and instances of another object, in this object's
 * space. */
static Vector<float3> object_points(const Object &self, const Object &points_ob)
{
  Vector<float3> points;
  const bke::GeometrySet geometry = bke::object_get_evaluated_geometry_set(points_ob);
  if (const PointCloud *pointcloud = geometry.get_pointcloud()) {
    points.extend(pointcloud->positions());
  }
  if (const Mesh *mesh = geometry.get_mesh()) {
    points.extend(mesh->vert_positions());
  }
  if (const Curves *curves = geometry.get_curves()) {
    points.extend(curves->geometry.wrap().positions());
  }
  if (const bke::Instances *instances = geometry.get_instances()) {
    for (const float4x4 &transform : instances->transforms()) {
      points.append(transform.location());
    }
  }
  const float4x4 to_self = self.world_to_object() * points_ob.object_to_world();
  for (float3 &point : points) {
    point = math::transform_point(to_self, point);
  }
  return points;
}

/** The cell stretch, 1 for any axis that is not set. */
static float3 cell_scale(const VoronoiFractureModifierData &vmd)
{
  float3 scale(vmd.cell_scale);
  for (const int axis : IndexRange(3)) {
    if (!(scale[axis] > 1e-4f)) {
      scale[axis] = 1.0f;
    }
  }
  return scale;
}

/**
 * Piece Index and Piece Random on the points, and the pieces picked at random marked, removed or
 * kept alone. A piece is a connected part of the result. Returns the mesh to use.
 */
static Mesh *finish_pieces(Mesh *result, const VoronoiFractureModifierData &vmd)
{
  const bool piece_data = vmd.flag & MOD_VORONOI_PIECE_DATA;
  const int select = vmd.select_mode;
  if ((!piece_data && select == MOD_VORONOI_SELECT_NONE) || result->verts_num == 0) {
    return result;
  }
  DisjointSet<int> sets(result->verts_num);
  for (const int2 edge : result->edges()) {
    sets.join(edge[0], edge[1]);
  }
  const Span<float3> positions = result->vert_positions();
  Array<int> piece_of_root(result->verts_num, -1);
  Array<int> vert_piece(result->verts_num);
  Vector<float3> center;
  Vector<int> verts;
  for (const int v : positions.index_range()) {
    int &piece = piece_of_root[sets.find_root(v)];
    if (piece == -1) {
      piece = center.append_and_get_index(float3(0.0f));
      verts.append(0);
    }
    vert_piece[v] = piece;
    center[piece] += positions[v];
    verts[piece]++;
  }
  /* A random value per piece, the same as long as the piece stays where it is. */
  Array<float> random(center.size());
  for (const int piece : center.index_range()) {
    const float3 c = center[piece] / float(std::max(verts[piece], 1));
    random[piece] = noise::hash_float_to_float(
        float4(c.x, c.y, c.z, float(vmd.select_seed) * 1.618f));
  }
  const auto picked = [&](const int v) { return random[vert_piece[v]] < vmd.select_amount; };

  bke::MutableAttributeAccessor attributes = result->attributes_for_write();
  if (piece_data) {
    bke::SpanAttributeWriter<int> index = attributes.lookup_or_add_for_write_only_span<int>(
        "Piece Index", bke::AttrDomain::Point);
    bke::SpanAttributeWriter<float> value = attributes.lookup_or_add_for_write_only_span<float>(
        "Piece Random", bke::AttrDomain::Point);
    if (index && value) {
      for (const int v : positions.index_range()) {
        index.span[v] = vert_piece[v];
        value.span[v] = random[vert_piece[v]];
      }
    }
    index.finish();
    value.finish();
  }
  if (select == MOD_VORONOI_SELECT_MARK) {
    bke::SpanAttributeWriter<bool> marked = attributes.lookup_or_add_for_write_only_span<bool>(
        "Selected", bke::AttrDomain::Point);
    if (marked) {
      for (const int v : positions.index_range()) {
        marked.span[v] = picked(v);
      }
      marked.finish();
    }
    return result;
  }
  if (!ELEM(select, MOD_VORONOI_SELECT_REMOVE, MOD_VORONOI_SELECT_KEEP)) {
    return result;
  }
  Array<bool> keep(result->verts_num);
  for (const int v : positions.index_range()) {
    keep[v] = picked(v) == (select == MOD_VORONOI_SELECT_KEEP);
  }
  const std::optional<Mesh *> kept = geometry::mesh_copy_selection(
      *result, VArray<bool>::from_span(keep), bke::AttrDomain::Point);
  if (!kept) {
    return result;
  }
  BKE_id_free(nullptr, result);
  if (*kept) {
    return *kept;
  }
  return BKE_mesh_new_nomain(0, 0, 0, 0);
}

static Mesh *modify_mesh_compute(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const VoronoiFractureModifierData &vmd = *reinterpret_cast<VoronoiFractureModifierData *>(md);
  if (mesh->faces_num == 0) {
    return mesh;
  }

  /* Stretched cells: the fracture runs on the mesh squeezed by the stretch, then it is stretched
   * back, so the cells come out longer along the stretched axes. */
  const float3 stretch = cell_scale(vmd);
  const bool stretched = stretch != float3(1.0f);
  Mesh *squeezed = nullptr;
  Mesh *input = mesh;
  if (stretched) {
    squeezed = BKE_mesh_copy_for_eval(*mesh);
    for (float3 &position : squeezed->vert_positions_for_write()) {
      position /= stretch;
    }
    squeezed->tag_positions_changed();
    input = squeezed;
  }
  BLI_SCOPED_DEFER([&]() {
    if (squeezed != nullptr) {
      BKE_id_free(nullptr, squeezed);
    }
  });

  Impact impact;
  if (vmd.source == MOD_VORONOI_SOURCE_RANDOM && vmd.impact_object != nullptr) {
    impact.use = true;
    impact.center = math::transform_point(ctx->object->world_to_object(),
                                          vmd.impact_object->object_to_world().location()) /
                    stretch;
    impact.radius = vmd.impact_radius / math::reduce_max(stretch);
    impact.share = vmd.impact_share;
  }

  const bool per_island = vmd.source == MOD_VORONOI_SOURCE_RANDOM &&
                          vmd.per == MOD_VORONOI_PER_ISLAND;
  Vector<float3> points;
  if (vmd.source == MOD_VORONOI_SOURCE_OBJECT) {
    if (vmd.points_object == nullptr) {
      return mesh;
    }
    points = object_points(*ctx->object, *vmd.points_object);
    for (float3 &point : points) {
      point /= stretch;
    }
  }
  else if (vmd.per == MOD_VORONOI_PER_FACE_SET) {
    bool has_face_sets = false;
    points = face_set_points(
        *input, vmd.count, vmd.seed, vmd.flag & MOD_VORONOI_BY_SIZE, has_face_sets);
    if (!has_face_sets) {
      BKE_modifier_set_error(ctx->object, md, "The mesh has no face sets");
      return mesh;
    }
  }
  else if (!per_island) {
    points = random_points(*input, vmd.count, vmd.seed, impact);
  }
  if (points.is_empty() && !per_island) {
    return mesh;
  }

  const bool use_interior_material = vmd.flag & MOD_VORONOI_INTERIOR_MATERIAL;
  const int interior_material = use_interior_material ?
                                    MOD_material_slot(*mesh,
                                                      vmd.interior_material_pick,
                                                      vmd.interior_material) :
                                    0;
  geometry::MeshVoronoiFractureParams params;
  params.cut_gap = vmd.cut_gap;
  params.separate_islands = vmd.flag & MOD_VORONOI_SEPARATE_ISLANDS;
  if (use_interior_material) {
    params.interior_id = INTERIOR_ATTRIBUTE;
  }

  geometry::MeshVoronoiFractureError error = geometry::MeshVoronoiFractureError::NoError;
  Mesh *result = per_island ? fracture_per_island(*input, vmd, params, impact, &error) :
                              geometry::mesh_voronoi_fracture(*input, points, params, &error);
  if (result == nullptr) {
    if (error == geometry::MeshVoronoiFractureError::NonManifold) {
      BKE_modifier_set_error(ctx->object, md, "The mesh must be closed and manifold");
    }
    else if (error == geometry::MeshVoronoiFractureError::SolverNotAvailable) {
      BKE_modifier_set_error(ctx->object, md, "Built without the Manifold library");
    }
    else {
      BKE_modifier_set_error(ctx->object, md, "The mesh could not be fractured");
    }
    return mesh;
  }

  if (use_interior_material) {
    bke::MutableAttributeAccessor attributes = result->attributes_for_write();
    {
      const VArraySpan<bool> interior = *attributes.lookup_or_default<bool>(
          INTERIOR_ATTRIBUTE, bke::AttrDomain::Face, false);
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
    attributes.remove(INTERIOR_ATTRIBUTE);
  }

  if (stretched) {
    for (float3 &position : result->vert_positions_for_write()) {
      position *= stretch;
    }
    result->tag_positions_changed();
  }

  result = finish_pieces(result, vmd);
  geometry::debug_randomize_mesh_order(result);
  return result;
}

static void panel_draw_VoronoiFracture(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.prop(ptr, "source", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.use_property_split_set(true);
  if (RNA_enum_get(ptr, "source") == MOD_VORONOI_SOURCE_OBJECT) {
    layout.prop(ptr, "points_object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else {
    layout.prop(ptr, "count", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "per", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    if (RNA_enum_get(ptr, "per") != MOD_VORONOI_PER_MESH) {
      layout.prop(ptr, "use_by_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
    layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "impact_object", UI_ITEM_NONE, IFACE_("Impact"), ICON_NONE);
    if (RNA_pointer_get(ptr, "impact_object").data != nullptr) {
      layout.prop(ptr, "impact_radius", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(ptr, "impact_share", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
  }
  layout.prop(ptr, "cell_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "cut_gap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_separate_islands", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &row = layout.row(true, IFACE_("Interior Material"));
  row.prop(ptr, "use_interior_material", UI_ITEM_NONE, "", ICON_NONE);
  ui::Layout &sub = row.row(true);
  sub.active_set(RNA_boolean_get(ptr, "use_interior_material"));
  modifier_material_ui(sub, ptr, "interior_material_pick", "interior_material", "");

  layout.prop(ptr, "use_piece_data", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "select_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "select_mode") != MOD_VORONOI_SELECT_NONE) {
    layout.prop(ptr, "select_amount", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    layout.prop(ptr, "select_seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  modifier_error_message_draw(layout, ptr);
}

static void panel_register_VoronoiFracture(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_VoronoiFracture, panel_draw_VoronoiFracture);
}

/* Taken from the cache when nothing it depends on changed, see #MOD_cached_modify_mesh. Points
 * from another object are not checked for changes, so then it is not cached. */
static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const VoronoiFractureModifierData &vmd = *reinterpret_cast<VoronoiFractureModifierData *>(md);
  if ((vmd.source == MOD_VORONOI_SOURCE_OBJECT && vmd.points_object != nullptr) ||
      (vmd.source == MOD_VORONOI_SOURCE_RANDOM && vmd.impact_object != nullptr))
  {
    return modify_mesh_compute(md, ctx, mesh);
  }
  if (vmd.flag & MOD_VORONOI_INTERIOR_MATERIAL) {
    MOD_material_slot(*mesh, vmd.interior_material_pick, 0);
  }
  return MOD_cached_modify_mesh(md, ctx, mesh, modify_mesh_compute);
}

ModifierTypeInfo modifierType_VoronoiFracture = {
    /*idname*/ "VoronoiFracture",
    /*name*/ N_("Voronoi Fracture"),
    /*struct_name*/ "VoronoiFractureModifierData",
    /*struct_size*/ sizeof(VoronoiFractureModifierData),
    /*srna*/ &RNA_VoronoiFractureModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_MOD_VORONOI_FRACTURE,

    /*copy_data*/ BKE_modifier_copydata_generic,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data_VoronoiFractureModifierData,
    /*required_data_mask*/ nullptr,
    /*free_data*/ nullptr,
    /*is_disabled*/ is_disabled,
    /*update_depsgraph*/ update_depsgraph,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register_VoronoiFracture,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
