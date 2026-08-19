// DESC: a store of a local's address into a STATIC data member reached as
// `obj.member` rather than `Class::member`. A static data member is a variable,
// not a subobject, and only the qualified spelling is a DeclRefExpr -- the
// member spelling built an origin disconnected from the variable, so the store
// landed on a throwaway expression origin and the global-escape fact keyed on
// the VarDecl was never emitted. Both spellings denote the same object and are
// now modelled alike.
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-return
struct R {
  static int *slot;
};
int *R::slot = nullptr;

volatile int sink;

void stash() {
  R r;
  int x = 7;
  r.slot = &x; // store the address of a local through the object spelling
}

int main() {
  R r;
  stash();
  sink = *r.slot; // read it after stash() returned
  return 0;
}
