// vec2.h - 2D float vector value type and free math functions.
//
// A pure value type: it holds no borrow, so it needs no ownership annotation
// and passes through the safe programming model with zero friction. This is the
// baseline against which the more interesting types are measured.
#ifndef LMT_VEC2_H
#define LMT_VEC2_H

#include <cmath>

#include "annotations.h"
LIFETIME_SAFE_START

namespace ast {

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;
};

// All operators are by-value in and by-value out: no indirection at all.
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 v, float s) { return {v.x * s, v.y * s}; }
inline Vec2 operator*(float s, Vec2 v) { return {v.x * s, v.y * s}; }

inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float length2(Vec2 v) { return dot(v, v); }

inline float length(Vec2 v) {
  // std::sqrt is a by-value math function; no indirection, so it is usable
  // inside the safe region directly with no opt-out needed.
  return std::sqrt(length2(v));
}

inline Vec2 normalized(Vec2 v) {
  float len = length(v);
  if (len <= 1e-6f)
    return {0.0f, 0.0f};
  float inv = 1.0f / len;
  return {v.x * inv, v.y * inv};
}

// Unit vector pointing at `radians` (0 = +x, counter-clockwise).
inline Vec2 fromAngle(float radians) {
  return {std::cos(radians), std::sin(radians)};
}

// Wrap a position into the toroidal play field [0,w) x [0,h).
inline Vec2 wrap(Vec2 p, float w, float h) {
  if (p.x < 0.0f) p.x += w;
  else if (p.x >= w) p.x -= w;
  if (p.y < 0.0f) p.y += h;
  else if (p.y >= h) p.y -= h;
  return p;
}

// Toroidal displacement from `a` to `b`: the shortest vector across wraps.
inline Vec2 toroidalDelta(Vec2 a, Vec2 b, float w, float h) {
  float dx = b.x - a.x;
  float dy = b.y - a.y;
  if (dx > w * 0.5f) dx -= w;
  else if (dx < -w * 0.5f) dx += w;
  if (dy > h * 0.5f) dy -= h;
  else if (dy < -h * 0.5f) dy += h;
  return {dx, dy};
}

} // namespace ast

LIFETIME_SAFE_END

#endif // LMT_VEC2_H
