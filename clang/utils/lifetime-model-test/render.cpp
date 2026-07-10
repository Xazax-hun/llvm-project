// render.cpp - see render.h.
#include "render.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "entities.h"
#include "vec2.h"

#include "annotations.h"
LIFETIME_SAFE_START

namespace ast {

namespace {
constexpr float kTwoPi = 6.2831853f;

// 8-way heading glyph (world y points down, matching terminal rows).
char shipGlyph(float angle) {
  float a = angle;
  while (a < 0.0f)
    a += kTwoPi;
  while (a >= kTwoPi)
    a -= kTwoPi;
  const std::int32_t oct =
      static_cast<std::int32_t>(a / (kTwoPi / 8.0f) + 0.5f) & 7;
  switch (oct) {
  case 0: return '>';
  case 1: return '\\';
  case 2: return 'v';
  case 3: return '/';
  case 4: return '<';
  case 5: return '\\';
  case 6: return '^';
  default: return '/';
  }
}
} // namespace

Renderer::Renderer(std::int32_t cols, std::int32_t rows)
    : cells_(static_cast<std::size_t>(cols) * rows, ' '), cols_(cols),
      rows_(rows) {
  frame_.reserve(static_cast<std::size_t>(cols + 1) * rows + 8);
}

void Renderer::clear() {
  for (std::size_t i = 0; i < cells_.size(); ++i)
    cells_[i] = ' ';
}

void Renderer::plot(float wx, float wy, char ch) {
  const std::int32_t cx =
      static_cast<std::int32_t>(wx / worldW_ * static_cast<float>(cols_));
  const std::int32_t cy =
      static_cast<std::int32_t>(wy / worldH_ * static_cast<float>(rows_));
  if (cx < 0 || cy < 0 || cx >= cols_ || cy >= rows_)
    return;
  cells_[static_cast<std::size_t>(cy) * cols_ + cx] = ch;
}

void Renderer::draw(const World &world [[clang::noescape]]) {
  const Config &cfg = world.config();
  worldW_ = cfg.width;
  worldH_ = cfg.height;
  clear();

  // Asteroids: ring outline so size reads at a glance.
  const SlotMap<Asteroid> &rocks = world.asteroids();
  for (std::uint32_t i = 0; i < rocks.capacity(); ++i) {
    if (!rocks.aliveAt(i))
      continue;
    const Asteroid &a = rocks.at(i);
    const char ch = a.size == 2 ? '#' : (a.size == 1 ? 'o' : '.');
    if (a.size == 0) {
      plot(a.pos.x, a.pos.y, ch);
      continue;
    }
    const std::int32_t seg = 12;
    for (std::int32_t k = 0; k < seg; ++k) {
      const float ang = a.phase + static_cast<float>(k) / seg * kTwoPi;
      const Vec2 p = a.pos + fromAngle(ang) * a.radius;
      plot(p.x, p.y, ch);
    }
  }

  // Bullets (range-for over a std::vector is fine; only views are a problem).
  for (const Bullet &b : world.bullets())
    plot(b.pos.x, b.pos.y, '*');

  // Debris.
  for (const Particle &p : world.particles())
    plot(p.pos.x, p.pos.y, '.');

  // Ship.
  const Ship &s = world.ship();
  if (s.alive())
    plot(s.pos.x, s.pos.y, shipGlyph(s.angle));

  // HUD baked into the top row. Plain std::string concatenation: the safe model
  // now recognizes std::string's `operator+=` / `operator+` as copy-in (the
  // operand does not escape), so this idiomatic code stays fully in-model.
  std::string hud = "Score ";
  hud += std::to_string(world.score());
  hud += "   Lives ";
  hud += std::to_string(world.lives());
  hud += "   Rocks ";
  hud += std::to_string(world.liveAsteroidCount());
  if (world.lives() <= 0)
    hud += "   -- GAME OVER (press q) --";
  for (std::size_t k = 0; k < hud.size() && k < static_cast<std::size_t>(cols_);
       ++k)
    cells_[k] = hud[k];
}

void Renderer::present() {
  frame_.clear();
  // ANSI: move cursor to home (top-left) without clearing, to avoid flicker.
  frame_.push_back('\x1b');
  frame_.push_back('[');
  frame_.push_back('H');
  for (std::int32_t y = 0; y < rows_; ++y) {
    const std::size_t base = static_cast<std::size_t>(y) * cols_;
    for (std::int32_t x = 0; x < cols_; ++x)
      frame_.push_back(cells_[base + x]);
    frame_.push_back('\n');
  }

  // --- library boundary: hand the assembled bytes to stdio. fwrite takes
  // unannotated pointer parameters, so this single call opts out. ---
LIFETIME_UNSAFE_BEGIN
  std::fwrite(frame_.data(), 1, frame_.size(), stdout);
  std::fflush(stdout);
LIFETIME_UNSAFE_END
}

} // namespace ast

LIFETIME_SAFE_END
