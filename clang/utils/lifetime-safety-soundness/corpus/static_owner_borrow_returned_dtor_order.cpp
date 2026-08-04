// DESC: the Meyers-singleton accessor hands out a borrow of a function-local
// static `const std::string`. Static storage *duration* is not immortality: the
// string's destructor frees its buffer at teardown, and a static object destroyed
// LATER (here g_logger, constructed before the lazy static) reads the freed buffer
// from its own destructor. The escape checks only covered a borrow stored into
// global/static storage, not one RETURNED to the caller, so this was clean --
// even though the caller may keep it, or may itself be running at teardown.
// This is the idiom recommended to fix the static *initialization* order fiasco,
// which reintroduces the *destruction* order one; `name()` has no parameters, so
// there is nothing the model would otherwise ask you to annotate.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

const char *name() {
  static const std::string s = "a string long enough to need a heap buffer";
  return s.data(); // borrow of a static owner handed to the caller
}

struct Logger {
  ~Logger() { sink = name()[0]; } // runs after `s` is destroyed
};

Logger g_logger; // constructed before `s` -> destroyed after it

int main() {
  sink = name()[0]; // forces `s` to be constructed AFTER g_logger
  return 0;
}
