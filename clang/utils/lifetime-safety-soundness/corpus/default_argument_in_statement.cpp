// DESC: a hazard inside a default argument on a function declared within a STATEMENT. Default
// arguments are analyzed by the sweep over declarations that no other entry point reaches --
// the CFG deliberately does not descend into a CXXDefaultArgExpr, since the expression belongs
// to the callee's declaration and adding it to each caller's CFG would make one Expr appear in
// several. That sweep skipped statements as a cost optimization, which is sound for its other
// job (a statement cannot contain a file-scope variable) but made every function nested in a
// statement unreachable once default-argument analysis was layered on top of it.
//
// The control pins it to placement alone: the identical default argument on a namespace-scope
// function is reported. Affected shapes were a lambda parameter, a local class's member
// function and its constructor, and a block-scope redeclaration that introduces a default
// argument for a function that is itself at file scope.
//
// A lambda needed more than walking statements: its call operator is never reached as a
// declaration, because the traversal visits the body and parameters through the expression and
// never calls TraverseDecl on the operator.
// EXPECT-ASAN: heap-use-after-free

volatile int sink;

int *g_p = nullptr;

void hold(int v);

int main() {
  auto note = [](int x = (g_p = new int(7), delete g_p, hold(*g_p), 0)) { return x; };
  return note();
}

void hold(int v) { sink = v; }
