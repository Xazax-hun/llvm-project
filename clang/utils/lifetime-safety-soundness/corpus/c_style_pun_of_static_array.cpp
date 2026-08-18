// DESC: reinterpreting a static byte array as an object, with a C-STYLE cast. Reinterpreting
// one type's bytes as another is refused, because it launders a borrow through an unrelated
// type and hides its provenance -- but the check tested for the `reinterpret_cast` KEYWORD.
// `(std::string *)p` and `reinterpret_cast<std::string *>(p)` produce the identical `BitCast`
// in the AST, so the C-style spelling walked straight through.
//
// That is not array-specific: the same C-style pun of a LOCAL buffer was equally silent. The
// array is what makes it weaponizable, because it is the one route to an untracked pointer
// that no other net covers -- reading a global `char *`, `std::string &` or `string_view` each
// trips -Wlifetime-safety-lost-loan, while a global array trips nothing.
//
// So the refusal is keyed on the cast's KIND rather than its spelling, which covers the
// keyword, a C-style cast and a functional cast alike, plus the distinct kind a reference
// reinterpretation uses. Only written casts count: an implicit BitCast is the front end's own
// bookkeeping, and converting to `void *` nests one inside the explicit cast.
//
// Note the destructor here is '[[clang::destruction_order_safe]]' and honours the letter of
// that promise -- it names only a trivially destructible global and calls only
// std::string::operator[]. The object it actually reads is reached entirely through the pun.
// EXPECT-ASAN: heap-use-after-free
#include <new>
#include <string>

volatile char sink;

struct Raw {
  alignas(std::string) char b[sizeof(std::string)];
};
static Raw a, c;

__attribute__((constructor)) static void init() {
  new (a.b) std::string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  c = a; // bitwise copy: c.b now aliases the same heap buffer
}

struct [[clang::destruction_order_safe]] Reader {
  ~Reader() { sink = (*(std::string *)c.b)[0]; } // heap-use-after-free
};
static Reader reader;

__attribute__((destructor)) static void freeIt() {
  *(std::string *)a.b = std::string("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
}

int main() { return 0; }
