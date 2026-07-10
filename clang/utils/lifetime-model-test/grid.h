// grid.h - uniform spatial hash for broad-phase collision.
//
// The performance core of the simulation. Naive collision is O(bullets *
// asteroids); with thousands of each that dominates the frame. The grid buckets
// asteroids into cells so each query only scans a handful of candidates.
//
// Layout is a CSR / counting-sort, not a vector<vector>: two flat buffers
// (`cellStart_` offsets + `entries_` slot indices) that are *reused* every
// frame (assign/resize keep capacity), so steady-state rebuilds allocate
// nothing. Stored values are asteroid *slot indices* (uint32), never pointers,
// so the grid is a container of plain integers -- no "container of indirection".
#ifndef LMT_GRID_H
#define LMT_GRID_H

#include <cstdint>
#include <span>
#include <vector>

#include "entities.h"
#include "slotmap.h"

#include "annotations.h"
LIFETIME_SAFE_START

namespace ast {

struct Grid {
  // Rebuild the acceleration structure from the live asteroids. The pool is
  // borrowed for the duration of the call only -> [[clang::noescape]].
  void build(std::int32_t cols, std::int32_t rows, float cellSize,
             const SlotMap<Asteroid> &pool [[clang::noescape]]);

  std::int32_t cols() const { return cols_; }
  std::int32_t rows() const { return rows_; }

  // Clamp a world coordinate to a valid cell column/row.
  std::int32_t cellX(float x) const {
    std::int32_t c = static_cast<std::int32_t>(x / cellSize_);
    if (c < 0) c = 0;
    if (c >= cols_) c = cols_ - 1;
    return c;
  }
  std::int32_t cellY(float y) const {
    std::int32_t r = static_cast<std::int32_t>(y / cellSize_);
    if (r < 0) r = 0;
    if (r >= rows_) r = rows_ - 1;
    return r;
  }

  // The asteroid slot indices bucketed into cell (cx, cy). The returned span is
  // a view into `entries_`, valid only while this Grid is alive and unmodified
  // -> [[clang::lifetimebound]] on `this`. Returns empty for out-of-range cells.
  std::span<const std::uint32_t> cell(std::int32_t cx, std::int32_t cy) const
      [[clang::lifetimebound]];

private:
  // Flattened cell index for a world position (column-major: cy*cols + cx).
  std::int32_t cellIndex(Vec2 p) const {
    return cellY(p.y) * cols_ + cellX(p.x);
  }

  std::vector<std::uint32_t> cellStart_; // size cols_*rows_ + 1, prefix offsets
  std::vector<std::uint32_t> entries_;   // asteroid slot indices, grouped by cell
  std::vector<std::uint32_t> cursor_;    // scratch write-cursors during scatter
  std::int32_t cols_ = 0;
  std::int32_t rows_ = 0;
  float cellSize_ = 1.0f;
};

} // namespace ast

LIFETIME_SAFE_END

#endif // LMT_GRID_H
