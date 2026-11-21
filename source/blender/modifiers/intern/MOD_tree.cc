/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Tree: grows a tree from a Tree Graph, as one mesh ready for games. The graph is read level by
 * level, starting from what is linked into its Tree Output.
 */

#include <string>

#include <fmt/format.h>

#include <algorithm>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_constants.h"
#include "BLI_math_matrix.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "DNA_collection_types.h"
#include "DNA_color_types.h"
#include "DNA_image_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_collection.hh"
#include "BKE_colortools.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"

#include "BLO_read_write.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "GEO_mesh_tree.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "NOD_geometry_nodes_execute.hh"

#include "MOD_tree.hh"
#include "MOD_tree_fields.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"

namespace blender {

namespace tree = geometry::tree;

static void init_data(ModifierData *md)
{
  TreeModifierData *data = reinterpret_cast<TreeModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(data, modifier);
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
  const TreeModifierData *tmd = reinterpret_cast<const TreeModifierData *>(md);
  TreeModifierData *tmd_dst = reinterpret_cast<TreeModifierData *>(target);
  BKE_modifier_copydata_generic(md, target, flag);
  if (tmd->properties != nullptr) {
    tmd_dst->properties = IDP_CopyProperty_ex(tmd->properties, flag);
  }
}

static void free_data(ModifierData *md)
{
  TreeModifierData *tmd = reinterpret_cast<TreeModifierData *>(md);
  if (tmd->properties != nullptr) {
    IDP_FreeProperty_ex(tmd->properties, false);
    tmd->properties = nullptr;
  }
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const TreeModifierData *tmd = reinterpret_cast<const TreeModifierData *>(md);
  writer->write_struct(tmd);
  if (tmd->properties != nullptr) {
    IDP_BlendWrite(writer, tmd->properties);
  }
}

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  TreeModifierData *tmd = reinterpret_cast<TreeModifierData *>(md);
  BLO_read_struct(reader, IDProperty, &tmd->properties);
  IDP_BlendDataRead(reader, &tmd->properties);
}

/* -------------------------------------------------------------------- */
/** \name Graph inputs
 * \{ */

static bool is_input_type(const bNodeTreeInterfaceSocket &socket)
{
  const bke::bNodeSocketType *typeinfo = socket.socket_typeinfo();
  return typeinfo != nullptr &&
         ELEM(typeinfo->type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN, SOCK_VECTOR, SOCK_RGBA);
}

/** Both a single value or both a list of values, so the old one can be carried over. */
static bool same_shape(const IDProperty &a, const IDProperty &b)
{
  return (a.type == IDP_ARRAY) == (b.type == IDP_ARRAY);
}

void MOD_tree_update_interface(Object *object, TreeModifierData *tmd)
{
  IDProperty *old_properties = tmd->properties;
  tmd->properties = nullptr;

  if (tmd->tree_graph != nullptr && !ID_MISSING(tmd->tree_graph)) {
    const bNodeTree &tree = *tmd->tree_graph;
    tree.ensure_interface_cache();
    tmd->properties = bke::idprop::create_group("Tree Modifier Inputs").release();
    for (const bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
      if (!is_input_type(*socket)) {
        continue;
      }
      IDProperty *property =
          nodes::id_property_create_from_socket(*socket, nodes::StructureType::Single, false)
              .release();
      if (property == nullptr) {
        continue;
      }
      property->flag |= IDP_FLAG_OVERRIDABLE_LIBRARY | IDP_FLAG_STATIC_TYPE;
      if (socket->description && socket->description[0] != '\0') {
        IDPropertyUIData *ui_data = IDP_ui_data_ensure(property);
        MEM_SAFE_DELETE(ui_data->description);
        ui_data->description = BLI_strdup(socket->description);
      }
      IDP_AddToGroup(tmd->properties, property);

      /* Keep what was set before, also when the input changed between number types. */
      const IDProperty *old = old_properties ?
                                  IDP_GetPropertyFromGroup(old_properties, socket->identifier) :
                                  nullptr;
      if (old != nullptr && same_shape(*old, *property)) {
        tree_fields::property_value_set(*property, tree_fields::property_value(*old));
      }
    }
  }

  if (old_properties != nullptr) {
    IDP_FreeProperty(old_properties);
  }
  if (object != nullptr) {
    DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  }
}

