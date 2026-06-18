// DESC: a [[gsl::Pointer]] view holds a [[gsl::Owner]] member (`Box`) that has a
// PUBLIC raw pointer field. Storing a stack borrow into that pointer
// (`v.box.p = &local`) landed on a transient member-access origin (the owner is
// opaque -- a leaf in the origin tree) and was dropped, while a sibling pointer
// member seeded by aggregate-init gave the view a construction loan that masked
// lost-loan: a silent use-after-scope. The real defect is the type design -- a
// gsl::Owner with a public borrow-holding member is not a sound owner -- so it
// is now flagged at the owner's definition (-Wlifetime-safety-owner-public-pointer).
// EXPECT-ASAN: stack-use-after-scope
#include <cstdio>
struct [[gsl::Owner]] Box { int *p = nullptr; };
struct [[gsl::Pointer]] View {
  int *anchor = nullptr; // construction loan -> masks lost-loan
  Box box;               // gsl::Owner member with a public raw pointer
};
int g = 0;
__attribute__((noinline)) int trigger() {
  View v{&g, {}};
  {
    volatile int local = 42;
    v.box.p = (int *)&local; // store stack borrow into the owner-member's pointer
  }
  volatile int filler[64];
  for (int i = 0; i < 64; i++)
    filler[i] = i;
  return *v.box.p; // use-after-scope
}
int main() {
  printf("%d\n", trigger());
  return 0;
}
