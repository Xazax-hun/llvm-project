// DESC: a heap use-after-free inside a catch block, in a `try` written in a constructor's
// MEM-INITIALIZER. Exception control flow is refused rather than modeled -- a handler
// resumes after the stack has unwound, an edge the CFG does not carry -- and NOTHING inside
// a try/catch is analyzed otherwise: suppressing that refusal makes even a plain
// use-after-free in a catch block disappear, so the refusal is the whole coverage there.
// It is an AST walk, and it was rooted at the function BODY alone; a constructor's
// mem-initializers are not part of its body, so this `try` was not reachable from the walk's
// root and went unrefused -- while the identical `try` one line further down, in the body,
// was refused. The pre-scan in Origins.cpp already seeded both the body and the
// initializers. Base-class initializers are the same shape.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile int sink;

struct Cfg {
  int a;
  Cfg() : a(({
      int r = 0;
      try { (void)std::string("").at(1); }
      catch (...) {
        std::string *p = new std::string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        std::string_view sv(*p);
        delete p;
        r = (int)sv[0];
      }
      r; })) {}
};

int main() { Cfg c; sink = c.a; return 0; }
