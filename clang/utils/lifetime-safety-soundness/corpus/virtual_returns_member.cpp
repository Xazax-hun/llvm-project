// DESC: a virtual method returns a borrow of a member; the object then dies
// EXPECT-ASAN: stack-use-after-scope
struct Base {
  virtual int *get() = 0;
  virtual ~Base() = default;
};
struct Derived : Base {
  int m = 7;
  int *get() override { return &m; }
};
__attribute__((noinline)) int *via(Base &b) { return b.get(); }
int main() {
  int *p;
  {
    Derived d;
    p = via(d);
  }
  return *p;
}
