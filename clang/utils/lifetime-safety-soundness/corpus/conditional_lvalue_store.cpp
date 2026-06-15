// DESC: a store through a conditional-operator LVALUE (`(c ? p : q) = &local`)
// was dropped: handleAssignment routes only DeclRefExpr/MemberExpr/array-subscript
// LHS forms, so a conditional (or comma, `*&(...)`, etc.) LHS produced no
// OriginFlow. With p,q seeded a concrete long-lived loan (&g), the empty store
// was masked from lost-loan -> the dangling read was silent. Such an unroutable
// store into a borrow-holding destination is now conservatively rejected.
// EXPECT-ASAN: stack-use-after-scope
int g = 42;

volatile int sink;

__attribute__((noinline)) int test(bool c) {
  int *p = &g; // concrete long-lived loan masks lost-loan
  int *q = &g;
  {
    int local = 7;
    (c ? p : q) = &local; // conditional-lvalue store: dropped
  }                       // local dies
  // clobber the freed stack slot so a stale read is observable
  {
    volatile int junk[16];
    for (int i = 0; i < 16; ++i)
      junk[i] = 0xbeef;
    (void)junk;
  }
  return c ? *p : *q; // dangling read
}

int main() { return test(true); }
