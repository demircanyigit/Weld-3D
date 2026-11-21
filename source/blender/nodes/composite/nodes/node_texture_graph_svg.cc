/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * SVG: draws a vector drawing from an SVG file into an image at any resolution. A drawing
 * crossing an edge comes back on the other side, so the image still tiles.
 */

#include <string>

#include "BLI_array.hh"
#include "BLI_math_color.h"
#include "BLI_math_vector.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_task.hh"

#include "BKE_main.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "nanosvg.h"
#include "nanosvgrast.h"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_svg_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Mask")
      .structure_type(StructureType::Dynamic)
      .description("White where the drawing is");

  b.add_input<decl::String>("File")
      .subtype(PROP_FILEPATH)
      .path_filter("*.svg")
      .description("SVG file to draw");
  b.add_input<decl::Float>("Scale")
      .default_value(1.0f)
      .min(0.0f)
      .max(100.0f)
      .description("Size of the drawing, 1 fits it inside the image");
  b.add_input<decl::Vector>("Position")
      .dimensions(2)
      .default_value({0.5f, 0.5f})
      .subtype(PROP_FACTOR)
      .description("Where the middle of the drawing goes, from the bottom left (0, 0) to the top "
                   "right (1, 1)");
  b.add_input<decl::Float>("Rotation").default_value(0.0f).subtype(PROP_ANGLE);
  b.add_input<decl::Color>("Background").default_value({0.0f, 0.0f, 0.0f, 0.0f});
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(0))
      .static_items(texture_graph::generator_size_items)
      .description("Size of the image this node makes");
}

using namespace blender::compositor;

class SVGOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const std::string path = this->get_input("File").get_single_value_default<std::string>();
    const float scale = this->get_input("Scale").get_single_value_default<float>();
    const float2 position = this->get_input("Position").get_single_value_default<float2>();
    const float rotation = this->get_input("Rotation").get_single_value_default<float>();
    const float4 background = float4(
        this->get_input("Background").get_single_value_default<Color>());
    const int resolution = this->get_input("Resolution").get_single_value_default<MenuValue>().value;

    const Domain domain = texture_graph::generator_domain(this->context(), resolution);
    const int2 size = domain.data_size;

    int2 picture_size(0);
    Array<float4> picture;
    this->rasterize(path, scale, size, picture, picture_size);

    Result &image = this->get_result("Image");
    Result &mask = this->get_result("Mask");
    if (image.should_compute()) {
      image.allocate_texture(domain);
    }
    if (mask.should_compute()) {
      mask.allocate_texture(domain);
    }

    const float2 position_pixels = position * float2(size);
    const float2 anchor = float2(picture_size) * 0.5f;
    parallel_for(size, [&](const int2 texel) {
      const float4 color = picture_size.x == 0 ? float4(0.0f) :
                                                 texture_graph::place_on_tile(picture.data(),
                                                                              picture_size,
                                                                              anchor,
                                                                              position_pixels,
                                                                              rotation,
                                                                              texel,
                                                                              size);
      if (image.should_compute()) {
        image.store_pixel(texel, Color(color + background * (1.0f - color.w)));
      }
      if (mask.should_compute()) {
        mask.store_pixel(texel, math::clamp(color.w, 0.0f, 1.0f));
      }
    });
  }

  /* Draws the SVG file into a buffer of premultiplied linear colors, fitted inside the image at a
   * scale of 1. */
  void rasterize(const std::string &path,
                 const float scale,
                 const int2 image_size,
                 Array<float4> &r_picture,
                 int2 &r_size)
  {
    if (path.empty() || scale <= 0.0f) {
      return;
    }
    char filepath[FILE_MAX];
    STRNCPY(filepath, path.c_str());
    BLI_path_abs(filepath, BKE_main_blendfile_path_from_global());

    NSVGimage *svg = nsvgParseFromFile(filepath, "px", 96.0f);
    if (svg == nullptr) {
      this->context().set_info_message("Could not read the SVG file");
      return;
    }
    if (svg->width <= 0.0f || svg->height <= 0.0f) {
      nsvgDelete(svg);
      return;
    }

    const float fit = math::min(float(image_size.x) / svg->width,
                                float(image_size.y) / svg->height) *
                      scale;
    r_size = int2(math::ceil(float2(svg->width, svg->height) * fit));
    r_size = math::clamp(r_size, int2(1), int2(16384));

    NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
    if (rasterizer == nullptr) {
      nsvgDelete(svg);
      r_size = int2(0);
      return;
    }
    Array<uchar> bytes(int64_t(r_size.x) * r_size.y * 4, 0);
    nsvgRasterize(rasterizer, svg, 0.0f, 0.0f, fit, bytes.data(), r_size.x, r_size.y, r_size.x * 4);
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(svg);

    /* The drawing comes top row first in sRGB with straight alpha. */
    r_picture = Array<float4>(int64_t(r_size.x) * r_size.y);
    const int2 picture_size = r_size;
    threading::parallel_for(IndexRange(picture_size.y), 64, [&](const IndexRange rows) {
      for (const int y : rows) {
        const uchar *src = bytes.data() + int64_t(picture_size.y - 1 - y) * picture_size.x * 4;
        float4 *dst = r_picture.data() + int64_t(y) * picture_size.x;
        for (int x = 0; x < picture_size.x; x++, src += 4) {
          const float alpha = src[3] / 255.0f;
          dst[x] = float4(srgb_to_linearrgb(src[0] / 255.0f) * alpha,
                          srgb_to_linearrgb(src[1] / 255.0f) * alpha,
                          srgb_to_linearrgb(src[2] / 255.0f) * alpha,
                          alpha);
        }
      }
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new SVGOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeSVG");
  ntype.ui_name = "SVG";
  ntype.ui_description = "Draw a vector drawing from an SVG file into an image at any size";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_SVG";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_svg_cc