/** \} */

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const TreeModifierData &tmd = *reinterpret_cast<TreeModifierData *>(md);
  return tmd.tree_graph == nullptr;
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  TreeModifierData &tmd = *reinterpret_cast<TreeModifierData *>(md);
  if (tmd.tree_graph == nullptr) {
    return;
  }
  DEG_add_node_tree_output_relation(ctx->node, tmd.tree_graph, "Tree Modifier");

  /* Forces, shapes and mesh leaves read other objects, placed in the space of this one. */
  bool uses_objects = false;
  for (const bNode *node : tmd.tree_graph->all_nodes()) {
    for (const bNodeSocket *socket = static_cast<const bNodeSocket *>(node->inputs.first); socket;
         socket = socket->next)
    {
      if (socket->type != SOCK_OBJECT) {
        continue;
      }
      Object *object = socket->default_value_typed<bNodeSocketValueObject>()->value;
      if (object == nullptr || object == ctx->object) {
        continue;
      }
      DEG_add_object_relation(ctx->node, object, DEG_OB_COMP_TRANSFORM, "Tree Modifier");
      if (ELEM(object->type, OB_MESH, OB_CURVES_LEGACY, OB_CURVES)) {
        DEG_add_object_relation(ctx->node, object, DEG_OB_COMP_GEOMETRY, "Tree Modifier");
      }
      uses_objects = true;
    }
    /* Mesh leaves picked from a collection. */
    for (const bNodeSocket *socket = static_cast<const bNodeSocket *>(node->inputs.first); socket;
         socket = socket->next)
    {
      if (socket->type != SOCK_COLLECTION) {
        continue;
      }
      Collection *collection = socket->default_value_typed<bNodeSocketValueCollection>()->value;
      if (collection == nullptr) {
        continue;
      }
      DEG_add_collection_geometry_relation(ctx->node, collection, "Tree Modifier");
      uses_objects = true;
    }
  }
  if (uses_objects) {
    DEG_add_depends_on_transform_relation(ctx->node, "Tree Modifier");
  }
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  TreeModifierData &tmd = *reinterpret_cast<TreeModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&tmd.tree_graph), IDWALK_CB_USER);
}

/* -------------------------------------------------------------------- */
/** \name Reading the graph
 * \{ */

static const bNodeSocket *find_input(const bNode &node, const StringRef identifier)
{
  for (const bNodeSocket *socket = static_cast<const bNodeSocket *>(node.inputs.first); socket;
       socket = socket->next)
  {
    if (socket->identifier == identifier) {
      return socket;
    }
  }
  return nullptr;
}

static int input_menu(const bNode &node, const StringRef identifier, const int fallback)
{
  const bNodeSocket *socket = find_input(node, identifier);
  if (socket == nullptr || socket->type != SOCK_MENU) {
    return fallback;
  }
  return socket->default_value_typed<bNodeSocketValueMenu>()->value;
}

static Collection *input_collection(const bNode &node, const StringRef identifier)
{
  const bNodeSocket *socket = find_input(node, identifier);
  if (socket == nullptr || socket->type != SOCK_COLLECTION) {
    return nullptr;
  }
  return socket->default_value_typed<bNodeSocketValueCollection>()->value;
}

static Image *input_image(const bNode &node, const StringRef identifier)
{
  const bNodeSocket *socket = find_input(node, identifier);
  if (socket == nullptr || socket->type != SOCK_IMAGE) {
    return nullptr;
  }
  return socket->default_value_typed<bNodeSocketValueImage>()->value;
}

static Object *input_object(const bNode &node, const StringRef identifier)
{
  const bNodeSocket *socket = find_input(node, identifier);
  if (socket == nullptr || socket->type != SOCK_OBJECT) {
    return nullptr;
  }
  return socket->default_value_typed<bNodeSocketValueObject>()->value;
}

/** The node's curve, sampled so the tree can read it from any thread. */
[[maybe_unused]] static Array<float> sample_profile(const bNode &node, const int index = 0)
{
  const CurveMapping *source = static_cast<const CurveMapping *>(node.storage);
  if (source == nullptr || source->cm[index].curve == nullptr ||
      source->cm[index].totpoint < 2)
  {
    return {};
  }
  CurveMapping *cumap = BKE_curvemapping_copy(source);
  BKE_curvemapping_init(cumap);
  Array<float> values(65);
  for (const int i : values.index_range()) {
    values[i] = BKE_curvemapping_evaluateF(cumap, index, float(i) / float(values.size() - 1));
  }
  BKE_curvemapping_free(cumap);
  return values;
}

/** The nodes linked into a "Children" or "Tree" socket, past reroutes and muted nodes. */
static Vector<const bNode *> linked_nodes(const bNode &node, const StringRef identifier)
{
  Vector<const bNode *> nodes;
  const bNodeSocket *socket = find_input(node, identifier);
  if (socket == nullptr) {
    return nodes;
  }
  for (const bNodeSocket *from : socket->logically_linked_sockets()) {
    nodes.append_non_duplicates(&from->owner_node());
  }
  return nodes;
}

struct GraphReader {
  tree::TreeParams &params;
  /** Works out what the nodes linked into the numbers give. */
  tree_fields::FieldCompiler fields;
  Map<const bNode *, int> levels_by_node;

  const Object *self() const
  {
    return fields.self();
  }
  const float4x4 &world_to_tree() const
  {
    return fields.world_to_tree();
  }
};

static float input_float(GraphReader &reader,
                         const bNode &node,
                         const StringRef identifier,
                         const float fallback)
{
  const bNodeSocket *socket = find_input(node, identifier);
  if (socket == nullptr || socket->type != SOCK_FLOAT) {
    return fallback;
  }
  return reader.fields.number(*socket);
}

static int input_int(GraphReader &reader,
                     const bNode &node,
                     const StringRef identifier,
                     const int fallback)
{
  const bNodeSocket *socket = find_input(node, identifier);
  if (socket == nullptr || socket->type != SOCK_INT) {
    return fallback;
  }
  return reader.fields.integer(*socket);
}

