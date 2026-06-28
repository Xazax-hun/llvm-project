// DESC: a [[gsl::Pointer]] member bound by a default member initializer (NSDMI)
// to a heap-owning temporary, consumed by an *implicit* default constructor and
// read in the destructor. C++ does not lifetime-extend a temporary bound in an
// NSDMI, so the std::string returned by make() dies at the end of construction
// and the view member dangles immediately. Implicitly-defined / defaulted
// constructors never reached the per-function analysis path, so the NSDMI
// temporary binding was not visited and the dangling field went unflagged.
// Running lifetime safety on the synthesized constructor body closes this.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char g_sink = 0;
void sink(char c) { g_sink = c; }              // by-value: not a borrow
std::string make() { return std::string(64, 'Z'); } // heap (defeats SSO)

struct [[gsl::Pointer]] Guard {
  std::string_view v = make(); // NSDMI temporary dies at end of construction
  ~Guard() { sink(v[0]); }     // dtor reads the freed buffer
};

int main() {
  Guard g; // implicit default constructor applies the dangling NSDMI
  return 0;
}
