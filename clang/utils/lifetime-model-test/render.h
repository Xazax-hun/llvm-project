// render.h - ASCII framebuffer renderer.
//
// Rasterizes a World into a character grid and blits it to stdout in one write.
// The renderer only ever *borrows* the world (read-only), and only for the
// duration of draw() -> the parameter is [[clang::noescape]].
#ifndef LMT_RENDER_H
#define LMT_RENDER_H

#include <cstdint>
#include <string>
#include <vector>

#include "world.h"

#include "annotations.h"
LIFETIME_SAFE_START

namespace ast {

class Renderer {
public:
  Renderer(std::int32_t cols, std::int32_t rows);

  // Rasterize the current world state into the internal cell buffer.
  void draw(const World &world [[clang::noescape]]);

  // Write the current frame to stdout (cursor-home + cells), in a single
  // fwrite. Non-const because it fills the reused `frame_` scratch buffer: the
  // usual "mutable scratch member on a const method" trick is banned by the
  // safe model (no `mutable`), so the method advertises the mutation honestly.
  void present();

  std::int32_t cols() const { return cols_; }
  std::int32_t rows() const { return rows_; }

private:
  void clear();
  // Plot a character at world coordinate (wx, wy), mapped to the cell grid.
  void plot(float wx, float wy, char ch);

  std::vector<char> cells_; // cols_ * rows_ characters, row-major
  std::string frame_;       // reused output scratch (cursor-home + rows)
  std::int32_t cols_ = 0;
  std::int32_t rows_ = 0;
  float worldW_ = 1.0f;
  float worldH_ = 1.0f;
};

} // namespace ast

LIFETIME_SAFE_END

#endif // LMT_RENDER_H
