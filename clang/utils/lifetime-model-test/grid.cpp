// grid.cpp - see grid.h.
#include "grid.h"

#include <cstdint>
#include <span>

#include "annotations.h"
LIFETIME_SAFE_START

namespace ast {

void Grid::build(std::int32_t cols, std::int32_t rows, float cellSize,
                 const SlotMap<Asteroid> &pool [[clang::noescape]]) {
  cols_ = cols < 1 ? 1 : cols;
  rows_ = rows < 1 ? 1 : rows;
  cellSize_ = cellSize > 0.0f ? cellSize : 1.0f;

  const std::int32_t numCells = cols_ * rows_;

  // Pass 1: count occupants per cell. cellStart_[c+1] accumulates the count for
  // cell c, so that after the prefix sum cellStart_[c] is c's start offset.
  cellStart_.assign(static_cast<std::size_t>(numCells) + 1, 0u);
  const std::uint32_t cap = pool.capacity();
  std::uint32_t live = 0;
  for (std::uint32_t i = 0; i < cap; ++i) {
    if (!pool.aliveAt(i))
      continue;
    const Asteroid &a = pool.at(i);
    const std::int32_t c = cellIndex(a.pos);
    ++cellStart_[static_cast<std::size_t>(c) + 1];
    ++live;
  }

  // Prefix sum -> start offsets.
  for (std::int32_t c = 0; c < numCells; ++c)
    cellStart_[static_cast<std::size_t>(c) + 1] +=
        cellStart_[static_cast<std::size_t>(c)];

  // Pass 2: scatter slot indices into their cell's slice using a write cursor.
  // NB: a whole-vector copy (`cursor_ = cellStart_`) is used rather than the
  // iterator-pair `assign(begin, end)` overload: passing STL iterators (which
  // are borrows) to libc++'s un-annotated iterator-range APIs trips
  // -Wlifetime-safety-unannotated-indirection. Value/index-based APIs stay in
  // the model cleanly. We simply ignore cursor_'s extra trailing element.
  cursor_ = cellStart_;
  entries_.resize(live);
  for (std::uint32_t i = 0; i < cap; ++i) {
    if (!pool.aliveAt(i))
      continue;
    const Asteroid &a = pool.at(i);
    const std::int32_t c = cellIndex(a.pos);
    entries_[cursor_[static_cast<std::size_t>(c)]++] = i;
  }
}

std::span<const std::uint32_t> Grid::cell(std::int32_t cx,
                                          std::int32_t cy) const {
  if (cx < 0 || cy < 0 || cx >= cols_ || cy >= rows_)
    return {};
  const std::size_t c = static_cast<std::size_t>(cy) * cols_ + cx;
  const std::uint32_t begin = cellStart_[c];
  const std::uint32_t end = cellStart_[c + 1];
  return std::span<const std::uint32_t>(entries_.data() + begin, end - begin);
}

} // namespace ast

LIFETIME_SAFE_END
