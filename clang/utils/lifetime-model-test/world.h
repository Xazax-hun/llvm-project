// world.h - the simulation: state ownership + the per-frame update.
//
// World owns everything: the ship, the asteroid pool, the bullet/particle
// arrays, the RNG, and the broad-phase grid. The renderer and the input layer
// only *borrow* it (const for reading, non-const for input). All cross-entity
// references inside the update are slot indices / handles, never pointers.
#ifndef LMT_WORLD_H
#define LMT_WORLD_H

#include <cstdint>
#include <vector>

#include "entities.h"
#include "grid.h"
#include "rng.h"
#include "slotmap.h"
#include "vec2.h"

#include "annotations.h"
LIFETIME_SAFE_START

namespace ast {

// Per-frame player intent. A value type: no borrow, trivially copied.
struct Input {
  bool thrust = false;
  bool left = false;
  bool right = false;
  bool fire = false;
};

class World {
public:
  // Max simultaneous asteroids. Fixed so the pool allocates once; splits that
  // would overflow are simply dropped.
  static constexpr std::uint32_t kMaxAsteroids = 4096;

  World(Config config, std::uint64_t seed);

  // Advance the simulation by dt seconds under the given input.
  void step(Input in, float dt);

  // Spawn `count` large asteroids at random edge positions.
  void spawnWave(std::int32_t count);

  // ---- read-only views for the renderer (all bound to `this`) ----
  const Config &config() const [[clang::lifetimebound]] { return config_; }
  const Ship &ship() const [[clang::lifetimebound]] { return ship_; }
  const SlotMap<Asteroid> &asteroids() const [[clang::lifetimebound]] {
    return asteroids_;
  }
  const std::vector<Bullet> &bullets() const [[clang::lifetimebound]] {
    return bullets_;
  }
  const std::vector<Particle> &particles() const [[clang::lifetimebound]] {
    return particles_;
  }

  std::int64_t score() const { return score_; }
  std::int32_t lives() const { return ship_.lives; }
  std::int32_t liveAsteroidCount() const {
    return static_cast<std::int32_t>(asteroids_.size());
  }

private:
  void updateShip(Input in, float dt);
  void fireBullet();
  void updateBullets(float dt);
  void updateAsteroids(float dt);
  void updateParticles(float dt);
  void rebuildGrid();
  void collideBulletsAsteroids();
  void collideShipAsteroids();
  // Destroy the asteroid in slot `slot`: score it, spawn debris, and split it
  // into two smaller asteroids (unless it is already the smallest).
  void destroyAsteroid(std::uint32_t slot);
  void spawnDebris(Vec2 at, std::int32_t amount);
  void respawnShip();

  Vec2 wrapPos(Vec2 p) const {
    return wrap(p, config_.width, config_.height);
  }

  Config config_;
  Rng rng_;
  Ship ship_;
  SlotMap<Asteroid> asteroids_;
  std::vector<Bullet> bullets_;
  std::vector<Particle> particles_;
  Grid grid_;
  std::int64_t score_ = 0;
  float cellSize_ = 12.0f;
};

} // namespace ast

LIFETIME_SAFE_END

#endif // LMT_WORLD_H
