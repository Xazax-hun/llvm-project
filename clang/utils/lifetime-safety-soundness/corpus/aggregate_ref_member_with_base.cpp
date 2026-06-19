// DESC: aggregate initialization of a [[gsl::Pointer]] view that has a base
// subobject and a reference member. A reference member binds to its
// initializer's storage (a borrow of it), so it must use the unpeeled origin.
// But the reference-member special-case was gated on the record having no base
// subobjects (a base prepends an init with no matching field, which would
// misalign the field iterator), so a record WITH a base disabled it: the
// reference member's borrow was peeled away by getRValueOrigins and silently
// dropped. A sibling pointer member initialized from a global supplied a valid
// loan that populated the shared leaf-object origin, masking lost-loan -- so the
// dangling reference to a local escaped with no diagnostic. Found by the 60th
// multi-agent bypass hunt (C). Closed by skipping the leading base-subobject
// inits and zipping the rest to fields, so a reference field is recognized even
// with base subobjects present.
// EXPECT-ASAN: stack-use-after-return
int g_ok = 99;

struct Base {};
struct [[gsl::Pointer]] RefView : Base {
  const int *p; // valid (global) -- masks lost-loan
  const int &r; // dangling (local) -- borrow was silently dropped
};

__attribute__((noinline)) const int &leak() {
  int local = 0x5151;
  RefView v{Base{}, &g_ok, local}; // base subobject disabled the ref-member case
  return v.r;                      // dangling reference to local
}

__attribute__((noinline)) void clobber() {
  volatile int junk[64];
  for (int i = 0; i < 64; ++i)
    junk[i] = i * 3;
}

int main() {
  const int &d = leak();
  clobber();
  return d == 0x5151 ? 0 : 1; // use-after-return read
}
