// DESC: a callee's LOCAL parked in a caller-owned out-parameter by a WHOLE-OBJECT
// store. Such a store always dangles -- the destination outlives the call and the
// local does not -- and no liveness question is available, because the read is in the
// caller and nothing in the callee keeps the borrow live, so expiry never fires.
//
// The check for it was driven by a FieldStore and needed a MemberExpr to name the
// field, so only a store into a NAMED member was seen. `dst = Box::wrap(buf)` names
// none and was silent, in both the reference and pointer spellings, while
// `dst.p = buf` was reported.
//
// The destination is a [[gsl::Owner]] because that is a single level of indirection:
// a [[gsl::Pointer]] out-parameter is refused by multilevel-indirection instead, so
// the escape would never have to be caught on its own.
// EXPECT-ASAN: stack-use-after-return
volatile char g_sink;

struct [[gsl::Owner(char)]] Box {
  static Box wrap(char *s [[clang::lifetimebound]]) {
    Box b;
    b.p = s;
    return b;
  }
  void poke() const { g_sink = *p; }
  friend void fill(Box &dst [[clang::noescape]]);

private:
  char *p = nullptr;
};

void fill(Box &dst [[clang::noescape]]) {
  char buf[8] = "hello";
  dst = Box::wrap(buf); // `buf` dies when this call returns
}

int main() {
  Box b;
  fill(b);
  b.poke(); // reads a borrow of the returned-from frame
  return 0;
}
