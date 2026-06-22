// DESC: a [[gsl::Pointer]] view type whose destructor `delete`s its borrowed
// member is a contract lie -- a view owns nothing, so its destruction should
// free nothing. A caller-owned borrow is slipped into the view by AGGREGATE
// INITIALIZATION (`Wrap w{x}`, an InitListExpr -- no constructor parameter to
// flag), and ~Wrap frees it at scope end, leaving the caller's `x` a dangling
// alias. This threaded every backstop: gsl::Pointer (no unknown-ownership),
// aggregate-init (no capture/unannotated-indirection), a tracked `new` loan (no
// lost-loan), and the naked-delete destructor exemption (which assumed any dtor
// freeing a member is the normal owner pattern). Found by the multi-agent bypass
// hunt. Fixed by dropping the naked-delete destructor exemption for a
// [[gsl::Pointer]] view, so the unverifiable `delete p` in ~Wrap is flagged.
// EXPECT-ASAN: heap-use-after-free
struct Big {
  int a[64];
  int v;
};

struct [[gsl::Pointer(Big)]] Wrap {
  Big *p;
  ~Wrap() { delete p; } // a view's destructor must not deallocate
};

volatile int sink;
int main() {
  Big *x = new Big();
  x->v = 7;
  { Wrap w{x}; } // aggregate-init stores x into w; ~Wrap frees x at scope end
  sink = x->v;   // use-after-free: x still aliases the freed storage
  return sink;
}