static bool input_bool(GraphReader &reader,
                       const bNode &node,
                       const StringRef identifier,
                       const bool fallback)
{
  const bNodeSocket *socket = find_input(node, identifier);
  if (socket == nullptr || socket->type != SOCK_BOOLEAN) {
    return fallback;
  }
  return reader.fields.number(*socket) != 0.0f;
}

static float3 input_vector(GraphReader &reader,
                           const bNode &node,
                           const StringRef identifier,
                           const float3 &fallback)
{
  const bNodeSocket *socket = find_input(node, identifier);
  if (socket == nullptr || socket->type != SOCK_VECTOR) {
    return fallback;
  }
  return reader.fields.vector(*socket);
}

/** A number on a level, worked out by the nodes linked into it for every branch. */
static tree::Param input_param(GraphReader &reader,
                               const bNode &node,
                               const StringRef identifier,
                               const float fallback)
{
  const bNodeSocket *socket = find_input(node, identifier);
  if (socket == nullptr || !ELEM(socket->type, SOCK_FLOAT, SOCK_INT)) {
    return fallback;
  }
  return reader.fields.param(*socket);
}

/** A number kept between \a lo and \a hi, also when nodes work it out. */
static tree::Param clamped(tree::Param param, const float lo, const float hi)
{
  param.value = std::clamp(param.value, lo, hi);
  if (param.fn) {
    param.fn = [fn = std::move(param.fn), lo, hi](const tree::FieldContext &context) {
      return std::clamp(fn(context), lo, hi);
    };
  }
  return param;
}

static tree::Param input_param(GraphReader &reader,
                               const bNode &node,
                               const StringRef identifier,
                               const float fallback,
                               const float lo,
                               const float hi)
{
  return clamped(input_param(reader, node, identifier, fallback), lo, hi);
}

static const Mesh *object_mesh(const GraphReader &reader, const Object *object)
{
  if (object == nullptr || object == reader.self() || object->type != OB_MESH) {
    return nullptr;
  }
  return BKE_object_get_evaluated_mesh(object);
}

/** The splines of a curve object, in the space of the tree, with their radius. */
static Vector<tree::DrawnSpline> drawn_splines(const GraphReader &reader, const Object *object)
{
  Vector<tree::DrawnSpline> splines;
  if (object == nullptr || object == reader.self()) {
    return splines;
  }
  const float4x4 to_tree = reader.world_to_tree() * object->object_to_world();
  const bke::GeometrySet geometry = bke::object_get_evaluated_geometry_set(*object);
  const Curves *curves_id = geometry.get_curves();
  if (curves_id == nullptr) {
    return splines;
  }
  const bke::CurvesGeometry &curves = curves_id->geometry.wrap();
  if (curves.curves_num() == 0) {
    return splines;
  }
  const Span<float3> positions = curves.evaluated_positions();
  const VArraySpan<float> radius = *curves.attributes().lookup_or_default<float>(
      "radius", bke::AttrDomain::Point, 1.0f);
  Array<float> evaluated_radius(curves.evaluated_points_num());
  curves.interpolate_to_evaluated(Span<float>(radius), evaluated_radius.as_mutable_span());
  const OffsetIndices<int> points_by_curve = curves.evaluated_points_by_curve();
  for (const int curve : curves.curves_range()) {
    const IndexRange range = points_by_curve[curve];
    if (range.size() < 2) {
      continue;
    }
    tree::DrawnSpline spline;
    for (const int i : range) {
      spline.points.append(math::transform_point(to_tree, positions[i]));
      spline.radii.append(evaluated_radius[i]);
    }
    splines.append(std::move(spline));
  }
  return splines;
}

/** The gray of an image, row by row from the bottom. */
static void read_image(Image *image, tree::Displace &displace)
{
  if (image == nullptr) {
    return;
  }
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  if (ibuf != nullptr && ibuf->x > 0 && ibuf->y > 0) {
    const int w = ibuf->x;
    const int h = ibuf->y;
    displace.image.reinitialize(int64_t(w) * h);
    displace.image_size = int2(w, h);
    const int channels = std::max(ibuf->channels, 1);
    for (const int64_t i : displace.image.index_range()) {
      float gray = 0.0f;
      if (ibuf->float_buffer.data) {
        const float *px = ibuf->float_buffer.data + i * channels;
        gray = channels >= 3 ? (px[0] + px[1] + px[2]) / 3.0f : px[0];
      }
      else if (ibuf->byte_buffer.data) {
        const uchar *px = ibuf->byte_buffer.data + i * 4;
        gray = (float(px[0]) + float(px[1]) + float(px[2])) / (3.0f * 255.0f);
      }
      displace.image[i] = gray;
    }
  }
  BKE_image_release_ibuf(image, ibuf, lock);
}

