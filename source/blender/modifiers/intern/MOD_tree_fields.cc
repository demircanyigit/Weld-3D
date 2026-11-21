/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <algorithm>
#include <cmath>
#include <memory>

#include "BLI_math_base.hh"
#include "BLI_math_constants.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"
#include "BLI_utildefines.h"

#include "DNA_color_types.h"
#include "DNA_customdata_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "BKE_colortools.hh"
#include "BKE_idprop.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "NOD_math_functions.hh"

#include "MOD_tree_fields.hh"

namespace blender::tree_fields {

using geometry::tree::Param;

/** Links deeper than this are taken as a loop. */
static constexpr int max_depth = 64;

static bool is_scalar(const int type)
{
  return ELEM(type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN, SOCK_MENU);
}

static float3 constant_of(const bNodeSocket &socket)
{
  switch (socket.type) {
    case SOCK_FLOAT:
      return float3(socket.default_value_typed<bNodeSocketValueFloat>()->value);
    case SOCK_INT:
      return float3(float(socket.default_value_typed<bNodeSocketValueInt>()->value));
    case SOCK_BOOLEAN:
      return float3(socket.default_value_typed<bNodeSocketValueBoolean>()->value ? 1.0f : 0.0f);
    case SOCK_MENU:
      return float3(float(socket.default_value_typed<bNodeSocketValueMenu>()->value));
    case SOCK_VECTOR:
      return float3(socket.default_value_typed<bNodeSocketValueVector>()->value);
    case SOCK_RGBA:
      return float3(socket.default_value_typed<bNodeSocketValueRGBA>()->value);
    default:
      return float3(0.0f);
  }
}

static Fn constant(const float3 &value)
{
  return [value](const Context & /*context*/) { return value; };
}

float4 property_value(const IDProperty &property)
{
  switch (property.type) {
    case IDP_FLOAT:
      return float4(IDP_float_get(&property));
    case IDP_DOUBLE:
      return float4(float(IDP_double_get(&property)));
    case IDP_INT:
    case IDP_BOOLEAN:
      return float4(float(property.data.val));
    case IDP_ARRAY: {
      float4 value(0.0f);
      const void *data = property.data.pointer;
      for (int i = 0; i < std::min(property.len, 4); i++) {
        switch (property.subtype) {
          case IDP_FLOAT:
            value[i] = static_cast<const float *>(data)[i];
            break;
          case IDP_DOUBLE:
            value[i] = float(static_cast<const double *>(data)[i]);
            break;
          case IDP_INT:
            value[i] = float(static_cast<const int *>(data)[i]);
            break;
          case IDP_BOOLEAN:
            value[i] = float(static_cast<const int8_t *>(data)[i]);
            break;
        }
      }
      return value;
    }
  }
  return float4(0.0f);
}

void property_value_set(IDProperty &property, const float4 &value)
{
  switch (property.type) {
    case IDP_FLOAT:
      IDP_float_set(&property, value.x);
      break;
    case IDP_DOUBLE:
      IDP_double_set(&property, double(value.x));
      break;
    case IDP_INT:
      property.data.val = int(std::round(value.x));
      break;
    case IDP_BOOLEAN:
      property.data.val = value.x != 0.0f;
      break;
    case IDP_ARRAY: {
      void *data = property.data.pointer;
      for (int i = 0; i < std::min(property.len, 4); i++) {
        switch (property.subtype) {
          case IDP_FLOAT:
            static_cast<float *>(data)[i] = value[i];
            break;
          case IDP_DOUBLE:
            static_cast<double *>(data)[i] = double(value[i]);
            break;
          case IDP_INT:
            static_cast<int *>(data)[i] = int(std::round(value[i]));
            break;
          case IDP_BOOLEAN:
            static_cast<int8_t *>(data)[i] = value[i] != 0.0f;
            break;
        }
      }
      break;
    }
  }
}

FieldCompiler::FieldCompiler(const Object *self,
                             const float4x4 &world_to_tree,
                             const bNodeTree *tree,
                             const IDProperty *inputs)
    : self_(self), world_to_tree_(world_to_tree), tree_(tree), inputs_(inputs)
{
}

/** The default of an input of the graph, from its interface. */
static float3 interface_default(const bNodeTreeInterfaceSocket &socket)
{
  const bke::bNodeSocketType *typeinfo = socket.socket_typeinfo();
  if (typeinfo == nullptr || socket.socket_data == nullptr) {
    return float3(0.0f);
  }
  switch (typeinfo->type) {
    case SOCK_FLOAT:
      return float3(static_cast<const bNodeSocketValueFloat *>(socket.socket_data)->value);
    case SOCK_INT:
      return float3(float(static_cast<const bNodeSocketValueInt *>(socket.socket_data)->value));
    case SOCK_BOOLEAN:
      return float3(static_cast<const bNodeSocketValueBoolean *>(socket.socket_data)->value ? 1.0f :
                                                                                              0.0f);
    case SOCK_VECTOR:
      return float3(static_cast<const bNodeSocketValueVector *>(socket.socket_data)->value);
    case SOCK_RGBA:
      return float3(static_cast<const bNodeSocketValueRGBA *>(socket.socket_data)->value);
    default:
      return float3(0.0f);
  }
}

float3 FieldCompiler::group_input(const StringRef identifier) const
{
  if (inputs_ != nullptr) {
    if (const IDProperty *property = IDP_GetPropertyFromGroup_null(inputs_, identifier)) {
      return property_value(*property).xyz();
    }
  }
  if (tree_ != nullptr) {
    for (const bNodeTreeInterfaceSocket *socket : tree_->interface_inputs()) {
      if (socket->identifier == identifier) {
        return interface_default(*socket);
      }
    }
  }
  return float3(0.0f);
}

bool FieldCompiler::is_linked(const bNodeSocket &input) const
{
  return input.is_available() && !input.logically_linked_sockets().is_empty();
}

Fn FieldCompiler::input(const bNodeSocket &input, const int depth)
{
  const Span<const bNodeSocket *> links = input.is_available() ?
                                              input.logically_linked_sockets() :
                                              Span<const bNodeSocket *>();
  if (links.is_empty() || depth > max_depth) {
    return constant(constant_of(input));
  }
  const bNodeSocket &from = *links.first();
  Fn fn = this->output(from, depth + 1);
  if (is_scalar(input.type) && !is_scalar(from.type)) {
    /* A vector read as a number: the average, as in geometry nodes. */
    return [fn](const Context &context) {
      const float3 v = fn(context);
      return float3((v.x + v.y + v.z) / 3.0f);
    };
  }
  return fn;
}

Fn FieldCompiler::output(const bNodeSocket &output, const int depth)
{
  if (const Fn *fn = outputs_.lookup_ptr(&output)) {
    return *fn;
  }
  Fn fn = this->build_output(output, depth);
  if (!fn) {
    fn = constant(float3(0.0f));
  }
  outputs_.add(&output, fn);
  return fn;
}

Param FieldCompiler::param(const bNodeSocket &input)
{
  Param param(constant_of(input).x);
  if (this->is_linked(input)) {
    Fn fn = this->input(input, 0);
    param.fn = [fn](const Context &context) { return fn(context).x; };
  }
  return param;
}

float FieldCompiler::number(const bNodeSocket &input)
{
  return this->input(input, 0)(Context()).x;
}

int FieldCompiler::integer(const bNodeSocket &input)
{
  return int(std::round(this->number(input)));
}

float3 FieldCompiler::vector(const bNodeSocket &input)
{
  return this->input(input, 0)(Context());
}

/* -------------------------------------------------------------------- */
/** \name Nodes
 * \{ */

static const bNodeSocket *find_socket(const bNode &node,
                                      const bool is_input,
                                      const StringRef identifier)
{
  for (const bNodeSocket *socket : is_input ? node.input_sockets() : node.output_sockets()) {
    if (socket->identifier == identifier) {
      return socket;
    }
  }
  return nullptr;
}

static std::shared_ptr<CurveMapping> curve_copy(const bNode &node)
{
  const CurveMapping *source = static_cast<const CurveMapping *>(node.storage);
  if (source == nullptr) {
    return nullptr;
  }
  CurveMapping *cumap = BKE_curvemapping_copy(source);
  BKE_curvemapping_init(cumap);
  return std::shared_ptr<CurveMapping>(cumap, [](CurveMapping *c) { BKE_curvemapping_free(c); });
}

static float map_range(float value,
                       const float from_min,
                       const float from_max,
                       const float to_min,
                       const float to_max,
                       const float steps,
                       const int interpolation,
                       const bool clamp)
{
  const float range = from_max - from_min;
  float factor = range != 0.0f ? (value - from_min) / range : 0.0f;
  switch (interpolation) {
    case NODE_MAP_RANGE_STEPPED:
      factor = steps > 0.0f ? std::floor(factor * (steps + 1.0f)) / steps : 0.0f;
      break;
    case NODE_MAP_RANGE_SMOOTHSTEP:
      factor = std::clamp(factor, 0.0f, 1.0f);
      factor = (3.0f - 2.0f * factor) * factor * factor;
      break;
    case NODE_MAP_RANGE_SMOOTHERSTEP:
      factor = std::clamp(factor, 0.0f, 1.0f);
      factor = factor * factor * factor * (factor * (factor * 6.0f - 15.0f) + 10.0f);
      break;
    default:
      break;
  }
  float result = to_min + factor * (to_max - to_min);
  if (clamp && ELEM(interpolation, NODE_MAP_RANGE_LINEAR, NODE_MAP_RANGE_STEPPED)) {
    result = std::clamp(result, std::min(to_min, to_max), std::max(to_min, to_max));
  }
  return result;
}

static bool compare_floats(const int operation, const float a, const float b, const float epsilon)
{
  switch (operation) {
    case NODE_COMPARE_LESS_THAN:
      return a < b;
    case NODE_COMPARE_LESS_EQUAL:
      return a <= b;
    case NODE_COMPARE_GREATER_THAN:
      return a > b;
    case NODE_COMPARE_GREATER_EQUAL:
      return a >= b;
    case NODE_COMPARE_EQUAL:
      return std::abs(a - b) <= epsilon;
    case NODE_COMPARE_NOT_EQUAL:
      return std::abs(a - b) > epsilon;
  }
  return false;
}

static bool boolean_math(const int operation, const bool a, const bool b)
{
  switch (operation) {
    case NODE_BOOLEAN_MATH_AND:
      return a && b;
    case NODE_BOOLEAN_MATH_OR:
      return a || b;
    case NODE_BOOLEAN_MATH_NOT:
      return !a;
    case NODE_BOOLEAN_MATH_NAND:
      return !(a && b);
    case NODE_BOOLEAN_MATH_NOR:
      return !(a || b);
    case NODE_BOOLEAN_MATH_XNOR:
      return a == b;
    case NODE_BOOLEAN_MATH_XOR:
      return a != b;
    case NODE_BOOLEAN_MATH_IMPLY:
      return !a || b;
    case NODE_BOOLEAN_MATH_NIMPLY:
      return a && !b;
  }
  return false;
}

static float float_to_int(const int mode, const float value)
{
  switch (mode) {
    case FN_NODE_FLOAT_TO_INT_FLOOR:
      return std::floor(value);
    case FN_NODE_FLOAT_TO_INT_CEIL:
      return std::ceil(value);
    case FN_NODE_FLOAT_TO_INT_TRUNCATE:
      return std::trunc(value);
    default:
      return std::round(value);
  }
}

Fn FieldCompiler::build_output(const bNodeSocket &output, const int depth)
{
  const bNode &node = output.owner_node();
  const StringRef out_id = output.identifier;
  const auto in = [&](const StringRef identifier) -> Fn {
    const bNodeSocket *socket = find_socket(node, true, identifier);
    if (socket == nullptr) {
      return constant(float3(0.0f));
    }
    return this->input(*socket, depth);
  };
  const auto in_index = [&](const int index) -> Fn {
    if (index >= node.input_sockets().size()) {
      return constant(float3(0.0f));
    }
    return this->input(*node.input_sockets()[index], depth);
  };
  const auto linked = [&](const StringRef identifier) {
    const bNodeSocket *socket = find_socket(node, true, identifier);
    return socket != nullptr && this->is_linked(*socket);
  };

  /* Inputs. */
  if (node.is_group_input()) {
    /* The value set on the modifier, the same for the whole tree. */
    return constant(this->group_input(out_id));
  }
  if (node.is_type("ShaderNodeValue")) {
    return constant(constant_of(output));
  }
  if (node.is_type("FunctionNodeInputInt")) {
    return constant(float3(float(static_cast<const NodeInputInt *>(node.storage)->integer)));
  }
  if (node.is_type("FunctionNodeInputBool")) {
    return constant(float3(static_cast<const NodeInputBool *>(node.storage)->boolean ? 1.0f : 0.0f));
  }
  if (node.is_type("FunctionNodeInputVector")) {
    return constant(float3(static_cast<const NodeInputVector *>(node.storage)->vector));
  }
  if (node.is_type("TreeGraphNodeBranchInfo")) {
    if (out_id == "Along Parent") {
      return [](const Context &c) { return float3(c.along_parent); };
    }
    if (out_id == "Along Branch") {
      return [](const Context &c) { return float3(c.along_branch); };
    }
    if (out_id == "Random") {
      return [](const Context &c) { return float3(c.random); };
    }
    if (out_id == "ID") {
      return [](const Context &c) { return float3(float(c.id & 0xffffff)); };
    }
    if (out_id == "Position") {
      return [](const Context &c) { return c.position; };
    }
    if (out_id == "Parent Length") {
      return [](const Context &c) { return float3(c.parent_length); };
    }
    if (out_id == "Parent Radius") {
      return [](const Context &c) { return float3(c.parent_radius); };
    }
    if (out_id == "Depth") {
      return [](const Context &c) { return float3(float(c.depth)); };
    }
    if (out_id == "Index") {
      return [](const Context &c) { return float3(float(c.index)); };
    }
    if (out_id == "Count") {
      return [](const Context &c) { return float3(float(c.count)); };
    }
    if (out_id == "Height") {
      return [](const Context &c) {
        return float3((c.position.z - c.origin.z) / std::max(c.scale, 1e-6f));
      };
    }
    if (out_id == "Distance to Trunk") {
      return [](const Context &c) {
        const float2 d(c.position.x - c.origin.x, c.position.y - c.origin.y);
        return float3(math::length(d) / std::max(c.scale, 1e-6f));
      };
    }
    return nullptr;
  }
  if (node.is_type("TreeGraphNodeZone")) {
    const Fn start = in("Start");
    const Fn end = in("End");
    const Fn side = in("Side");
    const Fn width = in("Width");
    const Fn falloff = in("Falloff");
    const Fn invert = in("Invert");
    return [=](const Context &c) {
      const float soft = std::max(falloff(c).x, 1e-4f);
      const float x = c.along_parent;
      const auto sat = [](const float v) { return std::clamp(v, 0.0f, 1.0f); };
      const float half_band = soft * 0.5f;
      const float band = sat((x - start(c).x) / half_band + 0.5f) *
                         sat((end(c).x - x) / half_band + 0.5f);
      float around = 1.0f;
      const float w = width(c).x;
      if (w < 2.0f * float(M_PI) - 1e-4f) {
        const float a = c.around - side(c).x;
        const float d = std::abs(std::atan2(std::sin(a), std::cos(a)));
        around = sat((w * 0.5f - d) / std::max(soft * float(M_PI), 1e-4f) + 0.5f);
      }
      const float mask = band * around;
      return float3(invert(c).x != 0.0f ? 1.0f - mask : mask);
    };
  }
  if (node.is_type("TreeGraphNodeObjectInfo")) {
    const bNodeSocket *socket = find_socket(node, true, "Object");
    const Object *object = socket ? socket->default_value_typed<bNodeSocketValueObject>()->value :
                                    nullptr;
    if (object == nullptr) {
      return constant(out_id == "Scale" ? float3(1.0f) : float3(0.0f));
    }
    const float4x4 &world = object->object_to_world();
    if (out_id == "Location") {
      return constant(math::transform_point(world_to_tree_, world.location()));
    }
    if (out_id == "Rotation") {
      float3 euler;
      mat4_to_eul(euler, world.ptr());
      return constant(euler);
    }
    float3 size;
    mat4_to_size(size, world.ptr());
    return constant(size);
  }
  if (node.is_type("TreeGraphNodeProfile")) {
    std::shared_ptr<CurveMapping> curve = curve_copy(node);
    const Fn lo = in("Min");
    const Fn hi = in("Max");
    const Fn top = in("Top");
    const bNodeSocket *along_socket = find_socket(node, true, "Along");
    /* 0: along the parent, 1: along the branch, 2: by height. */
    const int along = along_socket ? int(constant_of(*along_socket).x) : 0;
    return [curve, lo, hi, top, along](const Context &c) {
      float x;
      if (along == 1) {
        x = c.along_branch;
      }
      else if (along == 2) {
        x = (c.position.z - c.origin.z) / std::max(top(c).x * c.scale, 1e-6f);
      }
      else {
        x = c.along_parent;
      }
      x = std::clamp(x, 0.0f, 1.0f);
      const float y = curve ? BKE_curvemapping_evaluateF(curve.get(), 0, x) : x;
      const float a = lo(c).x;
      return float3(a + (hi(c).x - a) * y);
    };
  }

  /* Math. */
  if (node.is_type("ShaderNodeMath")) {
    const Fn a = in_index(0);
    const Fn b = in_index(1);
    const Fn c = in_index(2);
    const bool clamp = node.custom2 & SHD_MATH_CLAMP;
    const auto finish = [clamp](float value) {
      return float3(clamp ? std::clamp(value, 0.0f, 1.0f) : value);
    };
    Fn result;
    nodes::try_dispatch_float_math_fl_to_fl(
        node.custom1, [&](auto /*preset*/, auto math_fn, const auto & /*info*/) {
          result = [a, math_fn, finish](const Context &x) { return finish(math_fn(a(x).x)); };
        });
    if (!result) {
      nodes::try_dispatch_float_math_fl_fl_to_fl(
          node.custom1, [&](auto /*preset*/, auto math_fn, const auto & /*info*/) {
            result = [a, b, math_fn, finish](const Context &x) {
              return finish(math_fn(a(x).x, b(x).x));
            };
          });
    }
    if (!result) {
      nodes::try_dispatch_float_math_fl_fl_fl_to_fl(
          node.custom1, [&](auto /*preset*/, auto math_fn, const auto & /*info*/) {
            result = [a, b, c, math_fn, finish](const Context &x) {
              return finish(math_fn(a(x).x, b(x).x, c(x).x));
            };
          });
    }
    return result;
  }
  if (node.is_type("ShaderNodeVectorMath")) {
    const Fn a = in("Vector");
    const Fn b = in("Vector_001");
    const Fn c = in("Vector_002");
    const Fn s = in("Scale");
    const NodeVectorMathOperation op = NodeVectorMathOperation(node.custom1);
    const bool want_value = out_id == "Value";
    Fn vec;
    Fn val;
    nodes::try_dispatch_float_math_fl3_fl3_to_fl3(
        op, [&](auto /*preset*/, auto math_fn, const auto & /*info*/) {
          vec = [a, b, math_fn](const Context &x) { return float3(math_fn(a(x), b(x))); };
        });
    nodes::try_dispatch_float_math_fl3_fl3_fl3_to_fl3(
        op, [&](auto /*preset*/, auto math_fn, const auto & /*info*/) {
          vec = [a, b, c, math_fn](const Context &x) {
            return float3(math_fn(a(x), b(x), c(x)));
          };
        });
    nodes::try_dispatch_float_math_fl3_fl3_fl_to_fl3(
        op, [&](auto /*preset*/, auto math_fn, const auto & /*info*/) {
          vec = [a, b, s, math_fn](const Context &x) {
            return float3(math_fn(a(x), b(x), s(x).x));
          };
        });
    nodes::try_dispatch_float_math_fl3_fl_to_fl3(
        op, [&](auto /*preset*/, auto math_fn, const auto & /*info*/) {
          vec = [a, s, math_fn](const Context &x) { return float3(math_fn(a(x), s(x).x)); };
        });
    nodes::try_dispatch_float_math_fl3_to_fl3(
        op, [&](auto /*preset*/, auto math_fn, const auto & /*info*/) {
          vec = [a, math_fn](const Context &x) { return float3(math_fn(a(x))); };
        });
    nodes::try_dispatch_float_math_fl3_fl3_to_fl(
        op, [&](auto /*preset*/, auto math_fn, const auto & /*info*/) {
          val = [a, b, math_fn](const Context &x) { return float3(float(math_fn(a(x), b(x)))); };
        });
    nodes::try_dispatch_float_math_fl3_to_fl(
        op, [&](auto /*preset*/, auto math_fn, const auto & /*info*/) {
          val = [a, math_fn](const Context &x) { return float3(float(math_fn(a(x)))); };
        });
    return want_value ? val : vec;
  }
  if (node.is_type("ShaderNodeMapRange")) {
    const NodeMapRange &storage = *static_cast<const NodeMapRange *>(node.storage);
    const int interpolation = storage.interpolation_type;
    const bool clamp = storage.clamp;
    if (storage.data_type == CD_PROP_FLOAT3) {
      const Fn v = in("Vector");
      const Fn f0 = in("From_Min_FLOAT3");
      const Fn f1 = in("From_Max_FLOAT3");
      const Fn t0 = in("To_Min_FLOAT3");
      const Fn t1 = in("To_Max_FLOAT3");
      const Fn st = in("Steps_FLOAT3");
      return [=](const Context &x) {
        const float3 vv = v(x), a0 = f0(x), a1 = f1(x), b0 = t0(x), b1 = t1(x), ss = st(x);
        float3 r;
        for (int i = 0; i < 3; i++) {
          r[i] = map_range(vv[i], a0[i], a1[i], b0[i], b1[i], ss[i], interpolation, clamp);
        }
        return r;
      };
    }
    const Fn v = in("Value");
    const Fn f0 = in("From Min");
    const Fn f1 = in("From Max");
    const Fn t0 = in("To Min");
    const Fn t1 = in("To Max");
    const Fn st = in("Steps");
    return [=](const Context &x) {
      return float3(map_range(
          v(x).x, f0(x).x, f1(x).x, t0(x).x, t1(x).x, st(x).x, interpolation, clamp));
    };
  }
  if (node.is_type("ShaderNodeClamp")) {
    const Fn v = in("Value");
    const Fn lo = in("Min");
    const Fn hi = in("Max");
    const bool range = node.custom1 == NODE_CLAMP_RANGE;
    return [=](const Context &x) {
      float a = lo(x).x;
      float b = hi(x).x;
      if (range && a > b) {
        std::swap(a, b);
      }
      return float3(std::clamp(v(x).x, std::min(a, b), std::max(a, b)));
    };
  }
  if (node.is_type("ShaderNodeMix")) {
    const NodeShaderMix &storage = *static_cast<const NodeShaderMix *>(node.storage);
    const bool clamp_factor = storage.clamp_factor;
    const auto fac = [clamp_factor](float f) {
      return clamp_factor ? std::clamp(f, 0.0f, 1.0f) : f;
    };
    if (storage.data_type == SOCK_VECTOR) {
      const bool non_uniform = storage.factor_mode == NODE_MIX_MODE_NON_UNIFORM;
      const Fn f = non_uniform ? in("Factor_Vector") : in("Factor_Float");
      const Fn a = in("A_Vector");
      const Fn b = in("B_Vector");
      return [=](const Context &x) {
        const float3 t = f(x);
        const float3 av = a(x), bv = b(x);
        if (non_uniform) {
          return float3(math::interpolate(av.x, bv.x, fac(t.x)),
                        math::interpolate(av.y, bv.y, fac(t.y)),
                        math::interpolate(av.z, bv.z, fac(t.z)));
        }
        return math::interpolate(av, bv, fac(t.x));
      };
    }
    const Fn f = in("Factor_Float");
    const bool color = storage.data_type == SOCK_RGBA;
    const Fn a = color ? in("A_Color") : in("A_Float");
    const Fn b = color ? in("B_Color") : in("B_Float");
    return [=](const Context &x) { return math::interpolate(a(x), b(x), fac(f(x).x)); };
  }
  if (node.is_type("ShaderNodeCombineXYZ")) {
    const Fn fx = in_index(0);
    const Fn fy = in_index(1);
    const Fn fz = in_index(2);
    return [=](const Context &x) { return float3(fx(x).x, fy(x).x, fz(x).x); };
  }
  if (node.is_type("ShaderNodeSeparateXYZ")) {
    const Fn v = in_index(0);
    const int axis = out_id == "Y" ? 1 : out_id == "Z" ? 2 : 0;
    return [=](const Context &x) { return float3(v(x)[axis]); };
  }
  if (node.is_type("ShaderNodeFloatCurve")) {
    std::shared_ptr<CurveMapping> curve = curve_copy(node);
    const Fn f = in("Factor");
    const Fn v = in("Value");
    if (!curve) {
      return v;
    }
    return [=](const Context &x) {
      const float value = v(x).x;
      return float3(
          math::interpolate(value, BKE_curvemapping_evaluateF(curve.get(), 0, value), f(x).x));
    };
  }

  /* Randomness. */
  if (node.is_type("FunctionNodeRandomValue")) {
    const NodeRandomValue &storage = *static_cast<const NodeRandomValue *>(node.storage);
    const Fn seed_fn = in("Seed");
    const Fn id_fn = linked("ID") ? in("ID") : Fn();
    const auto id_of = [id_fn](const Context &x) {
      return id_fn ? uint32_t(int(std::round(id_fn(x).x))) : x.id;
    };
    const auto seed_of = [seed_fn](const Context &x) {
      return uint32_t(int(std::round(seed_fn(x).x)));
    };
    switch (storage.data_type) {
      case CD_PROP_FLOAT3: {
        const Fn lo = in("Min");
        const Fn hi = in("Max");
        return [=](const Context &x) {
          const uint32_t id = id_of(x), seed = seed_of(x);
          const float3 t(noise::hash_to_float(seed, id, 0),
                         noise::hash_to_float(seed, id, 1),
                         noise::hash_to_float(seed, id, 2));
          const float3 a = lo(x), b = hi(x);
          return a + (b - a) * t;
        };
      }
      case CD_PROP_INT32: {
        const Fn lo = in("Min_002");
        const Fn hi = in("Max_002");
        return [=](const Context &x) {
          const int a = int(std::round(lo(x).x));
          const int b = int(std::round(hi(x).x));
          const int low = std::min(a, b);
          const uint32_t range = uint32_t(std::max(a, b) - low + 1);
          const uint32_t hash = noise::hash(id_of(x), seed_of(x));
          return float3(float(low + int(range == 0 ? hash : hash % range)));
        };
      }
      case CD_PROP_BOOL: {
        const Fn p = in("Probability");
        return [=](const Context &x) {
          return float3(noise::hash_to_float(id_of(x), seed_of(x)) <= p(x).x ? 1.0f : 0.0f);
        };
      }
      default: {
        const Fn lo = in("Min_001");
        const Fn hi = in("Max_001");
        return [=](const Context &x) {
          const float t = noise::hash_to_float(seed_of(x), id_of(x));
          return float3(math::interpolate(lo(x).x, hi(x).x, t));
        };
      }
    }
  }
  if (node.is_type("ShaderNodeTexNoise")) {
    const Fn v = linked("Vector") ? in("Vector") : Fn();
    const Fn w = in("W");
    const Fn scale = in("Scale");
    const Fn detail = in("Detail");
    const Fn roughness = in("Roughness");
    const Fn lacunarity = in("Lacunarity");
    const NodeTexNoise *storage = static_cast<const NodeTexNoise *>(node.storage);
    const int dims = storage ? storage->dimensions : 3;
    const bool normalize = storage ? storage->normalize : true;
    const bool color = out_id == "Color";
    return [=](const Context &x) {
      const float3 p = (v ? v(x) : x.position) * scale(x).x;
      const float ww = w(x).x * scale(x).x;
      const float d = std::clamp(detail(x).x, 0.0f, 15.0f);
      const float r = roughness(x).x;
      const float l = lacunarity(x).x;
      const auto fbm = [&](const float3 &q, const float qw) {
        switch (dims) {
          case 1:
            return noise::perlin_fbm<float>(qw, d, r, l, normalize);
          case 2:
            return noise::perlin_fbm<float2>(float2(q.x, q.y), d, r, l, normalize);
          case 4:
            return noise::perlin_fbm<float4>(float4(q.x, q.y, q.z, qw), d, r, l, normalize);
          default:
            return noise::perlin_fbm<float3>(q, d, r, l, normalize);
        }
      };
      if (!color) {
        return float3(fbm(p, ww));
      }
      return float3(fbm(p, ww),
                    fbm(p + float3(97.3f, 13.1f, 51.7f), ww + 19.3f),
                    fbm(p + float3(31.4f, 77.9f, 3.3f), ww + 41.1f));
    };
  }
  if (node.is_type("ShaderNodeTexWhiteNoise")) {
    const Fn v = linked("Vector") ? in("Vector") : Fn();
    const Fn w = in("W");
    const int dims = node.custom1;
    const bool color = out_id == "Color";
    return [=](const Context &x) {
      const float3 p = v ? v(x) : x.position;
      const auto hash = [&](const float3 &q, const float qw) {
        switch (dims) {
          case 1:
            return noise::hash_float_to_float(qw);
          case 2:
            return noise::hash_float_to_float(float2(q.x, q.y));
          case 4:
            return noise::hash_float_to_float(float4(q.x, q.y, q.z, qw));
          default:
            return noise::hash_float_to_float(q);
        }
      };
      const float ww = w(x).x;
      if (!color) {
        return float3(hash(p, ww));
      }
      return float3(hash(p, ww), hash(p + float3(1.0f), ww + 1.0f), hash(p + float3(2.0f), ww + 2.0f));
    };
  }

  /* Logic. */
  if (node.is_type("FunctionNodeCompare")) {
    const NodeFunctionCompare &storage = *static_cast<const NodeFunctionCompare *>(node.storage);
    const int op = storage.operation;
    const bool ints = storage.data_type == SOCK_INT;
    const Fn a = ints ? in("A_INT") : in("A");
    const Fn b = ints ? in("B_INT") : in("B");
    const Fn eps = in("Epsilon");
    return [=](const Context &x) {
      return float3(compare_floats(op, a(x).x, b(x).x, ints ? 0.0f : eps(x).x) ? 1.0f : 0.0f);
    };
  }
  if (node.is_type("FunctionNodeBooleanMath")) {
    const Fn a = in_index(0);
    const Fn b = in_index(1);
    const int op = node.custom1;
    return [=](const Context &x) {
      return float3(boolean_math(op, a(x).x != 0.0f, b(x).x != 0.0f) ? 1.0f : 0.0f);
    };
  }
  if (node.is_type("FunctionNodeFloatToInt")) {
    const Fn v = in_index(0);
    const int mode = node.custom1;
    return [=](const Context &x) { return float3(float_to_int(mode, v(x).x)); };
  }
  return nullptr;
}

/** \} */

}  // namespace blender::tree_fields
