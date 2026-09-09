// DESC: a borrow captured into a heap object, then converted to a BASE pointer whose
// pointee type holds no origins. The conversion drops the level the borrow lived on,
// so it cannot be represented through the base pointer.
//
// That alone was a refusal (the destination origin stayed empty, and the read of it
// drew the empty-origin sentinel). Then carrying the object's own loan through the
// upcast -- needed so a base-member access still names the object -- filled the
// destination and MASKED that sentinel, turning a refusal into silence. The exact
// same code with `PrintTask *` instead of `Task *` is reported precisely, because
// nothing is dropped there.
// EXPECT-ASAN: stack-use-after-scope
#include <string>

volatile char g_sink;

struct Task {
  virtual ~Task() = default;
  virtual void run() const = 0;
};

// Holds a borrow; `Task` cannot.
struct [[gsl::Pointer]] PrintTask : Task {
  const std::string *m;
  PrintTask(const std::string &s [[clang::lifetimebound]]) : m(&s) {}
  void run() const override { g_sink = (*m)[0]; }
};

int main() {
  Task *t = nullptr;
  {
    std::string s(50, 'x');
    t = new PrintTask(s); // the borrow of `s` does not survive the conversion
  }
  t->run(); // reads a borrow of the dead string
  return 0;
}