/** The Force, Avoid, Crown, Prune and Bark nodes linked into a level. */
static void read_forces(GraphReader &reader, const bNode &node, tree::Level &level)
{
  const float scale = reader.params.scale;
  Vector<const bNode *> helpers = linked_nodes(node, "Forces");
  for (const bNode *helper : linked_nodes(node, "Surface")) {
    helpers.append_non_duplicates(helper);
  }
  for (const bNode *helper_node : helpers) {
    const bNode &f = *helper_node;
    if (f.is_type("TreeGraphNodeForce")) {
      tree::Force force;
      const int mode = std::clamp(input_menu(f, "Mode", 0), 0, 2);
      force.type = mode == 1 ? tree::ForceType::Toward :
                   mode == 2 ? tree::ForceType::Away :
                               tree::ForceType::Direction;
      const float3 direction = input_vector(reader, f, "Direction", float3(0.0f, 0.0f, 1.0f));
      force.direction = direction;
      const Object *object = input_object(f, "Object");
      if (object != nullptr && object != reader.self()) {
        force.point = math::transform_point(reader.world_to_tree(),
                                            object->object_to_world().location());
      }
      else {
        force.point = direction * scale;
      }
      force.strength = input_param(reader, f, "Strength", 0.02f);
      level.forces.append(std::move(force));
    }
    else if (f.is_type("TreeGraphNodeAvoid")) {
      tree::Force force;
      force.type = tree::ForceType::Avoid;
      const Object *object = input_object(f, "Object");
      force.mesh = object_mesh(reader, object);
      if (force.mesh == nullptr) {
        continue;
      }
      force.mesh_transform = reader.world_to_tree() * object->object_to_world();
      force.strength = input_param(reader, f, "Strength", 1.0f);
      force.distance = input_param(reader, f, "Distance", 0.3f, 0.0f, FLT_MAX);
      force.lean = input_menu(f, "Mode", 0) == 1;
      force.reach = std::max(input_float(reader, f, "Reach", 3.0f), 0.0f);
      force.rays = std::clamp(input_int(reader, f, "Rays", 8), 1, 64);
      force.up_bias = std::max(input_float(reader, f, "Up Bias", 0.5f), 0.0f);
      level.forces.append(std::move(force));
    }
    else if (f.is_type("TreeGraphNodeCrown")) {
      tree::CrownShape shape;
      shape.type = tree::ShapeType(std::clamp(input_menu(f, "Shape", 0), 0, 3));
      /* Shortened so the tips still end in a point. */
      shape.cut = false;
      shape.softness = std::clamp(input_float(reader, f, "Softness", 0.3f), 0.0f, 1.0f);
      const float3 size = math::max(input_vector(reader, f, "Size", float3(4.0f, 4.0f, 5.0f)),
                                    float3(1e-3f));
      float4x4 shape_to_tree = float4x4::identity();
      const Object *object = input_object(f, "Object");
      if (object != nullptr && object != reader.self()) {
        shape_to_tree = reader.world_to_tree() * object->object_to_world();
      }
      else {
        shape_to_tree[0][0] = size.x * scale;
        shape_to_tree[1][1] = size.y * scale;
        shape_to_tree[2][2] = size.z * scale;
        shape_to_tree.location() = input_vector(reader, f, "Offset", float3(0.0f, 0.0f, 6.0f)) *
                                   scale;
      }
      shape.to_shape = math::invert(shape_to_tree);
      level.shapes.append(shape);
    }
    else if (f.is_type("TreeGraphNodePrune")) {
      tree::Prune prune;
      prune.mode = tree::PruneMode(std::clamp(input_menu(f, "Mode", 0), 0, 2));
      prune.amount = input_param(reader, f, "Amount", 0.3f);
      prune.spacing = std::max(input_float(reader, f, "Spacing", 0.3f), 0.0f);
      prune.seed = input_int(reader, f, "Seed", 0);
      level.prunes.append(std::move(prune));
    }
    else if (f.is_type("TreeGraphNodeBark")) {
      level.flutes = std::clamp(input_int(reader, f, "Flutes", 0), 0, 32);
      level.flute_depth = input_param(reader, f, "Flute Depth", 0.25f, 0.0f, 0.9f);
      level.flute_height = 1.0f;
      level.bumps = input_param(reader, f, "Bumps", 0.0f, 0.0f, 1.0f);
      level.bump_size = input_param(reader, f, "Bump Size", 0.3f, 1e-3f, FLT_MAX);
      tree::Displace displace;
      read_image(input_image(f, "Image"), displace);
      displace.use_image = !displace.image.is_empty();
      displace.radial = input_param(reader, f, "Displace", 0.0f);
      displace.scale = input_param(reader, f, "Scale", 1.0f, 1e-3f, FLT_MAX);
      displace.fine_amount = 0.5f;
      displace.flare_count = std::clamp(input_int(reader, f, "Count", 0), 0, 16);
      displace.flare_radius = input_param(reader, f, "Reach", 0.5f, 0.0f, FLT_MAX);
      displace.flare_height = input_param(reader, f, "Height", 0.15f, 0.0f, 1.0f);
      const bool displaces = displace.radial.fn || displace.radial.value != 0.0f;
      if (displaces || displace.flare_count > 0) {
        level.displaces.append(std::move(displace));
      }
    }
  }
}

