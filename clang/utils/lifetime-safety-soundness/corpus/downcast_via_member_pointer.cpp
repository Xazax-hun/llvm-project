// DESC: a base-to-derived conversion laundered through a POINTER TO MEMBER. Bases are
// destroyed after the derived part, so a base destructor that reads derived state is a
// use-after-free -- which is why base-to-derived conversions are refused outright.
//
// For a pointer to member the hazardous direction is REVERSED. Converting `Derived::*` to
// `Base::*` is what lets a Base object reach a member only the derived class has (`b->*p`);
// `Base::* -> Derived::*` is the implicit, safe widening, since a member of Base is a member
// of every Derived. The refusal peeled references and pointers to find the two classes, and a
// member-pointer type has no pointee CLASS to peel -- peeling yields the pointed-to type
// (`std::string`), not the class -- so both sides came back null and the conversion was never
// examined.
//
// It is also the only laundering channel that produces no laundered pointer VALUE. `memcpy`,
// unions, `uintptr_t` and `bit_cast` are all backstopped by -Wlifetime-safety-lost-loan or
// -Wlifetime-safety-type-punning, but `this->*f` looks like an ordinary tracked member access,
// so the borrow is attributed to the Base object while it actually reaches Derived storage.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct Base {
  virtual ~Base();
};
struct Derived : Base {
  std::string s{"a-derived-member-string-long-enough-to-heap-allocate-for-sure"};
};

// By the time this runs, ~Derived has already freed `s`'s buffer.
Base::~Base() {
  std::string Base::*f = static_cast<std::string Base::*>(&Derived::s);
  sink = (this->*f)[0]; // heap-use-after-free
}

int main() {
  Derived d;
  (void)&d;
  return 0;
}
