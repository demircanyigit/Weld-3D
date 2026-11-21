/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Text: draws text with a font file into an image. Text crossing an edge comes back on the other
 * side, so the image still tiles.
 */

#include <string>

#include "BLI_array.hh"
#include "BLI_math_vector.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_main.hh"

#include "BLF_api.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
#include "texture_graph_util.hh"

namespace blender::nodes::node_texture_graph_text_cc {

enum class Alignment : int {
  Left = 0,
  Center = 1,
  Right = 2,
};

static const EnumPropertyItem alignment_items[] = {
    {int(Alignment::Left), "LEFT", 0, N_("Left"), N_("Line up the text on its left side")},
    {int(Alignment::Center), "CENTER", 0, N_("Center"), N_("Center the text")},
    {int(Alignment::Right), "RIGHT", 0, N_("Right"), N_("Line up the text on its right side")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Mask")
      .structure_type(StructureType::Dynamic)
      .description("White where the text is");

  b.add_input<decl::String>("Text").default_value("Text").description(
      "Text to draw, a new line starts a new row");
  b.add_input<decl::String>("Font")
      .subtype(PROP_FILEPATH)
      .path_filter("*.ttf;*.otf;*.woff;*.woff2")
      .description("Font file to use, leave empty for the built-in font");
  b.add_input<decl::Float>("Size")
      .default_value(0.2f)
      .min(0.0f)
      .max(4.0f)
      .description("Height of a row of text, as a part of the image height");
  b.add_input<decl::Vector>("Position")
      .dimensions(2)
      .default_value({0.5f, 0.5f})
      .subtype(PROP_FACTOR)
      .description("Where the text goes, from the bottom left (0, 0) to the top right (1, 1)");
  b.add_input<decl::Float>("Rotation").default_value(0.0f).subtype(PROP_ANGLE);
  b.add_input<decl::Menu>("Alignment")
      .default_value(MenuValue(Alignment::Center))
      .static_items(alignment_items)
      .optional_label();
  b.add_input<decl::Color>("Color").default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Color>("Background").default_value({0.0f, 0.0f, 0.0f, 1.0f});
  b.add_input<decl::Menu>("Resolution")
      .default_value(MenuValue(0))
      .static_items(texture_graph::generator_size_items)
      .description("Size of the image this node makes");
}

using namespace blender::compositor;

/* Font used when no font file is given. Loaded once, kept for the session. */
static int default_font_id()
{
  static int font_id = -1;
  if (font_id == -1) {
    font_id = BLF_load_default(false);
  }
  return font_id;
}

static int load_font(const std::string &path)
{
  if (path.empty()) {
    return default_font_id();
  }
  char filepath[FILE_MAX];
  STRNCPY(filepath, path.c_str());
  BLI_path_abs(filepath, BKE_main_blendfile_path_from_global());
  const int font_id = BLF_load(filepath);
  return font_id == -1 ? default_font_id() : font_id;
}

class TextOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const std::string text = this->get_input("Text").get_single_value_default<std::string>();
    const std::string font_path = this->get_input("Font").get_single_value_default<std::string>();
    const float size_factor = this->get_input("Size").get_single_value_default<float>();
    const float2 position = this->get_input("Position").get_single_value_default<float2>();
    const float rotation = this->get_input("Rotation").get_single_value_default<float>();
    const Alignment alignment = Alignment(
        this->get_input("Alignment").get_single_value_default<MenuValue>().value);
    const float4 color = float4(this->get_input("Color").get_single_value_default<Color>());
    const float4 background = float4(
        this->get_input("Background").get_single_value_default<Color>());
    const int resolution = this->get_input("Resolution").get_single_value_default<MenuValue>().value;

    const Domain domain = texture_graph::generator_domain(this->context(), resolution);
    const int2 size = domain.data_size;

    /* Draw the text once into its own buffer, then place that on the image. */
    int2 picture_size(0);
    float2 anchor(0.0f);
    Array<float4> picture;
    this->draw_text(
        text, font_path, size_factor * float(size.y), alignment, picture, picture_size, anchor);

    Result &image = this->get_result("Image");
    Result &mask = this->get_result("Mask");
    if (image.should_compute()) {
      image.allocate_texture(domain);
    }
    if (mask.should_compute()) {
      mask.allocate_texture(domain);
    }

    const float2 position_pixels = position * float2(size);
    parallel_for(size, [&](const int2 texel) {
      const float coverage = picture_size.x == 0 ?
                                 0.0f :
                                 math::clamp(texture_graph::place_on_tile(picture.data(),
                                                                          picture_size,
                                                                          anchor,
                                                                          position_pixels,
                                                                          rotation,
                                                                          texel,
                                                                          size)
                                                 .w,
                                             0.0f,
                                             1.0f);
      if (image.should_compute()) {
        image.store_pixel(texel, Color(math::interpolate(background, color, coverage)));
      }
      if (mask.should_compute()) {
        mask.store_pixel(texel, coverage);
      }
    });
  }

