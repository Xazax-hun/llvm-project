// DESC: a std::string_view of a `const std::string` global is cached into a
// longer-lived global object. The const global cannot be mutated (no aliasing
// hazard), but its non-trivial destructor still frees its heap buffer at static
// destruction; because the caching global outlives it, that global's own
// destructor reads the freed buffer. Destruction order across globals (and TUs)
// is unspecified, so the analysis flags any borrow of a global/static owner with
// a non-trivial destructor that escapes to global storage. The immortal-storage
// reasoning used to treat static storage *duration* as immortal and missed this.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct Cache {
  std::string_view value;
  ~Cache() { sink = value[0]; } // reads the freed buffer at teardown
};

// Cache is constructed FIRST, so it is destroyed LAST -- after g_config.
Cache g_cache;
const std::string g_config = "a long const global string that owns a heap buffer";

struct Populate {
  Populate() { g_cache.value = g_config; } // borrow of a const global escapes to a global
};
Populate g_populate;

int main() { return 0; }