/** The Weld, Normals and Mesh panels of trunks, branches, roots and drawn stems. */
static void read_stem_panels(GraphReader &reader, const bNode &node, tree::Level &level)
{
  level.weld = input_param(reader, node, "Weld", 0.0f, 0.0f, 1.0f);
  level.weld_length = input_param(reader, node, "Weld Length", 1.0f, 0.0f, FLT_MAX);
  level.weld_upper = 1.0f;
  level.weld_lower = 1.5f;
  level.weld_rings = std::clamp(input_int(reader, node, "Weld Rings", 6), 2, 32);
  level.parent_normals = input_bool(reader, node, "From Parent", true);
  level.normal_blend = input_param(reader, node, "Blend Length", 1.0f, 0.0f, FLT_MAX);
  level.smooth = input_bool(reader, node, "Smooth", true);
  level.segments = std::max(input_int(reader, node, "Sections", 8), 1);
  level.mesh_sides = std::max(input_int(reader, node, "Sides", 0), 0);
  level.u_tiles = std::max(input_int(reader, node, "U Tiles", 1), 1);
  level.v_tiles = std::max(input_float(reader, node, "V Scale", 1.0f), 0.01f);
  level.material = std::max(input_int(reader, node, "Material Index", 0), 0);
  level.gnarl = input_param(reader, node, "Gnarl", 0.0f, 0.0f, FLT_MAX);
  level.stem_twist = input_param(reader, node, "Twist", 0.0f);
  level.taper = input_param(reader, node, "Taper", 0.5f, 0.0f, 1.0f);
  level.make_mesh = input_menu(node, "Skin", 0) == 0;
}

