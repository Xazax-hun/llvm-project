// DESC: a narrow opt-out region silencing every LATER construction of the same type.
// Descending through an implicit constructor is guarded so that a class reaching itself
// cannot loop. That guard was a memo lasting the whole body, and it recorded the visit
// whether or not a diagnostic actually came out of it -- so a `#pragma clang diagnostic
// ignored` region around one construction poisoned it for the rest of the function.
//
// The same defect under-reported with no suppression at all: `W w1; W w2;` in one verified
// destructor examined only `w1`, because the second construction found the memo already
// populated. That is the cheaper way to see it; this file is the weaponized form, where the
// suppression is narrow, looks local, and silently disables the enforcement that follows it.
//
// The guard belongs per construction site: within a site it still stops a loop and stops a
// repeated member type being walked twice, but two sites are two locations and each gets its
// own answer.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

extern std::string g_name;

// Trivially destructible, so no type-level rule constrains it; the constructor is the hazard.
struct Peeker {
  char c;
  Peeker();
};

struct Wraps {
  Peeker p;
};

struct [[clang::destruction_order_safe]] Last {
  ~Last() {
// A deliberately narrow opt-out, of the kind a reader would believe is confined to the
// statement it brackets.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-destruction-order"
    Wraps ignored;
    sink = ignored.p.c;
#pragma clang diagnostic pop
    // Outside the region, and formerly silent: this is the use-after-free.
    Wraps enforced;
    sink = enforced.p.c;
  }
};

Last last; // constructed first -> destroyed LAST

std::string g_name = "a-global-string-long-enough-that-its-buffer-is-heap-allocated";

Peeker::Peeker() : c(g_name.data()[0]) {}

int main() { return 0; }
