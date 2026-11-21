/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Texture Graph nodes that draw simple 3D scenes into an image: Height Extrude and Shape Extrude
 * render solids seen by a turned camera, Panorama Shape places lights in a panorama and Triangle
 * Grid draws a jittered triangle mesh from above.
 */

#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_shapes.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_extrude_cc {

namespace tg = texture_graph;
using namespace tg::shapes;
using namespace blender::compositor;

static const EnumPropertyItem normal_format_items[] = {
    {0, "OPENGL", 0, N_("OpenGL"), N_("Green points up")},
    {1, "DIRECTX", 0, N_("DirectX"), N_("Green points down")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void declare_image_input(NodeDeclarationBuilder &b,
                                const char *name,
                                const float d,
                                const char *description)
{
  b.add_input<decl::Float>(name)
      .default_value(d)
      .min(0.0f)
      .max(1.0f)
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic)
      .description(description);
}

static void declare_factor(NodeDeclarationBuilder &b,
                           const char *name,
                           const float d,
                           const char *description)
{
  b.add_input<decl::Float>(name).default_value(d).min(0.0f).max(1.0f).subtype(PROP_FACTOR).description(
      description);
}

static void declare_float(NodeDeclarationBuilder &b,
                          const char *name,
                          const float d,
                          const float min,
                          const float max,
                          const char *description)
{
  b.add_input<decl::Float>(name).default_value(d).min(min).max(max).description(description);
}

static void declare_resolution(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(0))
      .static_items(tg::generator_size_items)
      .description("Size of the image this node makes");
}

class SceneOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

 protected:
  float f(const char *name)
  {
    return this->get_input(name).get_single_value_default<float>();
  }
  int i(const char *name)
  {
    return this->get_input(name).get_single_value_default<int>();
  }
  bool b(const char *name)
  {
    return this->get_input(name).get_single_value_default<bool>();
  }
  int m(const char *name)
  {
    return this->get_input(name).get_single_value_default<MenuValue>().value;
  }
  float2 v(const char *name)
  {
    return this->get_input(name).get_single_value_default<float2>();
  }
  float4 c(const char *name)
  {
    return float4(this->get_input(name).get_single_value_default<Color>());
  }
  Domain output_domain()
  {
    return tg::generator_domain(this->context(), this->m("Resolution"));
  }
};

/* A camera looking at the origin from a direction given by a tilt from straight down and a turn
 * around the vertical. */
struct Camera {
  float3 forward;
  float3 right;
  float3 up;

  Camera(const float tilt_turns, const float turn_turns)
  {
    const float tilt = tilt_turns * float(M_PI * 2.0);
    const float turn = turn_turns * float(M_PI * 2.0);
    forward = float3(math::sin(tilt) * math::cos(turn),
                     math::sin(tilt) * math::sin(turn),
                     -math::cos(tilt));
    right = float3(-math::sin(turn), math::cos(turn), 0.0f);
    up = math::cross(right, forward);
  }
};

/* Where a ray enters and leaves a box, false when it misses. */
static bool box_span(const float3 origin,
                     const float3 dir,
                     const float3 lo,
                     const float3 hi,
                     float &r_near,
                     float &r_far)
{
  r_near = -1e9f;
  r_far = 1e9f;
  for (int k = 0; k < 3; k++) {
    if (math::abs(dir[k]) < 1e-8f) {
      if (origin[k] < lo[k] || origin[k] > hi[k]) {
        return false;
      }
      continue;
    }
    float t0 = (lo[k] - origin[k]) / dir[k];
    float t1 = (hi[k] - origin[k]) / dir[k];
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    r_near = math::max(r_near, t0);
    r_far = math::min(r_far, t1);
  }
  return r_far >= r_near;
}

/* -------------------------------------------------------------------- */
/** \name Height Extrude
 * \{ */

namespace height_extrude {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Height").structure_type(StructureType::Dynamic).description(
      "How near the surface is to the camera");
  b.add_output<decl::Color>("Normal").structure_type(StructureType::Dynamic).description(
      "Normal map of the surface as the camera sees it");
  declare_image_input(b, "Height", 0.5f, "The height to raise into a solid");
  declare_float(b, "Camera Tilt", 0.0f, 0.0f, 0.24f, "Tilt of the camera away from straight down, in turns");
  declare_factor(b, "Camera Turn", 0.0f, "Turn of the camera around the solid");
  declare_float(b, "Camera Scale", 1.0f, 0.001f, 3.0f, "How much of the scene the image shows");
  declare_float(b, "Height Scale", 0.5f, 0.0f, 2.0f, "How tall the solid is");
  declare_float(b, "Vertical Offset", 0.0f, -1.0f, 1.0f, "Raises or lowers the output");
  b.add_input<decl::Bool>("Ground").default_value(true).description(
      "Draw a flat ground where the height is zero, off leaves it black");
  b.add_input<decl::Menu>("Normal Format")
      .default_value(MenuValue(0))
      .static_items(normal_format_items)
      .description("Which way green points in the normal map");
  declare_float(b, "Normal Intensity", 1.0f, 0.0f, 256.0f, "Strength of the normal map");
  declare_resolution(b);
}

