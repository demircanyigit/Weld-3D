/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup geo
 *
 * A convex cell made by cutting a box with planes, shared by Voronoi Fracture and Rock.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "BLI_array.hh"
#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

namespace blender::geometry::fracture {

struct CellFace {
  Vector<double3, 8> verts;
  /* Index in #CellBuilder::normals. */
  int plane;
};

/**
 * A convex polyhedron that is made smaller by cutting off the part in front of a plane.
 */
class CellBuilder {
 public:
  Vector<CellFace, 16> faces;
  Vector<double3, 16> normals;

  CellBuilder(const double3 &min, const double3 &max)
  {
    const auto corner = [&](const int x, const int y, const int z) {
      return double3(x ? max.x : min.x, y ? max.y : min.y, z ? max.z : min.z);
    };
    this->add_box_face({-1, 0, 0}, {corner(0, 0, 0), corner(0, 0, 1), corner(0, 1, 1), corner(0, 1, 0)});
    this->add_box_face({1, 0, 0}, {corner(1, 0, 0), corner(1, 1, 0), corner(1, 1, 1), corner(1, 0, 1)});
    this->add_box_face({0, -1, 0}, {corner(0, 0, 0), corner(1, 0, 0), corner(1, 0, 1), corner(0, 0, 1)});
    this->add_box_face({0, 1, 0}, {corner(0, 1, 0), corner(0, 1, 1), corner(1, 1, 1), corner(1, 1, 0)});
    this->add_box_face({0, 0, -1}, {corner(0, 0, 0), corner(0, 1, 0), corner(1, 1, 0), corner(1, 0, 0)});
    this->add_box_face({0, 0, 1}, {corner(0, 0, 1), corner(1, 0, 1), corner(1, 1, 1), corner(0, 1, 1)});
  }

  /** Keep the part where `dot(normal, position) <= offset`. Returns false if nothing changed. */
  bool clip(const double3 &normal, const double offset, const double epsilon)
  {
    Vector<double3, 16> cap;
    Vector<CellFace, 16> new_faces;
    bool changed = false;
    for (const CellFace &face : faces) {
      Vector<double3, 8> kept;
      bool face_changed = false;
      for (const int i : face.verts.index_range()) {
        const double3 &a = face.verts[i];
        const double3 &b = face.verts[(i + 1) % face.verts.size()];
        const double dist_a = math::dot(normal, a) - offset;
        const double dist_b = math::dot(normal, b) - offset;
        const bool inside_a = dist_a <= epsilon;
        const bool inside_b = dist_b <= epsilon;
        if (inside_a) {
          kept.append(a);
        }
        else {
          face_changed = true;
        }
        if (inside_a != inside_b) {
          const double factor = std::clamp(dist_a / (dist_a - dist_b), 0.0, 1.0);
          const double3 crossing = a + (b - a) * factor;
          kept.append(crossing);
          cap.append(crossing);
        }
      }
      changed |= face_changed;
      if (kept.size() >= 3) {
        new_faces.append({std::move(kept), face.plane});
      }
    }
    if (!changed) {
      return false;
    }
    faces = std::move(new_faces);
    this->add_cap(cap, normal, epsilon);
    return true;
  }

  double max_distance(const double3 &position) const
  {
    double result = 0.0;
    for (const CellFace &face : faces) {
      for (const double3 &vert : face.verts) {
        result = std::max(result, math::distance_squared(vert, position));
      }
    }
    return std::sqrt(result);
  }

 private:
  void add_box_face(const double3 &normal, const std::array<double3, 4> &verts)
  {
    CellFace face;
    face.verts.extend(verts.data(), int64_t(verts.size()));
    face.plane = normals.append_and_get_index(normal);
    faces.append(std::move(face));
  }

  /** Close the hole left by a cut. The crossing points are only known in a random order. */
  void add_cap(Span<double3> points, const double3 &normal, const double epsilon)
  {
    if (points.size() < 3) {
      return;
    }
    double3 center(0.0);
    for (const double3 &point : points) {
      center += point;
    }
    center /= double(points.size());

    /* Any direction in the plane works, so use the one that is the farthest from the normal. */
    const double3 helper = std::abs(normal.x) < 0.5 ? double3(1, 0, 0) : double3(0, 1, 0);
    const double3 axis_u = math::normalize(math::cross(normal, helper));
    const double3 axis_v = math::cross(normal, axis_u);

    Array<std::pair<double, double3>> sorted(points.size());
    for (const int i : points.index_range()) {
      const double3 offset = points[i] - center;
      sorted[i] = {std::atan2(math::dot(offset, axis_v), math::dot(offset, axis_u)), points[i]};
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
      return a.first < b.first;
    });

    CellFace face;
    for (const auto &[angle, point] : sorted) {
      if (face.verts.is_empty() || math::distance(face.verts.last(), point) > epsilon) {
        face.verts.append(point);
      }
    }
    if (face.verts.size() > 1 && math::distance(face.verts.last(), face.verts.first()) <= epsilon)
    {
      face.verts.pop_last();
    }
    if (face.verts.size() >= 3) {
      face.plane = normals.append_and_get_index(normal);
      faces.append(std::move(face));
    }
  }
};

}  // namespace blender::geometry::fracture
