// DESC: a [[gsl::Owner]] container of pointers holds a borrow to a dead local
// EXPECT-ASAN: stack-use-after-scope
#include <vector>
int main() {
  std::vector<int *> v;
  {
    int x = 7;
    v.push_back(&x);
  }
  return *v[0];
}
