// main.cpp - entry point, argument parsing, and the two run modes.
//
// `main` itself is intentionally OUTSIDE the safe-model region: its `char**
// argv` is, by definition, multi-level indirection (the C runtime boundary),
// and argument parsing talks to <cstdlib>/<cstdio>. main parses argv into plain
// value types and hands off to the in-model runners below. This is the one
// structurally unavoidable opt-out -- everything reachable from runGame /
// runBench is fully inside the model.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "render.h"
#include "terminal.h"
#include "world.h"

namespace ast {

struct BenchResult {
  std::int64_t frames = 0;
  std::int64_t asteroidSteps = 0; // sum of live-asteroid count per frame
  double seconds = 0.0;
};

// Monotonic clock in nanoseconds. Defined outside the model region (chrono's
// duration arithmetic is library glue we don't want to annotate); the in-model
// bench loop just consumes the int64 it returns.
std::int64_t nowNanos() {
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

BenchResult runBench(std::int32_t frames, std::int32_t targetAsteroids);
void runInteractive();

} // namespace ast

// ---------------------------------------------------------------------------
// In-model runners.
// ---------------------------------------------------------------------------
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wlifetime-safety-soundness"

namespace ast {

// Headless throughput benchmark: keep ~targetAsteroids alive, fly the ship
// around (no firing, so the population is stable), and measure how fast the
// movement + grid rebuild + broad-phase collision pipeline runs.
BenchResult runBench(std::int32_t frames, std::int32_t targetAsteroids) {
  Config cfg;
  // A roomier field than the interactive default, to exercise the grid.
  cfg.width = 480.0f;
  cfg.height = 320.0f;
  World world(cfg, 0xABCDEF01u);
  world.spawnWave(targetAsteroids);

  const float dt = 1.0f / 60.0f;
  BenchResult result;
  const std::int64_t t0 = nowNanos();
  for (std::int32_t f = 0; f < frames; ++f) {
    Input in;
    in.thrust = true;
    in.left = (f & 32) != 0; // weave around so the ship traverses the field
    in.right = (f & 32) == 0;
    world.step(in, dt);
    result.asteroidSteps += world.liveAsteroidCount();
    // Maintain population: respawn a batch when attrition (ship collisions)
    // thins the field.
    if (world.liveAsteroidCount() < targetAsteroids / 2)
      world.spawnWave(targetAsteroids / 4);
  }
  const std::int64_t t1 = nowNanos();
  result.frames = frames;
  result.seconds = static_cast<double>(t1 - t0) * 1e-9;
  return result;
}

void runInteractive() {
  Terminal term;
  Renderer renderer(term.cols(), term.rows());
  // Sim resolution is fixed (120x80) and independent of the terminal size; the
  // renderer scales it to the window. Keeps physics tuning stable.
  Config cfg;
  World world(cfg, 0x1234u);
  world.spawnWave(5);
  std::int32_t wave = 1;

  for (;;) {
    PollResult pr = term.poll();
    if (pr.quit)
      break;
    world.step(pr.input, 1.0f / 30.0f);
    if (world.liveAsteroidCount() == 0) {
      ++wave;
      world.spawnWave(4 + wave);
    }
    renderer.draw(world);
    renderer.present();
    sleepMs(33); // ~30 FPS
  }
}

} // namespace ast

#pragma clang diagnostic pop

// ---------------------------------------------------------------------------
// Entry point (outside the model: argv is char**).
// ---------------------------------------------------------------------------
namespace {
void printUsage() {
  std::printf("ASCII Asteroids - lifetime safe-model test\n"
              "  (no args)            play interactively (WASD/arrows + space, q to quit)\n"
              "  --bench N            headless: simulate N frames, report throughput\n"
              "  --asteroids M        target asteroid population for --bench (default 300)\n"
              "  --help               this message\n");
}
} // namespace

int main(int argc, char **argv) {
  std::int32_t benchFrames = 0;
  std::int32_t asteroids = 300;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--bench") == 0) {
      benchFrames = (i + 1 < argc) ? std::atoi(argv[++i]) : 2000;
    } else if (std::strcmp(argv[i], "--asteroids") == 0) {
      if (i + 1 < argc)
        asteroids = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--help") == 0) {
      printUsage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      printUsage();
      return 2;
    }
  }

  if (benchFrames < 0)
    benchFrames = 0;
  if (asteroids < 1)
    asteroids = 1;
  if (asteroids > static_cast<std::int32_t>(ast::World::kMaxAsteroids))
    asteroids = static_cast<std::int32_t>(ast::World::kMaxAsteroids);

  if (benchFrames > 0) {
    const ast::BenchResult r = ast::runBench(benchFrames, asteroids);
    const double fps = r.seconds > 0.0 ? r.frames / r.seconds : 0.0;
    const double msPerFrame = r.frames > 0 ? r.seconds * 1000.0 / r.frames : 0.0;
    const double astPerSec =
        r.seconds > 0.0 ? static_cast<double>(r.asteroidSteps) / r.seconds : 0.0;
    std::printf("frames           : %lld\n", static_cast<long long>(r.frames));
    std::printf("target asteroids : %d\n", asteroids);
    std::printf("wall time        : %.3f s\n", r.seconds);
    std::printf("frames / second  : %.1f\n", fps);
    std::printf("ms / frame       : %.4f\n", msPerFrame);
    std::printf("asteroid-updates : %lld (%.2f M/s)\n",
                static_cast<long long>(r.asteroidSteps), astPerSec * 1e-6);
    return 0;
  }

  ast::runInteractive();
  return 0;
}
