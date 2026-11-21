/* SPDX-FileCopyrightText: 2026 Weld 3D
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BKE_object.hh"

#include "BLI_bounds_types.hh"
#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Weld 3D: groups (empties marked with #OB_TRANSFLAG_GROUP) draw no axes. Instead, a box
 * is drawn around everything in the group while the group or one of its members is selected.
 * The box uses the group's selection ID, so clicking its lines selects the whole group.
 * It is drawn like the "Bounds" display of objects, with solid lines.
 *
 * Members can be in the regular or the in-front layer, so this lives once per instance (not per
 * layer) and collects the bounds of every member before drawing in #end_sync.
 */
class GroupBounds : Overlay {
 private:
  PassSimple ps_ = {"GroupBounds"};
  ShapeInstanceBuf<ExtraInstanceData> box_buf_;

  struct GroupInfo {
    /** Bounds of the members in the group's local space. */
    blender::Bounds<float3> bounds = {float3(FLT_MAX), float3(-FLT_MAX)};
    bool has_bounds = false;
    /** The group itself was synced (it is visible in this view). */
    bool synced = false;
    /** The group or one of its own members is selected. */
    bool shown = false;
    float4x4 object_to_world = float4x4::identity();
    float4 color = float4(1.0f);
    select::ID select_id = select::SelectMap::select_invalid_id();
  };
  Map<const Object *, GroupInfo> groups_;

 public:
  GroupBounds(const SelectionType selection_type) : box_buf_(selection_type, "group_bounds_buf")
  {
  }

  static bool is_group(const Object *ob)
  {
    return ob != nullptr && ob->type == OB_EMPTY && (ob->transflag & OB_TRANSFLAG_GROUP);
  }

  void begin_sync(Resources & /*res*/, const State &state) final
  {
    enabled_ = state.is_space_v3d() && state.show_extras();
    groups_.clear();
    box_buf_.clear();
  }

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final
  {
    if (!enabled_ || is_from_dupli_or_set(ob_ref)) {
      return;
    }
    const Object *ob = ob_ref.object;
    const bool selected = (ob->base_flag & BASE_SELECTED) != 0;

    if (is_group(ob)) {
      GroupInfo &info = groups_.lookup_or_add_default(ob);
      info.synced = true;
      info.object_to_world = ob->object_to_world();
      info.color = res.object_wire_color(ob_ref, state);
      info.select_id = res.select_id(ob_ref);
      info.shown |= selected;
    }

    /* Corners of this object in world space. Groups add nothing of their own. */
    Vector<float3, 8> corners;
    if (!is_group(ob)) {
      std::optional<blender::Bounds<float3>> local;
      if (ELEM(ob->type, OB_LATTICE, OB_ARMATURE)) {
        local = BKE_object_boundbox_get(ob);
      }
      else if (!ELEM(ob->type, OB_EMPTY, OB_LAMP, OB_CAMERA, OB_SPEAKER, OB_LIGHTPROBE)) {
        local = BKE_object_evaluated_geometry_bounds(ob);
      }
      if (local) {
        for (int i = 0; i < 8; i++) {
          const float3 corner((i & 1) ? local->max.x : local->min.x,
                              (i & 2) ? local->max.y : local->min.y,
                              (i & 4) ? local->max.z : local->min.z);
          corners.append(math::transform_point(ob->object_to_world(), corner));
        }
      }
      else {
        corners.append(ob->object_to_world().location());
      }
    }

    /* Add to every group above, so nested groups hold everything inside them. Only the nearest
     * group shows when a member is selected. */
    bool nearest = true;
    for (const Object *parent = ob->parent; parent; parent = parent->parent) {
      if (!is_group(parent)) {
        continue;
      }
      GroupInfo &info = groups_.lookup_or_add_default(parent);
      if (nearest && selected) {
        info.shown = true;
      }
      nearest = false;
      for (const float3 &corner : corners) {
        const float3 local = math::transform_point(parent->world_to_object(), corner);
        info.bounds.min = math::min(info.bounds.min, local);
        info.bounds.max = math::max(info.bounds.max, local);
        info.has_bounds = true;
      }
    }
  }

  void end_sync(Resources &res, const State &state) final
  {
    if (!enabled_) {
      return;
    }

    for (const GroupInfo &info : groups_.values()) {
      if (!info.synced || !info.shown || !info.has_bounds) {
        continue;
      }
      /* A little room around the members so the box does not sit on their edges. */
      const float3 size = info.bounds.max - info.bounds.min;
      /* The same margin on every side, from the largest side of the group. */
      const float3 pad = float3(std::max(math::reduce_max(size) * 0.05f, 0.02f));
      /* The cube shape spans -1..1, so scale by half the size. */
      float4x4 box = math::from_scale<float4x4>(size * 0.5f + pad);
      box.location() = math::midpoint(info.bounds.min, info.bounds.max);
      ExtraInstanceData data(info.object_to_world * box, info.color, 1.0f);
      box_buf_.append(data, info.select_id);
    }

    /* Same pass setup as #Bounds. */
    ps_.init();
    ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL,
                  state.clipping_plane_count);
    ps_.shader_set(res.shaders->extra_shape.get());
    ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
    res.select_bind(ps_);
    box_buf_.end_sync(ps_, res.shapes.cube.get());
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }
    GPU_framebuffer_bind(framebuffer);
    manager.submit(ps_, view);
  }
};

}  // namespace blender::draw::overlay