/** Mesh leaves from a collection, each as often as its "Weight" property says. */
static void read_leaf_choices(GraphReader &reader, Collection *collection, tree::Level &level)
{
  if (collection == nullptr) {
    return;
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, object) {
    const Mesh *mesh = object_mesh(reader, object);
    if (mesh == nullptr || mesh->faces_num == 0) {
      continue;
    }
    tree::LeafChoice choice;
    choice.mesh = mesh;
    if (object->id.properties != nullptr) {
      if (const IDProperty *weight = IDP_GetPropertyFromGroup(object->id.properties, "Weight")) {
        choice.weight = std::max(float(IDP_coerce_to_double_or_zero(weight)), 0.0f);
      }
    }
    level.leaf_choices.append(choice);
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
}

static int read_level(GraphReader &reader, const bNode &node, const int depth)
{
  if (const int *index = reader.levels_by_node.lookup_ptr(&node)) {
    return *index;
  }
  if (depth > 16) {
    return -1;
  }
  tree::Level level;
  level.id = node.identifier;
  if (node.is_type("TreeGraphNodeTrunk")) {
    level.type = tree::LevelType::Trunk;
    read_stem_panels(reader, node, level);
    level.count = std::clamp(input_int(reader, node, "Count", 1), 1, 64);
    level.length = input_param(reader, node, "Length", 10.0f, 0.0f, FLT_MAX);
    level.radius = input_param(reader, node, "Radius", 0.4f, 0.0f, FLT_MAX);
    level.taper = input_param(reader, node, "Taper", 0.7f, 0.0f, 1.0f);
    level.segments = std::max(input_int(reader, node, "Sections", 12), 1);
    /* The trunk's Sides set its rings, smaller stems count from them. */
    level.sides = std::max(input_int(reader, node, "Sides", 10), 3);
    level.mesh_sides = 0;
    level.weld = 0.0f;
    read_forces(reader, node, level);
  }
  else if (node.is_type("TreeGraphNodeBranches")) {
    level.type = tree::LevelType::Branches;
    read_stem_panels(reader, node, level);
    level.count = input_int(reader, node, "Count", 6);
    level.count_field = input_param(reader, node, "Count", 6.0f, 0.0f, 4096.0f);
    level.start = input_param(reader, node, "Start", 0.4f, 0.0f, 1.0f);
    level.angle = input_param(reader, node, "Angle", 0.8727f);
    level.length = input_param(reader, node, "Length", 4.0f, 0.0f, FLT_MAX);
    level.radius = input_param(reader, node, "Radius", 0.7f, 0.0f, 1.0f);
    level.arrangement = tree::Arrangement(
        std::clamp(input_menu(node, "Arrange", 0), 0, int(tree::Arrangement::Random)));
    level.per_node = std::max(input_int(reader, node, "Per Ring", 3), 1);
    level.jitter = input_param(reader, node, "Spread", 0.5f, 0.0f, 1.0f);
    read_forces(reader, node, level);
  }
  else if (node.is_type("TreeGraphNodeLeaves")) {
    level.type = tree::LevelType::Leaves;
    level.material = std::max(input_int(reader, node, "Material Index", 1), 0);
    level.shape = input_menu(node, "Shape", 0) == 1 ? tree::LeafShape::Mesh :
                                                      tree::LeafShape::Square;
    if (level.shape == tree::LeafShape::Mesh) {
      level.leaf_mesh = object_mesh(reader, input_object(node, "Object"));
      read_leaf_choices(reader, input_collection(node, "Collection"), level);
    }
    level.count = input_int(reader, node, "Count", 16);
    level.count_field = input_param(reader, node, "Count", 16.0f, 0.0f, 4096.0f);
    level.start = input_param(reader, node, "Start", 0.2f, 0.0f, 1.0f);
    level.angle = input_param(reader, node, "Angle", 0.698f);
    level.length = input_param(reader, node, "Size", 0.25f, 0.0f, FLT_MAX);
    level.size_random = input_param(reader, node, "Size Random", 0.5f, 0.0f, 1.0f);
    level.tip_leaf = input_bool(reader, node, "Tip Leaf", true);
    level.cross = input_menu(node, "Cards", 0) == 1;
    level.width = 1.0f;
    level.fold = 0.0f;
    level.leaf_weld = true;
    level.normal_mode = tree::NormalMode(std::clamp(input_menu(node, "Mode", 1), 0, 4));
    level.soft_normals = input_param(reader, node, "Blend", 0.7f, 0.0f, 1.0f);
    level.face_up = input_param(reader, node, "Face Up", 0.6f, 0.0f, 1.0f);
    level.use_clumps = input_bool(reader, node, "Use Clumps", false);
    level.clump_count = std::clamp(input_int(reader, node, "Clumps", 3), 1, 64);
    level.clump_size = input_param(reader, node, "Clump Size", 0.8f, 1e-3f, FLT_MAX);
    level.show_clumps = input_bool(reader, node, "Show Clumps", false);
    level.clump_squash = std::clamp(input_float(reader, node, "Squash", 0.8f), 0.05f, 4.0f);
    level.clump_merge = std::clamp(input_float(reader, node, "Merge", 0.5f), 0.0f, 1.0f);
    read_forces(reader, node, level);
  }
  else if (node.is_type("TreeGraphNodeFronds")) {
    level.type = tree::LevelType::Fronds;
    level.planes = std::clamp(input_int(reader, node, "Count", 1), 1, 8);
    level.start = input_param(reader, node, "Start", 0.0f, 0.0f, 1.0f);
    level.end = input_param(reader, node, "End", 1.0f, 0.0f, 1.0f);
    level.width = input_param(reader, node, "Width", 0.1f, 0.0f, FLT_MAX);
    level.size_random = std::clamp(input_float(reader, node, "Width Random", 0.2f), 0.0f, 1.0f);
    level.frond_tip = std::clamp(input_float(reader, node, "Tip", 0.3f), 0.0f, 1.0f);
    level.frond_round_tip = input_menu(node, "Tip Shape", 0) == 1;
    level.fold = input_param(reader, node, "Fold", 0.0f);
    level.curl = input_param(reader, node, "Curl", 0.0f);
    level.twist = input_float(reader, node, "Twist", 0.0f);
    level.frond_roll_random = std::clamp(input_float(reader, node, "Roll Random", 0.0f), 0.0f, 1.0f);
    level.normal_mode = tree::NormalMode(std::clamp(input_menu(node, "Mode", 3), 0, 4));
    level.soft_normals = input_param(reader, node, "Blend", 0.5f, 0.0f, 1.0f);
    level.segments = std::clamp(input_int(reader, node, "Sections", 8), 1, 128);
    level.frond_across = std::clamp(input_int(reader, node, "Across", 2), 1, 16);
    level.material = std::max(input_int(reader, node, "Material Index", 1), 0);
    read_forces(reader, node, level);
  }
  else if (node.is_type("TreeGraphNodeDrawn")) {
    level.type = tree::LevelType::Drawn;
    read_stem_panels(reader, node, level);
    level.splines = drawn_splines(reader, input_object(node, "Curve"));
    level.radius = input_param(reader, node, "Radius", 0.1f, 0.0f, FLT_MAX);
    level.taper = input_param(reader, node, "Taper", 0.8f, 0.0f, 1.0f);
    level.snap = std::max(input_float(reader, node, "Snap", 0.1f), 0.0f);
    level.weld = input_param(reader, node, "Weld", 0.5f, 0.0f, 1.0f);
    level.segments = std::max(input_int(reader, node, "Sections", 16), 2);
    /* Sides set the rings of the thickest spline, the others count from it. */
    level.sides = std::max(input_int(reader, node, "Sides", 8), 3);
    level.mesh_sides = 0;
    read_forces(reader, node, level);
  }
  else if (node.is_type("TreeGraphNodeIvy")) {
    level.type = tree::LevelType::Ivy;
    level.count = input_int(reader, node, "Count", 3);
    level.count_field = input_param(reader, node, "Count", 3.0f, 0.0f, 64.0f);
    level.start = input_param(reader, node, "Start", 0.0f, 0.0f, 1.0f);
    level.end = input_param(reader, node, "End", 0.8f, 0.0f, 1.0f);
    level.length = input_param(reader, node, "Length", 6.0f, 0.0f, FLT_MAX);
    level.radius = input_param(reader, node, "Radius", 0.015f, 0.0f, FLT_MAX);
    level.taper = input_param(reader, node, "Taper", 0.6f, 0.0f, 1.0f);
    level.angle = input_param(reader, node, "Climb", 1.1f, -1.5f, 1.5f);
    level.wiggle = input_param(reader, node, "Wiggle", 0.4f, 0.0f, 1.5f);
    level.droop = input_param(reader, node, "Hang", 0.2f, 0.0f, 1.0f);
    level.ivy_gap = std::max(input_float(reader, node, "Gap", 0.005f), 0.0f);
    level.segments = std::clamp(input_int(reader, node, "Sections", 48), 2, 512);
    level.mesh_sides = std::clamp(input_int(reader, node, "Sides", 4), 0, 32);
    level.material = std::max(input_int(reader, node, "Material Index", 0), 0);
    level.make_mesh = input_menu(node, "Skin", 0) == 0;
    /* The vines sit on the bark, they don't grow out of it. */
    level.weld = 0.0f;
    level.parent_normals = false;
    read_forces(reader, node, level);
  }
  else if (node.is_type("TreeGraphNodeRoots")) {
    level.type = tree::LevelType::Roots;
    read_stem_panels(reader, node, level);
    level.count = input_int(reader, node, "Count", 5);
    level.count_field = input_param(reader, node, "Count", 5.0f, 0.0f, 256.0f);
    level.length = input_param(reader, node, "Length", 1.5f, 0.0f, FLT_MAX);
    level.radius = input_param(reader, node, "Radius", 0.6f, 0.0f, 1.0f);
    level.taper = input_param(reader, node, "Taper", 0.8f, 0.0f, 1.0f);
    level.depth = input_param(reader, node, "Depth", 0.4f, 0.0f, 1.0f);
    level.gnarl = input_param(reader, node, "Gnarl", 0.2f, 0.0f, FLT_MAX);
    level.weld = input_param(reader, node, "Weld", 0.5f, 0.0f, 1.0f);
    read_forces(reader, node, level);
  }
  else {
    return -1;
  }

  const int index = reader.params.levels.append_and_get_index(std::move(level));
  reader.levels_by_node.add(&node, index);
  for (const bNode *child : linked_nodes(node, "Children")) {
    const int child_index = read_level(reader, *child, depth + 1);
    if (child_index >= 0) {
      reader.params.levels[index].children.append(child_index);
    }
  }
  return index;
}

static const bNode *find_output(const bNodeTree &ntree)
{
  const bNode *found = nullptr;
  for (const bNode *node : ntree.all_nodes()) {
    if (!node->is_type("TreeGraphNodeOutput")) {
      continue;
    }
    if (node->flag & NODE_DO_OUTPUT) {
      return node;
    }
    if (found == nullptr) {
      found = node;
    }
  }
  return found;
}

/** The Vertex Data and Color panels of the Tree Output. */
static void read_vertex_data(GraphReader &reader, const bNode &output, tree::VertexData &data)
{
  static const char *names[tree::vertex_value_count] = {"",
                                                        "Branch Random",
                                                        "Leaf Random",
                                                        "Level",
                                                        "Depth",
                                                        "Along Branch",
                                                        "Height",
                                                        "Radius",
                                                        "Occlusion",
                                                        "Weld",
                                                        "Leaf Facing",
                                                        "Clump Random",
                                                        "Clump Depth",
                                                        "Clump Edge",
                                                        "Gradient Height"};
  for (const int i : IndexRange(1, tree::vertex_value_count - 1)) {
    data.write[i] = input_bool(reader, output, names[i], false);
  }
  data.rays = std::clamp(input_int(reader, output, "Rays", 16), 1, 256);
  data.use_color = input_bool(reader, output, "Use Color", false);
  using V = tree::VertexValue;
  switch (input_menu(output, "Layout", 1)) {
    case 1:
      data.color = {V::BranchRandom, V::LeafRandom, V::AlongBranch, V::Height};
      break;
    case 2:
      data.color = {V::Occlusion, V::Weld, V::LeafFacing, V::Depth};
      break;
    case 3:
      data.color = {V::ClumpDepth, V::ClumpEdge, V::ClumpRandom, V::GradientHeight};
      break;
    default: {
      const char *channels[4] = {"Red", "Green", "Blue", "Alpha"};
      for (const int c : IndexRange(4)) {
        data.color[c] = V(std::clamp(input_menu(output, channels[c], 0),
                                     0,
                                     tree::vertex_value_count - 1));
      }
      break;
    }
  }
}

static void read_graph(const bNodeTree &ntree,
                       const IDProperty *inputs,
                       const Object &self,
                       tree::TreeParams &params)
{
  ntree.ensure_topology_cache();
  ntree.ensure_interface_cache();
  const bNode *output = find_output(ntree);
  if (output == nullptr) {
    return;
  }
  GraphReader reader{
      params,
      tree_fields::FieldCompiler(&self, math::invert(self.object_to_world()), &ntree, inputs)};
  read_vertex_data(reader, *output, params.vertex_data);
  params.conifer = input_menu(*output, "Form", 0) == 1;
  params.seed += input_int(reader, *output, "Seed", 0) * 7919;
  params.detail = std::clamp(input_float(reader, *output, "Detail", 1.0f), 0.02f, 1.0f);
  for (const bNode *node : linked_nodes(*output, "Tree")) {
    const int index = read_level(reader, *node, 0);
    if (index >= 0) {
      params.roots.append(index);
    }
  }
}

/** \} */

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  TreeModifierData &tmd = *reinterpret_cast<TreeModifierData *>(md);
  if (tmd.tree_graph == nullptr) {
    return mesh;
  }

  tree::TreeParams params;
  params.seed = tmd.seed;
  params.scale = tmd.scale;
  /* No triangle limit, the detail settings decide. */
  params.triangle_budget = 0;
  params.leaves = (tmd.flag & MOD_TREE_NO_LEAVES) == 0;
  read_graph(*tmd.tree_graph, tmd.properties, *ctx->object, params);
  if (params.roots.is_empty()) {
    BKE_modifier_set_error(ctx->object, md, "Link a Trunk into the Tree Output of the graph");
    return mesh;
  }

  const bool on_points = tmd.flag & MOD_TREE_ON_POINTS;
  if (on_points && mesh->verts_num == 0) {
    return mesh;
  }
  const Span<float3> positions = on_points ? mesh->vert_positions() : Span<float3>();

  int triangles = 0;
  Mesh *result = tree::mesh_trees(
      params, positions, &triangles, [&]() { return BKE_modifier_stop_requested(ctx); });

  if (DEG_is_active(ctx->depsgraph)) {
    ModifierData *md_orig = BKE_modifier_get_original(ctx->object, md);
    if (md_orig != nullptr) {
      reinterpret_cast<TreeModifierData *>(md_orig)->triangles = triangles;
    }
  }

  if (result == nullptr) {
    return mesh;
  }
  BKE_mesh_copy_parameters_for_eval(result, mesh);
  return result;
}

