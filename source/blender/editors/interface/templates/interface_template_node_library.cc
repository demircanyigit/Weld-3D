/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * The categories of the node Library, drawn as a tree like the Outliner.
 */

#include <memory>
#include <string>

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"

namespace blender::ui {

namespace {

struct LibraryCategory {
  int depth = 0;
  std::string path;
  std::string name;
  int icon = ICON_NONE;
  bool active = false;
  bool open = false;
};

Vector<LibraryCategory> parse_categories(StringRef items)
{
  Vector<LibraryCategory> categories;
  while (!items.is_empty()) {
    const int64_t line_end = items.find('\n');
    StringRef line = items.substr(0, line_end);
    items = (line_end == StringRef::not_found) ? StringRef() : items.drop_prefix(line_end + 1);

    Vector<StringRef, 6> fields;
    while (true) {
      const int64_t tab = line.find('\t');
      fields.append(line.substr(0, tab));
      if (tab == StringRef::not_found) {
        break;
      }
      line = line.drop_prefix(tab + 1);
    }
    if (fields.size() < 4) {
      continue;
    }
    LibraryCategory category;
    category.depth = std::max(0, std::atoi(std::string(fields[0]).c_str()));
    category.path = fields[1];
    category.name = fields[2];
    category.icon = std::atoi(std::string(fields[3]).c_str());
    category.active = fields.size() > 4 && fields[4] == "1";
    category.open = fields.size() > 5 && fields[5] == "1";
    categories.append(std::move(category));
  }
  return categories;
}

class NodeLibraryTreeView : public AbstractTreeView {
  Vector<LibraryCategory> categories_;

 public:
  explicit NodeLibraryTreeView(Vector<LibraryCategory> categories)
      : categories_(std::move(categories))
  {
  }

  void build_tree() override
  {
    /* The last item added on each depth, to add children to. */
    Vector<TreeViewOrItem *> parents = {this};
    for (const LibraryCategory &category : categories_) {
      const int depth = std::min<int>(category.depth, parents.size() - 1);
      parents.resize(depth + 1);
      BasicTreeViewItem &item = parents.last()->add_tree_item<BasicTreeViewItem>(category.name,
                                                                                 category.icon);
      const std::string path = category.path;
      item.set_on_activate_fn([path](bContext &C, BasicTreeViewItem & /*item*/) {
        wmOperatorType *ot = WM_operatortype_find("NODE_OT_library_category", false);
        if (ot == nullptr) {
          return;
        }
        PointerRNA ptr = WM_operator_properties_create_ptr(ot);
        RNA_string_set(&ptr, "path", path.c_str());
        WM_operator_name_call_ptr(&C, ot, wm::OpCallContext::ExecDefault, &ptr, nullptr);
        WM_operator_properties_free(&ptr);
      });
      const bool active = category.active;
      item.set_is_active_fn([active]() { return active; });
      if (category.open) {
        item.uncollapse_by_default();
      }
      parents.append(&item);
    }
  }
};

}  // namespace

void template_node_library_tree(Layout *layout, const bContext *C, StringRef items)
{
  Block *block = layout->block();
  AbstractTreeView *tree_view = block_add_view(
      *block,
      "node library categories",
      std::make_unique<NodeLibraryTreeView>(parse_categories(items)));
  TreeViewBuilder::build_tree_view(*C, *tree_view, *layout, false);
}

}  // namespace blender::ui