class Operation : public SceneOperation {
 public:
  using SceneOperation::SceneOperation;

  void execute() override
  {
    const Picture height = read_picture(this->get_input("Height"));
    const Camera camera(this->f("Camera Tilt"), this->f("Camera Turn"));
    const float camera_scale = math::max(this->f("Camera Scale"), 1e-3f);
    const float height_scale = this->f("Height Scale");
    const float vertical_offset = this->f("Vertical Offset");
    const bool ground = this->b("Ground");
    const bool directx = this->m("Normal Format") == 1;
    const float intensity = this->f("Normal Intensity");

    /* The solid stands on the square from -0.5 to 0.5. */
    const auto height_at = [&](const float2 xy) {
      if (math::abs(xy.x) > 0.5f || math::abs(xy.y) > 0.5f) {
        return ground ? 0.0f : -1.0f;
      }
      return height.sample_clamped(xy + 0.5f) * height_scale;
    };

    const Domain domain = this->output_domain();
    const int2 size = domain.data_size;
    Result &out_height = this->get_result("Height");
    Result &out_normal = this->get_result("Normal");
    if (out_height.should_compute()) {
      out_height.allocate_texture(domain);
    }
    if (out_normal.should_compute()) {
      out_normal.allocate_texture(domain);
    }
    const float texel = 1.0f / float(math::max(size.x, size.y));
    const int steps = math::clamp(math::max(size.x, size.y), 64, 1024);

    parallel_for(size, [&](const int2 pixel) {
      const float2 screen = ((float2(pixel) + 0.5f) / float2(size) - 0.5f) * camera_scale;
      const float3 origin = camera.right * screen.x + camera.up * screen.y - camera.forward * 4.0f;
      float t_near, t_far;
      const float top = math::max(height_scale, 1e-4f);
      const float reach = ground ? 8.0f : 0.5f;
      float value = 0.0f;
      float3 normal(0.0f, 0.0f, 1.0f);
      bool hit = false;
      if (box_span(origin,
                   camera.forward,
                   float3(-reach, -reach, -1e-4f),
                   float3(reach, reach, top + 1e-4f),
                   t_near,
                   t_far))
      {
        t_near = math::max(t_near, 0.0f);
        const float step = (t_far - t_near) / float(steps);
        float prev_t = t_near;
        for (int k = 0; k <= steps; k++) {
          const float t = t_near + step * float(k);
          const float3 p = origin + camera.forward * t;
          if (p.z <= height_at(float2(p.x, p.y))) {
            /* Narrow down the crossing. */
            float lo = prev_t;
            float hi = t;
            for (int r = 0; r < 10; r++) {
              const float mid = (lo + hi) * 0.5f;
              const float3 q = origin + camera.forward * mid;
              if (q.z <= height_at(float2(q.x, q.y))) {
                hi = mid;
              }
              else {
                lo = mid;
              }
            }
            const float3 q = origin + camera.forward * hi;
            if (height_at(float2(q.x, q.y)) >= 0.0f &&
                (ground || height_at(float2(q.x, q.y)) > 1e-5f))
            {
              hit = true;
              value = -math::dot(q, camera.forward);
              const float e = texel * camera_scale;
              const float dx = (height_at(float2(q.x + e, q.y)) - height_at(float2(q.x - e, q.y))) /
                               (2.0f * e);
              const float dy = (height_at(float2(q.x, q.y + e)) - height_at(float2(q.x, q.y - e))) /
                               (2.0f * e);
              normal = math::normalize(float3(-dx * intensity, -dy * intensity, 1.0f));
            }
            break;
          }
          prev_t = t;
        }
      }
      if (out_height.should_compute()) {
        out_height.store_pixel(pixel, hit ? value + vertical_offset : 0.0f);
      }
      if (out_normal.should_compute()) {
        float3 n(0.0f, 0.0f, 1.0f);
        if (hit) {
          n = float3(math::dot(normal, camera.right),
                     math::dot(normal, camera.up),
                     -math::dot(normal, camera.forward));
        }
        if (directx) {
          n.y = -n.y;
        }
        out_normal.store_pixel(pixel, Color(float4(n * 0.5f + 0.5f, 1.0f)));
      }
    });
  }
};

}  // namespace height_extrude

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shape Extrude
 * \{ */

namespace shape_extrude {

static const EnumPropertyItem section_items[] = {
    {0, "CUBE", 0, N_("Cube"), N_("A box")},
    {1, "CYLINDER", 0, N_("Cylinder"), N_("A round column")},
    {2, "CUSTOM", 0, N_("Custom"), N_("The Shape input, white is inside")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem profile_items[] = {
    {0, "STRAIGHT", 0, N_("Straight"), N_("The same section all the way")},
    {1, "GRADIENT", 0, N_("Gradient"), N_("The Profile input scales the section along the length")},
    {2, "MASK", 0, N_("Mask"), N_("The Profile input cuts out parts along the length")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem range_items[] = {
    {0, "POSITIVE", 0, N_("0 to 1"), N_("Heights below zero are cut to zero")},
    {1, "SIGNED", 0, N_("-1 to 1"), N_("Heights below zero are kept")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Height").structure_type(StructureType::Dynamic);
  declare_image_input(b, "Shape", 1.0f, "The section when Section is Custom, white is inside");
  declare_image_input(
      b, "Profile", 1.0f, "Scales or cuts the section along its length, bottom to top");
  declare_float(b, "Extrude Height", 0.5f, 0.0f, 1.0f, "How far the solid reaches up from its middle");
  declare_float(b, "Extrude Depth", 0.0f, 0.0f, 1.0f, "How far the solid reaches down from its middle");
  b.add_input<decl::Menu>("Section")
      .default_value(MenuValue(0))
      .static_items(section_items)
      .description("The shape that is pushed out");
  b.add_input<decl::Vector>("Section Size")
      .subtype(PROP_FACTOR)
      .dimensions(2)
      .default_value({0.5f, 0.5f})
      .min(0.0f)
      .max(1.0f)
      .description("Width and depth of the box or column");
  declare_float(b, "Scale", 1.0f, 0.0f, 2.0f, "Size of the section");
  declare_factor(b, "Rotation X", 0.0f, "Turn around the left to right axis");
  declare_factor(b, "Rotation Y", 0.0f, "Turn around the bottom to top axis");
  declare_factor(b, "Rotation Z", 0.0f, "Turn around the axis facing the camera");
  b.add_input<decl::Menu>("Profile Type")
      .default_value(MenuValue(0))
      .static_items(profile_items)
      .description("How the Profile input is used");
  declare_factor(b, "Bevel Height", 0.0f, "How far down from the top the bevel reaches");
  declare_factor(b, "Bevel Amount", 0.0f, "How far the bevel pulls the section in");
  declare_float(b, "Bevel Curve", 0.0f, -1.0f, 1.0f, "Rounder or hollower bevel");
  b.add_input<decl::Bool>("Mirror Bevel").default_value(false).description(
      "Bevel the bottom end too");
  b.add_input<decl::Menu>("Output Range")
      .default_value(MenuValue(0))
      .static_items(range_items)
      .description("Whether heights below zero are kept");
  declare_resolution(b);
}

static float3 rotate_xyz(const float3 p, const float3 turns)
{
  float3 r = p;
  const float3 a = turns * float(M_PI * 2.0);
  r = float3(r.x, r.y * math::cos(a.x) - r.z * math::sin(a.x), r.y * math::sin(a.x) + r.z * math::cos(a.x));
  r = float3(r.x * math::cos(a.y) + r.z * math::sin(a.y), r.y, -r.x * math::sin(a.y) + r.z * math::cos(a.y));
  r = float3(r.x * math::cos(a.z) - r.y * math::sin(a.z), r.x * math::sin(a.z) + r.y * math::cos(a.z), r.z);
  return r;
}

/* Undoes #rotate_xyz. */
static float3 unrotate_xyz(const float3 p, const float3 turns)
{
  float3 r = p;
  const float3 a = -turns * float(M_PI * 2.0);
  r = float3(r.x * math::cos(a.z) - r.y * math::sin(a.z), r.x * math::sin(a.z) + r.y * math::cos(a.z), r.z);
  r = float3(r.x * math::cos(a.y) + r.z * math::sin(a.y), r.y, -r.x * math::sin(a.y) + r.z * math::cos(a.y));
  r = float3(r.x, r.y * math::cos(a.x) - r.z * math::sin(a.x), r.y * math::sin(a.x) + r.z * math::cos(a.x));
  return r;
}

class Operation : public SceneOperation {
 public:
  using SceneOperation::SceneOperation;

  void execute() override
  {
    const Picture section_image = read_picture(this->get_input("Shape"));
    const Picture profile = read_picture(this->get_input("Profile"));
    const float extrude_height = this->f("Extrude Height");
    const float extrude_depth = this->f("Extrude Depth");
    const int section = this->m("Section");
    const float2 section_size = this->v("Section Size");
    const float scale = this->f("Scale");
    const float3 turns(this->f("Rotation X"), this->f("Rotation Y"), this->f("Rotation Z"));
    const int profile_type = this->m("Profile Type");
    const float bevel_height = this->f("Bevel Height");
    const float bevel_amount = this->f("Bevel Amount");
    const float bevel_curve = this->f("Bevel Curve");
    const bool mirror_bevel = this->b("Mirror Bevel");
    const bool signed_range = this->m("Output Range") == 1;
    const float length = math::max(extrude_height + extrude_depth, 1e-4f);

    /* Inside test in the solid's own space: the section lies across x and y, the length runs
     * along z from -depth to height. */
    const auto inside = [&](const float3 p) {
      if (p.z > extrude_height || p.z < -extrude_depth) {
        return false;
      }
      const float along = (p.z + extrude_depth) / length;
      float s = scale;
      if (profile_type == 1) {
        s *= profile.sample_clamped(float2(0.5f, along));
      }
      else if (profile_type == 2 && profile.sample_clamped(float2(0.5f, along)) < 0.5f) {
        return false;
      }
      if (bevel_height > 0.0f && bevel_amount > 0.0f) {
        const float from_top = (extrude_height - p.z) / (bevel_height * length);
        float pull = from_top < 1.0f ? 1.0f - from_top : 0.0f;
        if (mirror_bevel) {
          const float from_bottom = (p.z + extrude_depth) / (bevel_height * length);
          pull = math::max(pull, from_bottom < 1.0f ? 1.0f - from_bottom : 0.0f);
        }
        pull = std::pow(pull, std::exp2(bevel_curve * 2.0f));
        s *= 1.0f - bevel_amount * pull;
      }
      if (s <= 1e-5f) {
        return false;
      }
      const float2 q = float2(p.x, p.y) / s;
      switch (section) {
        case 1: {
          const float2 r = q / math::max(section_size * 0.5f, float2(1e-4f));
          return math::length_squared(r) <= 1.0f;
        }
        case 2:
          return math::abs(q.x) <= 0.5f && math::abs(q.y) <= 0.5f &&
                 section_image.sample_clamped(q + 0.5f) >= 0.5f;
        default:
          return math::abs(q.x) <= section_size.x * 0.5f &&
                 math::abs(q.y) <= section_size.y * 0.5f;
      }
    };

    const Domain domain = this->output_domain();
    const int2 size = domain.data_size;
    Result &output = this->get_result("Height");
    output.allocate_texture(domain);
    const int steps = math::clamp(math::max(size.x, size.y), 64, 768);
    parallel_for(size, [&](const int2 pixel) {
      const float2 xy = (float2(pixel) + 0.5f) / float2(size) - 0.5f;
      /* A ray straight down, taken into the solid's own space. */
      const float3 origin = unrotate_xyz(float3(xy.x, xy.y, 2.0f), turns);
      const float3 dir = unrotate_xyz(float3(0.0f, 0.0f, -1.0f), turns);
      const float reach = 1.5f;
      float t_near, t_far;
      float value = signed_range ? -1.0f : 0.0f;
      if (box_span(origin,
                   dir,
                   float3(-reach, -reach, -extrude_depth),
                   float3(reach, reach, extrude_height),
                   t_near,
                   t_far))
      {
        t_near = math::max(t_near, 0.0f);
        const float step = (t_far - t_near) / float(steps);
        for (int k = 0; k <= steps; k++) {
          const float t = t_near + step * float(k);
          if (inside(origin + dir * t)) {
            float lo = t - step;
            float hi = t;
            for (int r = 0; r < 8; r++) {
              const float mid = (lo + hi) * 0.5f;
              if (inside(origin + dir * mid)) {
                hi = mid;
              }
              else {
                lo = mid;
              }
            }
            value = 2.0f - hi;
            break;
          }
        }
      }
      output.store_pixel(pixel, signed_range ? value : math::max(value, 0.0f));
    });
  }
};

}  // namespace shape_extrude

/** \} */

/* -------------------------------------------------------------------- */
/** \name Panorama Shape
 * \{ */

namespace panorama_shape {

static const EnumPropertyItem light_items[] = {
    {0, "SQUARE", 0, N_("Square"), N_("A square panel")},
    {1, "DISC", 0, N_("Disc"), N_("A round light")},
    {2, "IMAGE", 0, N_("Image"), N_("The Light Image input")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_input<decl::Color>("Background")
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic)
      .description("A panorama to add the light to, connect another Panorama Shape to chain them");
  b.add_input<decl::Float>("Light Image")
      .default_value(1.0f)
      .hide_value()
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic)
      .usage_by_menu("Shape", 2)
      .description("The light's shape when Shape is Image");
  b.add_input<decl::Menu>("Shape")
      .default_value(MenuValue(0))
      .static_items(light_items)
      .description("The light's shape");
  declare_factor(b, "Longitude", 0.5f, "Where around the horizon the light is");
  declare_float(b, "Latitude", 0.2f, -1.0f, 1.0f, "How high the light is, 1 is straight up");
  b.add_input<decl::Vector>("Size")
      .subtype(PROP_FACTOR)
      .dimensions(2)
      .default_value({0.25f, 0.15f})
      .min(0.0f)
      .max(0.95f)
      .description("Width and height of the light");
  declare_factor(b, "Rotation", 0.0f, "Turn of the light");
  b.add_input<decl::Color>("Light Color").default_value({1.0f, 1.0f, 1.0f, 1.0f});
  declare_float(b, "Intensity", 10.0f, 0.0f, 100.0f, "Brightness of the light, above 1 for HDR");
  declare_factor(b, "Soft Border", 0.1f, "Softness of the light's edge");
  declare_float(b, "Hotspot Intensity", 0.0f, 0.0f, 100.0f, "Brightness of a bright spot on the light");
  declare_factor(b, "Hotspot Size", 0.3f, "Size of the bright spot");
  declare_factor(b, "Hotspot Falloff", 0.5f, "How softly the bright spot fades");
  b.add_input<decl::Vector>("Hotspot Position")
      .subtype(PROP_FACTOR)
      .dimensions(2)
      .default_value({0.0f, 0.0f})
      .min(-1.0f)
      .max(1.0f)
      .description("Where the bright spot is on the light");
  b.add_input<decl::Bool>("Fill Background").default_value(false).description(
      "Use the color below instead of the Background input");
  b.add_input<decl::Color>("Background Color").default_value({0.0f, 0.0f, 0.0f, 1.0f});
  declare_resolution(b);
}

class Operation : public SceneOperation {
 public:
  using SceneOperation::SceneOperation;

  void execute() override
  {
    const Result &background = this->get_input("Background");
    const Picture light_image = read_picture(this->get_input("Light Image"));
    const int shape = this->m("Shape");
    const float longitude = this->f("Longitude") * float(M_PI * 2.0) - float(M_PI);
    const float latitude = math::clamp(this->f("Latitude"), -1.0f, 1.0f) * float(M_PI) * 0.5f;
    const float2 half_tan(std::tan(math::clamp(this->v("Size").x, 0.01f, 0.95f) * float(M_PI) * 0.5f),
                          std::tan(math::clamp(this->v("Size").y, 0.01f, 0.95f) * float(M_PI) * 0.5f));
    const float rotation = this->f("Rotation") * float(M_PI * 2.0);
    const float3 color = this->c("Light Color").xyz();
    const float intensity = this->f("Intensity");
    const float soft = this->f("Soft Border");
    const float hot_intensity = this->f("Hotspot Intensity");
    const float hot_size = math::max(this->f("Hotspot Size"), 1e-3f);
    const float hot_falloff = this->f("Hotspot Falloff");
    const float2 hot_position = this->v("Hotspot Position");
    const bool fill = this->b("Fill Background");
    const float4 fill_color = this->c("Background Color");

    const auto direction = [](const float lon, const float lat) {
      return float3(math::cos(lat) * math::cos(lon), math::cos(lat) * math::sin(lon), math::sin(lat));
    };
    const float3 center = direction(longitude, latitude);
    const float3 east = math::normalize(float3(-math::sin(longitude), math::cos(longitude), 0.0f));
    const float3 north = math::cross(center, east);

    int2 background_size(0);
    Array<float4> background_colors;
    if (!fill && !background.is_single_value()) {
      background_size = background.domain().data_size;
      background_colors = tg::read_colors(background, background_size);
    }

    const Domain domain = this->output_domain();
    const int2 size = domain.data_size;
    Result &output = this->get_result("Image");
    output.allocate_texture(domain);
    parallel_for(size, [&](const int2 texel) {
      const float2 uv = (float2(texel) + 0.5f) / float2(size);
      float4 result;
      if (fill) {
        result = fill_color;
      }
      else if (background_size.x > 0) {
        result = tg::sample_wrapped(
            background_colors.as_span(), background_size, uv * float2(background_size));
      }
      else {
        result = float4(background.get_single_value<Color>());
      }
      const float3 d = direction((uv.x - 0.5f) * float(M_PI * 2.0), (uv.y - 0.5f) * float(M_PI));
      const float facing = math::dot(d, center);
      if (facing > 1e-3f) {
        /* Project onto the plane touching the sphere at the light's center. */
        const float2 plane = rotate(float2(math::dot(d, east), math::dot(d, north)) / facing,
                                    -rotation);
        const float2 q = plane / half_tan;
        float mask = 0.0f;
        switch (shape) {
          case 1:
            mask = soft_edge(math::length(q), 1.0f, soft);
            break;
          case 2:
            mask = light_image.sample_clamped(q * 0.5f + 0.5f);
            break;
          default:
            mask = soft_edge(math::max(math::abs(q.x), math::abs(q.y)), 1.0f, soft);
            break;
        }
        if (mask > 0.0f) {
          const float r = math::length(q - hot_position) / hot_size;
          const float hot = std::exp(-r * r / math::max(hot_falloff * 2.0f, 0.05f));
          const float3 light = (color * intensity + float3(hot_intensity * hot)) * mask;
          result = float4(result.xyz() + light, result.w);
        }
      }
      output.store_pixel(texel, Color(result));
    });
  }
};

}  // namespace panorama_shape

/** \} */

/* -------------------------------------------------------------------- */
/** \name Triangle Grid
 * \{ */

namespace triangle_grid {

static const EnumPropertyItem style_items[] = {
    {0, "PER_VERTEX", 0, N_("Per Vertex"), N_("Values blend smoothly across each triangle")},
    {1, "PER_TRIANGLE", 0, N_("Per Triangle"), N_("One flat value per triangle")},
    {2, "LINES", 0, N_("Lines"), N_("Only the edges of the triangles")},
    {3, "EDGE_DISTANCE", 0, N_("Edge Distance"), N_("Bright inside, fading toward the edges")},
    {4, "CENTER", 0, N_("Center"), N_("Bright at each triangle's middle")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem split_items[] = {
    {0, "DIAGONAL", 0, N_("Diagonal"), N_("Split every square from bottom left to top right")},
    {1, "OTHER_DIAGONAL", 0, N_("Other Diagonal"), N_("Split every square from top left to bottom right")},
    {2, "ALTERNATE", 0, N_("Alternate"), N_("Neighboring squares split the other way")},
    {3, "RANDOM", 0, N_("Random"), N_("Every square picks a way at random")},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem source_items[] = {
    {0, "HEIGHT", 0, N_("Height"), N_("The Height input and Height Random")},
    {1, "RANDOM", 0, N_("Random"), N_("A random value per point")},
    {2, "COLOR", 0, N_("Color"), N_("The Color input")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  declare_image_input(b, "Height", 0.5f, "Height of the points");
  b.add_input<decl::Color>("Color")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .structure_type(StructureType::Dynamic)
      .usage_by_menu("Source", 2)
      .description("Color of the points when Source is Color");
  b.add_input<decl::Menu>("Style")
      .default_value(MenuValue(1))
      .static_items(style_items)
      .description("How the triangles are drawn");
  b.add_input<decl::Menu>("Split")
      .default_value(MenuValue(2))
      .static_items(split_items)
      .description("Which way each square is cut into two triangles");
  b.add_input<decl::Int>("X Amount").default_value(8).min(1).max(128).description(
      "Points across");
  b.add_input<decl::Int>("Y Amount").default_value(8).min(1).max(128).description("Points down");
  b.add_input<decl::Vector>("Position Random")
      .subtype(PROP_FACTOR)
      .dimensions(2)
      .default_value({0.3f, 0.3f})
      .min(0.0f)
      .max(0.45f)
      .description("Random move of every point, as a part of a cell");
  declare_float(b, "Row Offset", 0.0f, -0.5f, 0.5f, "Shifts every other row sideways");
  declare_float(b, "Column Offset", 0.0f, -0.5f, 0.5f, "Shifts every other column up");
  b.add_input<decl::Menu>("Source")
      .default_value(MenuValue(1))
      .static_items(source_items)
      .description("Where the values of the points come from");
  declare_factor(b, "Height Amount", 1.0f, "How much the Height input counts");
  declare_factor(b, "Height Random", 0.0f, "Random height added per point");
  declare_factor(b, "Line Width", 0.05f, "Width of the lines, as a part of a cell");
  declare_factor(b, "Edge Width", 0.3f, "How far from the edges the fade reaches, as a part of a cell");
  declare_factor(b, "Line Value", 1.0f, "Brightness of the lines");
  declare_factor(b, "Background Value", 0.0f, "Brightness between the lines");
  b.add_input<decl::Int>("Seed").default_value(0).description(
      "Change it to get a different layout");
  declare_resolution(b);
}

class Operation : public SceneOperation {
 public:
  using SceneOperation::SceneOperation;

  void execute() override
  {
    const Picture height = read_picture(this->get_input("Height"));
    const Result &color_input = this->get_input("Color");
    int2 color_size(0);
    Array<float4> colors;
    float4 color_single(1.0f);
    if (color_input.is_single_value()) {
      color_single = float4(color_input.get_single_value<Color>());
    }
    else {
      color_size = color_input.domain().data_size;
      colors = tg::read_colors(color_input, color_size);
    }
    const int style = this->m("Style");
    const int split = this->m("Split");
    const int2 amount = math::max(int2(this->i("X Amount"), this->i("Y Amount")), int2(1));
    const float2 position_random = math::min(this->v("Position Random"), float2(0.45f));
    const float row_offset = this->f("Row Offset");
    const float column_offset = this->f("Column Offset");
    const int source = this->m("Source");
    const float height_amount = this->f("Height Amount");
    const float height_random = this->f("Height Random");
    const float line_width = this->f("Line Width");
    const float edge_width = math::max(this->f("Edge Width"), 1e-4f);
    const float line_value = this->f("Line Value");
    const float background_value = this->f("Background Value");
    const uint32_t seed = uint32_t(this->i("Seed"));

    /* Points in cell units, ids wrap so the mesh tiles. */
    const auto point = [&](const int2 id) {
      const int2 w = tg::wrap_texel(id, amount);
      uint32_t state = tg::hash(w.x, w.y, seed);
      const float2 jitter(random_float(state) - 0.5f, random_float(state) - 0.5f);
      return float2(id) + jitter * 2.0f * position_random +
             float2((w.y & 1) ? row_offset : 0.0f, (w.x & 1) ? column_offset : 0.0f);
    };
    const auto value_of = [&](const int2 id) -> float4 {
      const int2 w = tg::wrap_texel(id, amount);
      uint32_t state = tg::hash(w.x, w.y, seed ^ 0x51ed270bu);
      const float random = random_float(state);
      const float2 uv = fract_vec(point(id) / float2(amount));
      switch (source) {
        case 1:
          return float4(float3(random), 1.0f);
        case 2:
          return color_size.x > 0 ? tg::sample_wrapped(colors.as_span(), color_size, uv * float2(color_size)) :
                                    color_single;
        default:
          return float4(float3(height_amount * height.sample_wrapped(uv) + height_random * random), 1.0f);
      }
    };
    const auto split_up = [&](const int2 id) {
      switch (split) {
        case 1:
          return false;
        case 2:
          return ((id.x + id.y) & 1) == 0;
        case 3: {
          const int2 w = tg::wrap_texel(id, amount);
          return tg::hash_to_float(tg::hash(w.x, w.y, seed ^ 0x2545f491u)) < 0.5f;
        }
        default:
          return true;
      }
    };

    const Domain domain = this->output_domain();
    const int2 size = domain.data_size;
    Result &output = this->get_result("Image");
    output.allocate_texture(domain);
    parallel_for(size, [&](const int2 texel) {
      const float2 p = (float2(texel) + 0.5f) / float2(size) * float2(amount);
      const int2 base = int2(math::floor(p));
      float4 result(float3(background_value), 1.0f);
      bool found = false;
      for (int dy = -1; dy <= 1 && !found; dy++) {
        for (int dx = -1; dx <= 1 && !found; dx++) {
          const int2 c = base + int2(dx, dy);
          const int2 ids[4] = {c, c + int2(1, 0), c + int2(1, 1), c + int2(0, 1)};
          const float2 v[4] = {point(ids[0]), point(ids[1]), point(ids[2]), point(ids[3])};
          int tris[2][3];
          if (split_up(c)) {
            tris[0][0] = 0, tris[0][1] = 1, tris[0][2] = 2;
            tris[1][0] = 0, tris[1][1] = 2, tris[1][2] = 3;
          }
          else {
            tris[0][0] = 0, tris[0][1] = 1, tris[0][2] = 3;
            tris[1][0] = 1, tris[1][1] = 2, tris[1][2] = 3;
          }
          for (int t = 0; t < 2 && !found; t++) {
            const float2 a = v[tris[t][0]];
            const float2 b = v[tris[t][1]];
            const float2 cc = v[tris[t][2]];
            const float area = (b.x - a.x) * (cc.y - a.y) - (b.y - a.y) * (cc.x - a.x);
            if (math::abs(area) < 1e-8f) {
              continue;
            }
            const float w0 = ((b.x - p.x) * (cc.y - p.y) - (b.y - p.y) * (cc.x - p.x)) / area;
            const float w1 = ((cc.x - p.x) * (a.y - p.y) - (cc.y - p.y) * (a.x - p.x)) / area;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
              continue;
            }
            found = true;
            const auto edge_distance = [&](const float2 e0, const float2 e1) {
              const float2 e = e1 - e0;
              return math::abs(e.x * (p.y - e0.y) - e.y * (p.x - e0.x)) /
                     math::max(math::length(e), 1e-6f);
            };
            const float edge = math::min(math::min(edge_distance(a, b), edge_distance(b, cc)),
                                         edge_distance(cc, a));
            const float4 va = value_of(ids[tris[t][0]]);
            const float4 vb = value_of(ids[tris[t][1]]);
            const float4 vc = value_of(ids[tris[t][2]]);
            switch (style) {
              case 0:
                result = va * w0 + vb * w1 + vc * w2;
                break;
              case 1:
                result = (va + vb + vc) / 3.0f;
                break;
              case 2: {
                const float pixel = float(amount.x) / float(size.x);
                const float line = math::clamp((line_width * 0.5f - edge) / pixel + 0.5f, 0.0f, 1.0f);
                result = float4(float3(math::interpolate(background_value, line_value, line)), 1.0f);
                break;
              }
              case 3:
                result = float4(float3(math::clamp(edge / edge_width, 0.0f, 1.0f)), 1.0f) *
                         ((va + vb + vc) / 3.0f);
                break;
              default:
                result = float4(float3(math::clamp(math::min(math::min(w0, w1), w2) * 3.0f, 0.0f, 1.0f)),
                                1.0f) *
                         ((va + vb + vc) / 3.0f);
                break;
            }
            result.w = 1.0f;
          }
        }
      }
      output.store_pixel(texel, Color(result));
    });
  }
};

}  // namespace triangle_grid

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

template<typename T> static NodeOperation *make_operation(Context &context, const bNode &node)
{
  return new T(context, node);
}

static void register_one(bke::bNodeType &ntype,
                         const char *idname,
                         const char *name,
                         const char *description,
                         const char *enum_name,
                         bke::NodeDeclareFunction declare,
                         bke::NodeGetCompositorOperationFunction operation)
{
  tg_node_type_base(&ntype, idname);
  ntype.ui_name = name;
  ntype.ui_description = description;
  ntype.enum_name_legacy = enum_name;
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = operation;
  bke::node_register_type(ntype);
}

static void node_register()
{
  static bke::bNodeType types[4];
  register_one(types[0],
               "TextureGraphNodeHeightExtrude",
               "Height Extrude",
               "Raise a height into a solid and see it from a turned camera",
               "TEXTURE_GRAPH_HEIGHT_EXTRUDE",
               height_extrude::node_declare,
               make_operation<height_extrude::Operation>);
  register_one(types[1],
               "TextureGraphNodeShapeExtrude",
               "Shape Extrude",
               "Push a shape out into a solid, turn it and see its height from above",
               "TEXTURE_GRAPH_SHAPE_EXTRUDE",
               shape_extrude::node_declare,
               make_operation<shape_extrude::Operation>);
  register_one(types[2],
               "TextureGraphNodePanoramaShape",
               "Panorama Shape",
               "Place a light panel in a panorama",
               "TEXTURE_GRAPH_PANORAMA_SHAPE",
               panorama_shape::node_declare,
               make_operation<panorama_shape::Operation>);
  register_one(types[3],
               "TextureGraphNodeTriangleGrid",
               "Triangle Grid",
               "A grid of jittered triangles seen from above",
               "TEXTURE_GRAPH_TRIANGLE_GRID",
               triangle_grid::node_declare,
               make_operation<triangle_grid::Operation>);
}
NOD_REGISTER_NODE(node_register)

/** \} */

}  // namespace blender::nodes::node_texture_graph_extrude_cc