static void draw_input(ui::Layout &layout,
                       PointerRNA *ptr,
                       const IDProperty *properties,
                       const bNodeTreeInterfaceSocket &socket)
{
  if (!(socket.flag & NODE_INTERFACE_SOCKET_INPUT) || !is_input_type(socket)) {
    return;
  }
  if (socket.flag & NODE_INTERFACE_SOCKET_HIDE_IN_MODIFIER) {
    return;
  }
  /* Values can be removed from Python, then there is nothing to draw until the graph changes. */
  if (IDP_GetPropertyFromGroup_null(properties, socket.identifier) == nullptr) {
    return;
  }
  const std::string path = fmt::format("[\"{}\"]", BLI_str_escape(socket.identifier));
  const std::string name = socket.name ? IFACE_(socket.name) : "";
  ui::Layout &row = layout.row(true);
  row.use_property_decorate_set(true);
  row.prop(ptr, path, UI_ITEM_NONE, name, ICON_NONE);
}

static void draw_inputs(const bContext *C,
                        ui::Layout &layout,
                        PointerRNA *ptr,
                        const IDProperty *properties,
                        const bNodeTreeInterfacePanel &interface_panel)
{
  for (const bNodeTreeInterfaceItem *item : interface_panel.items()) {
    if (item->item_type == NODE_INTERFACE_SOCKET) {
      draw_input(
          layout, ptr, properties, *reinterpret_cast<const bNodeTreeInterfaceSocket *>(item));
    }
    else if (item->item_type == NODE_INTERFACE_PANEL) {
      const auto &sub = *reinterpret_cast<const bNodeTreeInterfacePanel *>(item);
      const std::string idname = fmt::format("tree_inputs_{}", sub.identifier);
      if (ui::Layout *body = layout.panel(C,
                                          idname,
                                          sub.flag & NODE_INTERFACE_PANEL_DEFAULT_CLOSED,
                                          sub.name ? IFACE_(sub.name) : ""))
      {
        draw_inputs(C, *body, ptr, properties, sub);
      }
    }
  }
}