  /* Draws white text on a clear buffer. The anchor is the point of the buffer that lands on the
   * text position: the middle height of the text, on the side picked by the alignment. */
  void draw_text(const std::string &text,
                 const std::string &font_path,
                 const float font_size,
                 const Alignment alignment,
                 Array<float4> &r_picture,
                 int2 &r_size,
                 float2 &r_anchor)
  {
    if (text.empty() || font_size < 1.0f) {
      return;
    }
    const int font_id = load_font(font_path);
    if (font_id == -1) {
      return;
    }

    Vector<std::string> lines;
    size_t start = 0;
    while (true) {
      const size_t end = text.find('\n', start);
      lines.append(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }

    BLF_size(font_id, font_size);
    const float line_height = font_size * 1.2f;
    const float descender = float(math::abs(BLF_descender(font_id)));

    Vector<float> widths;
    float max_width = 0.0f;
    for (const std::string &line : lines) {
      const float width = BLF_width(font_id, line.c_str(), line.size());
      widths.append(width);
      max_width = math::max(max_width, width);
    }

    /* Leave some room so glyphs that reach past their advance width are not cut. */
    const int padding = int(font_size * 0.25f) + 2;
    r_size = int2(int(max_width) + padding * 2, int(line_height * lines.size()) + padding * 2);
    r_size = math::min(r_size, int2(16384));
    r_picture = Array<float4>(int64_t(r_size.x) * r_size.y, float4(0.0f));

    BLF_buffer(font_id, reinterpret_cast<float *>(r_picture.data()), nullptr, r_size.x, r_size.y,
               nullptr);
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    BLF_buffer_col(font_id, white);
    for (const int i : lines.index_range()) {
      float x = float(padding);
      if (alignment == Alignment::Center) {
        x += (max_width - widths[i]) * 0.5f;
      }
      else if (alignment == Alignment::Right) {
        x += max_width - widths[i];
      }
      const float y = float(padding) + line_height * float(lines.size() - 1 - i) + descender;
      BLF_position(font_id, x, y, 0.0f);
      BLF_draw_buffer(font_id, lines[i].c_str(), lines[i].size());
    }
    BLF_buffer(font_id, nullptr, nullptr, 0, 0, nullptr);

    float anchor_x = float(padding) + max_width * 0.5f;
    if (alignment == Alignment::Left) {
      anchor_x = float(padding);
    }
    else if (alignment == Alignment::Right) {
      anchor_x = float(padding) + max_width;
    }
    r_anchor = float2(anchor_x, float(r_size.y) * 0.5f);
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new TextOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  tg_node_type_base(&ntype, "TextureGraphNodeText");
  ntype.ui_name = "Text";
  ntype.ui_description = "Draw text into an image, using any font file";
  ntype.enum_name_legacy = "TEXTURE_GRAPH_TEXT";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = node_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_texture_graph_text_cc
