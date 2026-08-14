// DESC: a '[[clang::destruction_order_safe]]' virtual method whose override drops the
// promise. The callee check resolves against the STATICALLY named method, so dispatch
// through the base sees the base's annotation and is satisfied; the override is neither
// annotated nor verified, and its body runs unchecked at shutdown.
//
// Every sibling contract already enforced this -- 'lifetime_immortal',
// 'lifetime_non_invalidating', 'noescape' and 'lifetimebound' all reject an override
// that weakens what the base advertises -- so this was simply a missing entry in that
// set. Requiring the override to repeat the promise also routes its body through the
// existing verifier, which then reports the untruth directly.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

extern std::string g_str;

struct [[clang::destruction_order_safe]] Ticker {
  // Truthful: touches nothing outside itself.
  [[clang::destruction_order_safe]] virtual void tick() {}
  virtual ~Ticker() {}
};

struct Derived : Ticker {
  // Drops the promise; nothing required it to be repeated.
  void tick() override { sink = g_str[0]; }
};

struct [[clang::destruction_order_safe]] Runner {
  ~Runner() {
    Derived d;
    Ticker &t = d; // dispatch resolves statically to Ticker::tick, which promises
    t.tick();      // ...but Derived::tick runs, and reads the destroyed global
  }
};

Runner g_runner; // registered first -> destroyed LAST

std::string g_str = "0123456789012345678901234567890123456789012345678901234567890123456789";

int main() { return 0; }
