// rng.h - small, fast, self-contained PRNG value type.
//
// Avoids <random> entirely: <random>'s engines and distributions are large,
// and threading their (often reference-taking) APIs through the safe model adds
// noise that has nothing to do with the game logic. A xorshift128+ generator is
// a plain value type holding two u64 words -- no indirection, no annotations.
#ifndef LMT_RNG_H
#define LMT_RNG_H

#include <cstdint>

#include "annotations.h"
LIFETIME_SAFE_START

namespace ast {

struct Rng {
  std::uint64_t s0 = 0x9e3779b97f4a7c15ULL;
  std::uint64_t s1 = 0xbf58476d1ce4e5b9ULL;

  // Seed deterministically (so --bench is reproducible).
  void seed(std::uint64_t v) {
    // splitmix64 to spread a single seed across both state words.
    s0 = mix(v + 0x9e3779b97f4a7c15ULL);
    s1 = mix(v + 0x3c6ef372fe94f82aULL);
    if (s0 == 0 && s1 == 0) s0 = 1; // xorshift must not be all-zero
  }

  std::uint64_t next() {
    std::uint64_t x = s0;
    std::uint64_t const y = s1;
    s0 = y;
    x ^= x << 23;
    s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return s1 + y;
  }

  // Uniform float in [0, 1).
  float uniform() {
    return static_cast<float>(next() >> 40) * (1.0f / 16777216.0f);
  }

  // Uniform float in [lo, hi).
  float range(float lo, float hi) { return lo + (hi - lo) * uniform(); }

private:
  static std::uint64_t mix(std::uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
};

} // namespace ast

LIFETIME_SAFE_END

#endif // LMT_RNG_H
