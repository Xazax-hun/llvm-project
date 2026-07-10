// entities.h - plain-old-data game entities and shared constants.
//
// Every entity is a trivially-copyable value type holding only scalars and
// Vec2s. Crucially, *nothing here is a pointer to another entity*: an asteroid
// does not point at the ship, a bullet does not point at the asteroid it might
// hit. Cross-entity references, where they exist at all, are expressed as
// SlotHandle / array indices elsewhere. That is what lets these live in
// std::vector and SlotMap without tripping the "no container of indirections"
// rule, and it is also the cache-friendly layout.
#ifndef LMT_ENTITIES_H
#define LMT_ENTITIES_H

#include <cstdint>

#include "vec2.h"

#include "annotations.h"
LIFETIME_SAFE_START

namespace ast {

// World dimensions are in abstract simulation units; the renderer maps them to
// terminal cells. Keeping the sim resolution independent of the terminal size
// means physics is deterministic regardless of window size (important for the
// reproducible --bench mode).
struct Config {
  float width = 120.0f;
  float height = 80.0f;
  float shipThrust = 90.0f;   // units/s^2
  float shipTurn = 3.2f;      // radians/s
  float shipDrag = 0.6f;      // velocity damping per second
  float bulletSpeed = 70.0f;  // units/s
  float bulletLife = 1.6f;    // seconds
  float fireCooldown = 0.18f; // seconds between shots
  float respawnDelay = 1.5f;  // seconds dead before respawn
};

struct Ship {
  Vec2 pos;
  Vec2 vel;
  float angle = 0.0f;     // facing, radians
  float fireTimer = 0.0f; // counts down to next allowed shot
  float deadTimer = 0.0f; // >0 means destroyed, counting down to respawn
  std::int32_t lives = 3;

  bool alive() const { return deadTimer <= 0.0f; }
};

struct Asteroid {
  Vec2 pos;
  Vec2 vel;
  float radius = 0.0f;
  float spin = 0.0f;  // visual rotation rate, radians/s
  float phase = 0.0f; // current visual rotation
  std::int32_t size = 0; // 2 = large, 1 = medium, 0 = small (splits down to 0)
};

struct Bullet {
  Vec2 pos;
  Vec2 vel;
  float life = 0.0f; // remaining seconds; <=0 means expired
};

// Visual debris when something is destroyed; pure eye-candy, kept in a vector
// with swap-and-pop churn.
struct Particle {
  Vec2 pos;
  Vec2 vel;
  float life = 0.0f;
};

inline float asteroidRadiusForSize(std::int32_t size) {
  switch (size) {
  case 2: return 6.0f;
  case 1: return 3.5f;
  default: return 2.0f;
  }
}

} // namespace ast

LIFETIME_SAFE_END

#endif // LMT_ENTITIES_H