static void panel_draw(const bContext *C, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  ui::template_id(&layout, C, ptr, "tree_graph", "OBJECT_OT_tree_graph_new", nullptr, nullptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_leaves", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_on_points", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  const TreeModifierData &tmd = *static_cast<const TreeModifierData *>(ptr->data);
  if (tmd.tree_graph != nullptr && tmd.properties != nullptr &&
      tmd.properties->data.group.first != nullptr)
  {
    if (ui::Layout *body = layout.panel(C, "tree_inputs", false, IFACE_("Inputs"))) {
      draw_inputs(C, *body, ptr, tmd.properties, tmd.tree_graph->tree_interface.root_panel);
    }
  }

  if (RNA_pointer_get(ptr, "tree_graph").data != nullptr) {
    const int triangles = RNA_int_get(ptr, "triangles");
    char text[64];
    SNPRINTF(text, "%s: %d", IFACE_("Triangles"), triangles);
    ui::Layout &row = layout.row(false, IFACE_("Result"));
    row.label(text, ICON_NONE);
    layout.op("OBJECT_OT_tree_bake_cards", IFACE_("Bake Cards"), ICON_RENDER_STILL);
  }

  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Tree, panel_draw);
}

ModifierTypeInfo modifierType_Tree = {
    /*idname*/ "Tree",
    /*name*/ N_("Tree"),
    /*struct_name*/ "TreeModifierData",
    /*struct_size*/ sizeof(TreeModifierData),
    /*srna*/ &RNA_TreeModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_MOD_TREE,

    /*copy_data*/ copy_data,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ nullptr,
    /*free_data*/ free_data,
    /*is_disabled*/ is_disabled,
    /*update_depsgraph*/ update_depsgraph,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register,
    /*blend_write*/ blend_write,
    /*blend_read*/ blend_read,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
