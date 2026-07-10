// world.cpp - see world.h.
#include "world.h"

#include <cstdint>
#include <span>

#include "annotations.h"
LIFETIME_SAFE_START

namespace ast {

namespace {
constexpr float kTwoPi = 6.2831853f;
constexpr float kShipRadius = 2.0f;

// Clamp a velocity-damping factor into [0, 1].
float dragFactor(float drag, float dt) {
  float f = 1.0f - drag * dt;
  if (f < 0.0f)
    return 0.0f;
  return f;
}
} // namespace

World::World(Config config, std::uint64_t seed)
    : config_(config), asteroids_(kMaxAsteroids) {
  rng_.seed(seed);
  ship_.pos = {config_.width * 0.5f, config_.height * 0.5f};
  // Reserve so the per-frame hot path never reallocates these arrays.
  bullets_.reserve(256);
  particles_.reserve(1024);
}

void World::step(Input in, float dt) {
  updateShip(in, dt);
  updateBullets(dt);
  updateAsteroids(dt);
  rebuildGrid();
  collideBulletsAsteroids();
  collideShipAsteroids();
  updateParticles(dt);
}

void World::updateShip(Input in, float dt) {
  if (!ship_.alive()) {
    ship_.deadTimer -= dt;
    if (ship_.deadTimer <= 0.0f && ship_.lives > 0)
      respawnShip();
    return;
  }
  if (in.left)
    ship_.angle -= config_.shipTurn * dt;
  if (in.right)
    ship_.angle += config_.shipTurn * dt;
  if (in.thrust)
    ship_.vel = ship_.vel + fromAngle(ship_.angle) * (config_.shipThrust * dt);
  ship_.vel = ship_.vel * dragFactor(config_.shipDrag, dt);
  ship_.pos = wrapPos(ship_.pos + ship_.vel * dt);

  ship_.fireTimer -= dt;
  if (in.fire && ship_.fireTimer <= 0.0f)
    fireBullet();
}

void World::fireBullet() {
  Vec2 dir = fromAngle(ship_.angle);
  Bullet b;
  b.pos = ship_.pos + dir * (kShipRadius + 0.5f);
  b.vel = ship_.vel + dir * config_.bulletSpeed;
  b.life = config_.bulletLife;
  bullets_.push_back(b);
  ship_.fireTimer = config_.fireCooldown;
}

void World::updateBullets(float dt) {
  std::size_t i = 0;
  while (i < bullets_.size()) {
    Bullet &b = bullets_[i];
    b.life -= dt;
    if (b.life <= 0.0f) {
      // swap-and-pop; the moved-in element is processed on the next iteration.
      bullets_[i] = bullets_.back();
      bullets_.pop_back();
      continue;
    }
    b.pos = wrapPos(b.pos + b.vel * dt);
    ++i;
  }
}

void World::updateAsteroids(float dt) {
  const std::uint32_t cap = asteroids_.capacity();
  for (std::uint32_t i = 0; i < cap; ++i) {
    if (!asteroids_.aliveAt(i))
      continue;
    Asteroid &a = asteroids_.at(i);
    a.pos = wrapPos(a.pos + a.vel * dt);
    a.phase += a.spin * dt;
  }
}

void World::updateParticles(float dt) {
  std::size_t i = 0;
  while (i < particles_.size()) {
    Particle &p = particles_[i];
    p.life -= dt;
    if (p.life <= 0.0f) {
      particles_[i] = particles_.back();
      particles_.pop_back();
      continue;
    }
    p.pos = wrapPos(p.pos + p.vel * dt);
    ++i;
  }
}

void World::rebuildGrid() {
  std::int32_t cols = static_cast<std::int32_t>(config_.width / cellSize_) + 1;
  std::int32_t rows = static_cast<std::int32_t>(config_.height / cellSize_) + 1;
  grid_.build(cols, rows, cellSize_, asteroids_);
}

void World::collideBulletsAsteroids() {
  const float w = config_.width;
  const float h = config_.height;
  for (std::size_t bi = 0; bi < bullets_.size(); ++bi) {
    // Read-only scan, then mutate. We copy the bullet position out by value and
    // hold no borrow into bullets_ across the mutation: the safe model treats a
    // non-const member call (destroyAsteroid) as invalidating views into *any*
    // of the object's owner fields, so a `Bullet&` kept across it -- even into a
    // different field than the one mutated -- is rejected.
    if (bullets_[bi].life <= 0.0f)
      continue;
    const Vec2 bpos = bullets_[bi].pos;
    const std::int32_t cx = grid_.cellX(bpos.x);
    const std::int32_t cy = grid_.cellY(bpos.y);
    bool found = false;
    std::uint32_t hitSlot = 0;
    for (std::int32_t dy = -1; dy <= 1 && !found; ++dy) {
      for (std::int32_t dx = -1; dx <= 1 && !found; ++dx) {
        // `entries` is a view into the grid (bound to grid_). The scan is
        // read-only, so access the asteroid pool through a const reference: the
        // const `aliveAt`/`at` overloads are not assumed to mutate. A non-const
        // member call (the mutable `at`) is conservatively assumed to mutate
        // `this`, which would invalidate this grid view -- a borrow into a
        // sibling member of the same object.
        const auto &asteroids = asteroids_;
        std::span<const std::uint32_t> entries = grid_.cell(cx + dx, cy + dy);
        for (std::uint32_t slot : entries) {
          if (!asteroids.aliveAt(slot))
            continue;
          const Asteroid &a = asteroids.at(slot);
          Vec2 d = toroidalDelta(bpos, a.pos, w, h);
          if (length2(d) <= a.radius * a.radius) {
            hitSlot = slot;
            found = true;
            break;
          }
        }
      }
    }
    if (found) {
      // Mutate: re-access bullets_ fresh; no borrow into an owner is live here.
      bullets_[bi].life = 0.0f;
      destroyAsteroid(hitSlot);
    }
  }
}

void World::collideShipAsteroids() {
  if (!ship_.alive())
    return;
  const float w = config_.width;
  const float h = config_.height;
  const std::int32_t cx = grid_.cellX(ship_.pos.x);
  const std::int32_t cy = grid_.cellY(ship_.pos.y);
  bool hit = false;
  for (std::int32_t dy = -1; dy <= 1 && !hit; ++dy) {
    for (std::int32_t dx = -1; dx <= 1 && !hit; ++dx) {
      // Read-only scan: const-access the asteroid pool (see
      // collideBulletsAsteroids) so the const `aliveAt`/`at` overloads are
      // selected and the grid view stays valid across the scan.
      const auto &asteroids = asteroids_;
      std::span<const std::uint32_t> entries = grid_.cell(cx + dx, cy + dy);
      for (std::uint32_t slot : entries) {
        if (!asteroids.aliveAt(slot))
          continue;
        const Asteroid &a = asteroids.at(slot);
        Vec2 d = toroidalDelta(ship_.pos, a.pos, w, h);
        const float rr = a.radius + kShipRadius;
        if (length2(d) <= rr * rr) {
          spawnDebris(ship_.pos, 14);
          ship_.deadTimer = config_.respawnDelay;
          ship_.lives -= 1;
          destroyAsteroid(slot); // invalidates `a`; not used afterwards
          hit = true;
          break;
        }
      }
    }
  }
}

void World::destroyAsteroid(std::uint32_t slot) {
  // Copy the asteroid out by value before erasing: nothing borrows the pool
  // across the mutation.
  Asteroid a = asteroids_.at(slot);
  asteroids_.eraseAt(slot);

  score_ += a.size == 2 ? 20 : (a.size == 1 ? 50 : 100);
  spawnDebris(a.pos, 6);

  if (a.size > 0) {
    const std::int32_t childSize = a.size - 1;
    const float childRadius = asteroidRadiusForSize(childSize);
    for (std::int32_t k = 0; k < 2; ++k) {
      Asteroid c;
      c.size = childSize;
      c.radius = childRadius;
      const float ang = rng_.range(0.0f, kTwoPi);
      const float spd = rng_.range(18.0f, 40.0f);
      c.pos = a.pos;
      c.vel = a.vel + fromAngle(ang) * spd;
      c.spin = rng_.range(-2.5f, 2.5f);
      c.phase = rng_.uniform() * kTwoPi;
      asteroids_.insert(c); // dropped if the pool is full
    }
  }
}

void World::spawnDebris(Vec2 at, std::int32_t amount) {
  for (std::int32_t i = 0; i < amount; ++i) {
    Particle p;
    p.pos = at;
    const float ang = rng_.range(0.0f, kTwoPi);
    const float spd = rng_.range(12.0f, 60.0f);
    p.vel = fromAngle(ang) * spd;
    p.life = rng_.range(0.3f, 0.9f);
    particles_.push_back(p);
  }
}

void World::respawnShip() {
  ship_.pos = {config_.width * 0.5f, config_.height * 0.5f};
  ship_.vel = {0.0f, 0.0f};
  ship_.angle = 0.0f;
  ship_.deadTimer = 0.0f;
  ship_.fireTimer = 0.0f;
}

void World::spawnWave(std::int32_t count) {
  for (std::int32_t i = 0; i < count; ++i) {
    Asteroid a;
    a.size = 2;
    a.radius = asteroidRadiusForSize(2);
    // Spawn along a random edge so asteroids drift inward, never on the ship.
    if (rng_.uniform() < 0.5f) {
      a.pos = {rng_.range(0.0f, config_.width),
               rng_.uniform() < 0.5f ? 0.0f : config_.height};
    } else {
      a.pos = {rng_.uniform() < 0.5f ? 0.0f : config_.width,
               rng_.range(0.0f, config_.height)};
    }
    const float ang = rng_.range(0.0f, kTwoPi);
    const float spd = rng_.range(10.0f, 26.0f);
    a.vel = fromAngle(ang) * spd;
    a.spin = rng_.range(-1.5f, 1.5f);
    a.phase = rng_.uniform() * kTwoPi;
    asteroids_.insert(a);
  }
}

} // namespace ast

LIFETIME_SAFE_END
