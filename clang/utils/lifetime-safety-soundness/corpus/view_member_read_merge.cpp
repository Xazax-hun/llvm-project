// DESC: reading a borrow-holding member of a [[gsl::Pointer]] view (`out = r.p`)
// dropped the borrow the view held -- a gsl::Pointer is a leaf in the origin tree
// (members not field-expanded), so the member read landed on a transient origin
// disconnected from the view. Normally the empty `out` trips lost-loan, but a
// control-flow merge assigning a valid loan (`out = &g`) on the other path unions
// to a non-empty set and MASKS lost-loan -> silent stack-use-after-scope. The
// read now flows the view's held borrow (read-side dual of the view-member store
// merge), so the use after scope is caught precisely.
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-scope
struct [[gsl::Pointer]] Ref {
  const int *p;
};
int g = 100;
volatile int sink;
__attribute__((noinline)) int danger(bool cond) {
  const int *out;
  if (cond) {
    out = &g;
  } else {
    int local = 7;
    Ref r{&local};
    out = r.p; // reads the view's held borrow -> &local
  }            // local dies on this path
  return *out; // cond=false: use-after-scope of local
}
int main() { return danger(false) == 7 ? 0 : 1; }
