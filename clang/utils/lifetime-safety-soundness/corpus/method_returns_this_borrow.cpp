// DESC: a member function returns a borrow of '*this'; the object then dies
// EXPECT-ASAN: stack-use-after-scope
struct C {
  int x;
  int *get() { return &x; } // implicit object should be lifetimebound
};
int main() {
  int *p;
  {
    C c{5};
    p = c.get();
  }
  return *p;
}
