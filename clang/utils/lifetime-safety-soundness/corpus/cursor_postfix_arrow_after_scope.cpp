// DESC: the cursor idiom `(p++)->field` reading storage whose scope has ended. A
// member access registers no use of its own base -- `p->m` is address arithmetic
// that touches nothing until the resulting lvalue is loaded, which `&p->m` proves
// -- so the increment was the pointer's ONLY use. While pointer arithmetic was
// treated as "the pointer now designates a different element, and bounds are out
// of scope", that single use excused the whole expression and the read went
// unreported. Postfix makes it worse: `p++` evaluates to the pointer from BEFORE
// the increment, so it reads exactly the object that died.
//
// Two changes close it. Arithmetic no longer cancels anything -- believing the
// new element is valid is not something the analysis knows, and after a scope
// ends the storage `++p` lands in is equally gone. And the access is recorded
// where the object is actually touched: at the lvalue-to-rvalue conversion.
// EXPECT-ASAN: stack-use-after-scope
volatile int sink;

struct S {
  int id;
};

int main() {
  S *p;
  {
    S arr[4]{};
    arr[0].id = 7;
    p = arr; // borrow of storage that dies at the closing brace
  }
  sink = (p++)->id; // reads arr[0].id after its scope ended
  return 0;
}
