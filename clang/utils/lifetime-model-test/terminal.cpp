// terminal.cpp - see terminal.h.
//
// All syscall interaction is confined to small `#pragma clang diagnostic
// ignored "-Wlifetime-safety-soundness"` regions: tcgetattr/tcsetattr/read/
// ioctl/write/nanosleep take raw pointer parameters that carry no lifetime
// annotations. Input *parsing* deliberately stays inside the model -- only the
// raw read() that fills the byte buffer is opted out.
#include "terminal.h"

#include <cstdint>

// System headers included OUTSIDE the safe-model region so their declarations
// produce no diagnostics.
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// Opt INTO the safe model for this file's own code; nested `ignored` regions
// below carve out the individual syscalls.
#pragma clang diagnostic push
#pragma clang diagnostic error "-Wlifetime-safety-soundness"

namespace ast {
namespace {

// There is a single terminal; stash its original mode here so the system struct
// never has to appear as a member inside a safe-model region.
termios g_origTermios;

// Emit a NUL-terminated control string to stdout. Opt-out: write() is a raw
// syscall with pointer parameters.
void emit(const char *s [[clang::noescape]]) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-soundness"
  std::size_t n = 0;
  while (s[n] != '\0')
    ++n;
  ::write(STDOUT_FILENO, s, n);
#pragma clang diagnostic pop
}

} // namespace

Terminal::Terminal() {
  // Enter raw, non-blocking mode and query the window size.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-soundness"
  if (::tcgetattr(STDIN_FILENO, &g_origTermios) == 0) {
    termios raw = g_origTermios;
    raw.c_lflag = raw.c_lflag & ~(static_cast<tcflag_t>(ICANON | ECHO));
    raw.c_cc[VMIN] = 0;  // non-blocking: return immediately
    raw.c_cc[VTIME] = 0; // ...with whatever is available
    ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    raw_ = true;
  }
  winsize ws;
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    cols_ = static_cast<std::int32_t>(ws.ws_col);
    rows_ = static_cast<std::int32_t>(ws.ws_row) - 1; // leave a status line
  }
#pragma clang diagnostic pop
  if (cols_ < 20) cols_ = 80;
  if (rows_ < 10) rows_ = 40;
  emit("\x1b[2J");    // clear screen
  emit("\x1b[?25l");  // hide cursor
}

Terminal::~Terminal() {
  emit("\x1b[?25h"); // show cursor
  emit("\x1b[2J\x1b[H");
  if (raw_) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-soundness"
    ::tcsetattr(STDIN_FILENO, TCSANOW, &g_origTermios);
#pragma clang diagnostic pop
  }
}

PollResult Terminal::poll() {
  PollResult result;

  char buf[64];
  std::int32_t n = 0;
  {
    // The only opt-out in this function: read() fills the local byte buffer.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-soundness"
    ssize_t got = ::read(STDIN_FILENO, buf, sizeof(buf));
#pragma clang diagnostic pop
    if (got > 0)
      n = static_cast<std::int32_t>(got);
  }

  // Parsing stays fully inside the safe model -- buf is a local array.
  for (std::int32_t i = 0; i < n; ++i) {
    const char c = buf[i];
    if (c == 27) { // ESC: either an arrow-key sequence or a quit request
      if (i + 2 < n && buf[i + 1] == '[') {
        const char dir = buf[i + 2];
        if (dir == 'A') result.input.thrust = true; // up
        else if (dir == 'D') result.input.left = true;
        else if (dir == 'C') result.input.right = true;
        i += 2;
      } else {
        result.quit = true;
      }
      continue;
    }
    switch (c) {
    case 'w': case 'W': result.input.thrust = true; break;
    case 'a': case 'A': result.input.left = true; break;
    case 'd': case 'D': result.input.right = true; break;
    case ' ': result.input.fire = true; break;
    case 'q': case 'Q': result.quit = true; break;
    default: break;
    }
  }
  return result;
}

void sleepMs(std::int32_t ms) {
  if (ms <= 0)
    return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-soundness"
  timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
  ::nanosleep(&ts, nullptr);
#pragma clang diagnostic pop
}

} // namespace ast

#pragma clang diagnostic pop
