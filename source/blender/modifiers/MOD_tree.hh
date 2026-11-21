/* SPDX-FileCopyrightText: 2026 Weld 3D Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender {

struct Object;
struct TreeModifierData;

/**
 * Make the modifier's values match the inputs of its Tree Graph: one value for every input,
 * keeping the values that were set before for inputs that are still there.
 */
void MOD_tree_update_interface(Object *object, TreeModifierData *tmd);

}  // namespace blender
