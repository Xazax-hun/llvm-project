// DESC: a borrow of another static-duration object CAPTURED by a static-duration
// initializer. The destruction-order verifier asks what names a destructor body
// mentions, so storing a reference to the victim launders its identity: `~Reader` looks
// like it only touches `this`, and both types can honestly carry
// '[[clang::destruction_order_safe]]'. That made the annotation satisfiable while false,
// which is worse than not having it.
//
// The capture itself is the reportable event, and the analysis already had a rule for it
// (-Wlifetime-safety-view-on-mutable-global, which fires for the same wiring done inside
// a function). Two things stopped it here. The initializer was skipped as a constant
// initializer -- binding a reference to a global IS constant-evaluable, and the comment
// justifying the skip claimed such an initializer "cannot create a dangling borrow",
// which is exactly backwards: binding a reference is how one is created. And once
// analyzed, the borrow came to rest on the initializer expression's origin with no
// escape fact, so nothing anchored the check.
//
// Fixing the second half also closed a pre-existing FIXME in
// warn-lifetime-analysis-nocfg.cpp asking for CFG-based detection of global
// initialisation.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct [[clang::destruction_order_safe]] Victim {
  std::string s;
  char read() const { return s[0]; }
};

extern Victim v;

struct [[clang::destruction_order_safe]] Reader {
  Victim &m; // captures the victim at static-init time
  ~Reader() { sink = m.read(); }
};

Reader r{v};                                                   // destroyed LAST
Victim v{"a long heap allocated victim string value here ok"}; // destroyed FIRST

int main() { return 0; }
