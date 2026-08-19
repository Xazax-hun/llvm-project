// DESC: the heap form of the static-data-member spelling gap: a borrow of a
// heap owner's buffer parked in a static data member reached as `obj.member`,
// then read after the owner is deleted. Same root cause as
// static_data_member_via_object.cpp -- the member spelling of a static data
// member did not resolve to the variable, so nothing recorded the store -- but
// this one is a use-after-free rather than a use-after-scope, and it needs no
// second function: the whole hazard is one straight-line main.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>

struct Cache {
  static const char *entry;
};
const char *Cache::entry = nullptr;

volatile char sink;

int main() {
  Cache c;
  std::string *s = new std::string("a long heap allocated string value ok!!");
  c.entry = s->c_str(); // borrow the heap buffer through the object spelling
  delete s;             // buffer freed
  sink = c.entry[0];    // heap-use-after-free
  return 0;
}
