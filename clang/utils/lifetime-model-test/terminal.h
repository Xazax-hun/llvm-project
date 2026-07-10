// terminal.h - raw-mode terminal session + non-blocking keyboard polling.
//
// This is the project's main "library boundary": termios/unistd syscalls take
// raw pointer parameters and are not lifetime-annotated, so the calls that touch
// them sit inside localized LIFETIME_UNSAFE_BEGIN/END opt-outs (see
// terminal.cpp). Everything the rest of the program sees is a clean, in-model
// value API: a Terminal RAII handle and a PollResult of value types.
#ifndef LMT_TERMINAL_H
#define LMT_TERMINAL_H

#include <cstdint>

#include "world.h" // for Input

#include "annotations.h"
LIFETIME_SAFE_START

namespace ast {

struct PollResult {
  Input input;     // keys seen this frame, folded into player intent
  bool quit = false;
};

// Puts the terminal into raw, non-blocking mode for the session's lifetime and
// restores the previous mode on destruction. Single-owner, non-copyable.
class Terminal {
public:
  Terminal();
  ~Terminal();
  Terminal(const Terminal &) = delete;
  Terminal &operator=(const Terminal &) = delete;

  // Drain pending input and fold it into a per-frame intent + quit flag.
  PollResult poll();

  std::int32_t cols() const { return cols_; }
  std::int32_t rows() const { return rows_; }

private:
  std::int32_t cols_ = 80;
  std::int32_t rows_ = 40;
  bool raw_ = false;
};

// Sleep for the given number of milliseconds (frame pacing). Lives here so the
// nanosleep opt-out is localized to this file.
void sleepMs(std::int32_t ms);

} // namespace ast

LIFETIME_SAFE_END

#endif // LMT_TERMINAL_H
