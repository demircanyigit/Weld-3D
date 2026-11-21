/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_string_ref.hh"

#include "RNA_access.hh"

#include "WM_types.hh"

#include "UI_interface_layout.hh"
#include "interface_intern.hh"

namespace blender::ui {

struct IconViewMenuArgs {
  PointerRNA ptr;
  PropertyRNA *prop;
  bool show_labels;
  float icon_scale;
};

/* ID Search browse menu, open */
static Block *ui_icon_view_menu_cb(bContext *C, ARegion *region, void *arg_litem)
{
  static IconViewMenuArgs args;

  /* arg_litem is malloced, can be freed by parent button */
  args = *(static_cast<IconViewMenuArgs *>(arg_litem));
  const int w = UI_UNIT_X * (args.icon_scale);
  const int h = UI_UNIT_X * (args.icon_scale + args.show_labels);

  Block *block = block_begin(C, region, "_popup", EmbossType::Pulldown);
  block_flag_enable(block, BLOCK_LOOP);
  block_theme_style_set(block, BLOCK_THEME_STYLE_POPUP);

  bool free;
  const EnumPropertyItem *item;
  RNA_property_enum_items(C, &args.ptr, args.prop, &item, nullptr, &free);

  for (int a = 0; item[a].identifier; a++) {
    const int x = (a % 8) * w;
    const int y = -(a / 8) * h;

    const int icon = item[a].icon;
    const int value = item[a].value;
    Button *but;
    if (args.show_labels) {
      but = uiDefIconTextButR_prop(block,
                                   ButtonType::Row,
                                   icon,
                                   item[a].name,
                                   x,
                                   y,
                                   w,
                                   h,
                                   &args.ptr,
                                   args.prop,
                                   -1,
                                   0,
                                   value,
                                   std::nullopt);
    }
    else {
      but = uiDefIconButR_prop(block,
                               ButtonType::Row,
                               icon,
                               x,
                               y,
                               w,
                               h,
                               &args.ptr,
                               args.prop,
                               -1,
                               0,
                               value,
                               std::nullopt);
    }
    def_but_icon(but, icon, UI_HAS_ICON | BUT_ICON_PREVIEW);
  }

  block_bounds_set_normal(block, 0.3f * U.widget_unit);
  block_direction_set(block, UI_DIR_DOWN);

  if (free) {
    MEM_delete(item);
  }

  return block;
}

void template_icon(Layout *layout, int icon_value, float icon_scale)
{
  Block *block = layout->absolute().block();
  Button *but = uiDefIconBut(block,
                             ButtonType::Label,
                             ICON_X,
                             0,
                             0,
                             UI_UNIT_X * icon_scale,
                             UI_UNIT_Y * icon_scale,
                             nullptr,
                             0.0,
                             0.0,
                             "");
  def_but_icon(but, icon_value, UI_HAS_ICON | BUT_ICON_PREVIEW);
}

static Button *node_tile_button(
    Block *block, int icon, StringRef text, float width, std::optional<StringRef> tip)
{
  const int tile_width = int(UI_UNIT_X * width);
  /* Room for a 32 pixel icon, two lines of name below it and a little space around both. */
  const int tile_height = int(ICON_DEFAULT_HEIGHT_TOOLBAR * UI_SCALE_FAC + UI_UNIT_Y * 2.2f);
  Button *but = uiDefIconTextButO(block,
                                  ButtonType::PreviewTile,
                                  "NODE_OT_add_library_node",
                                  wm::OpCallContext::InvokeDefault,
                                  icon,
                                  text,
                                  0,
                                  0,
                                  tile_width,
                                  tile_height,
                                  tip);
  but->drawflag |= BUT_ICON_NATIVE_SIZE;
  return but;
}

void template_node_tile(Layout *layout,
                        StringRefNull node_type,
                        StringRef text,
                        int icon,
                        StringRef tooltip,
                        float width,
                        const bool as_row)
{
  /* A row stretches with the layout, a tile keeps its size. */
  Block *block = as_row ? layout->block() : layout->absolute().block();
  const std::optional<StringRef> tip = tooltip.is_empty() ? std::nullopt :
                                                            std::optional<StringRef>(tooltip);
  Button *but;
  if (as_row) {
    /* List view: one row with the small icon and the name. */
    const EmbossType emboss = block->emboss;
    block_emboss_set(block, EmbossType::Pulldown);
    but = uiDefIconTextButO(block,
                            ButtonType::But,
                            "NODE_OT_add_library_node",
                            wm::OpCallContext::InvokeDefault,
                            icon,
                            text,
                            0,
                            0,
                            int(UI_UNIT_X * width),
                            UI_UNIT_Y,
                            tip);
    block_emboss_set(block, emboss);
    but->drawflag |= BUT_TEXT_LEFT | BUT_ICON_LEFT;
  }
  else {
    but = node_tile_button(block, icon, text, width, tip);
  }
  PointerRNA *ptr = button_operator_ptr_ensure(but);
  RNA_string_set(ptr, "type", node_type.c_str());

  /* Dragging onto the graph adds the node where it's dropped. */
  but->dragtype = WM_DRAG_STRING;
  but->dragpoin = MEM_new<std::string>(__func__, "node_type:" + std::string(node_type));
  but->dragflag |= BUT_DRAGPOIN_FREE | BUT_DRAG_FULL_BUT;
}

void template_icon_view(Layout *layout,
                        PointerRNA *ptr,
                        const StringRefNull propname,
                        bool show_labels,
                        float icon_scale,
                        float icon_scale_popup)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());

  if (!prop || RNA_property_type(prop) != PROP_ENUM) {
    RNA_warning("property of type Enum not found: %s.%s",
                RNA_struct_identifier(ptr->type),
                propname.c_str());
    return;
  }

  Block *block = layout->absolute().block();

  int tot_items;
  bool free_items;
  const EnumPropertyItem *items;
  RNA_property_enum_items(
      static_cast<bContext *>(block->evil_C), ptr, prop, &items, &tot_items, &free_items);
  const int value = RNA_property_enum_get(ptr, prop);
  int icon = ICON_NONE;
  RNA_enum_icon_from_value(items, value, &icon);

  Button *but;
  if (RNA_property_editable(ptr, prop)) {
    IconViewMenuArgs *cb_args = MEM_new<IconViewMenuArgs>(__func__);
    cb_args->ptr = *ptr;
    cb_args->prop = prop;
    cb_args->show_labels = show_labels;
    cb_args->icon_scale = icon_scale_popup;

    but = uiDefBlockButN(block,
                         ui_icon_view_menu_cb,
                         cb_args,
                         "",
                         0,
                         0,
                         UI_UNIT_X * icon_scale,
                         UI_UNIT_Y * icon_scale,
                         "",
                         but_func_argN_free<IconViewMenuArgs>,
                         but_func_argN_copy<IconViewMenuArgs>);
  }
  else {
    but = uiDefIconBut(block,
                       ButtonType::Label,
                       ICON_X,
                       0,
                       0,
                       UI_UNIT_X * icon_scale,
                       UI_UNIT_Y * icon_scale,
                       nullptr,
                       0.0,
                       0.0,
                       "");
  }

  def_but_icon(but, icon, UI_HAS_ICON | BUT_ICON_PREVIEW);

  if (free_items) {
    MEM_delete(items);
  }
}

}  // namespace blender::ui
