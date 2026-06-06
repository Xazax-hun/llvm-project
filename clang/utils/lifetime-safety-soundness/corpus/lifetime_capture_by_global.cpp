// DESC: [[clang::lifetime_capture_by(global)]] documents a global capture the
// analysis cannot actually track (Global captures are skipped), so a borrow
// captured into global storage dangles undetected
// EXPECT-ASAN: stack-use-after-scope
int *g;
void store(int *x [[clang::lifetime_capture_by(global)]]) { g = x; }

int main() {
  static int keep = 1;
  g = &keep; // valid loan so a lost-loan net cannot mask the bug
  {
    int a = 5;
    store(&a); // &a is captured into 'g'; the global capture is not tracked
  }
  return *g; // g points to the destroyed 'a'
}
