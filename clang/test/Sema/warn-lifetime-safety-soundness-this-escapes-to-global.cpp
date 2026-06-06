// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety -verify=permissive %s

// A borrow of the implicit object ('this') or one of its fields must not escape
// to global or static storage from a method: the global outlives the call, but
// the object's lifetime is the caller's, so the stored borrow may dangle. This
// is part of the "safe programming model" soundness group; it is silent under
// the default -Wlifetime-safety (the 'permissive' RUN expects no diagnostics).

// permissive-no-diagnostics

struct S {
  int f;
  void leak_this();
  void leak_field();
};

S *g_obj;
int *g_int;

// The diagnostic is anchored at the method, since the escape is observed where
// the analysis reaches function exit.
void S::leak_this() { // expected-warning {{a borrow of the enclosing object or one of its members escapes to global or static storage 'g_obj'}}
  g_obj = this;
}

void S::leak_field() { // expected-warning {{a borrow of the enclosing object or one of its members escapes to global or static storage 'g_int'}}
  g_int = &f;
}

// Storing into a static local counts as global/static storage.
struct T {
  int f;
  void leak() { // expected-warning {{escapes to global or static storage 'cache'}}
    static T *cache;
    cache = this;
  }
};

// A method that does not let 'this'/a field escape is not flagged.
struct Clean {
  int f;
  int read() const { return f; } // no-warning
  void store(int v) { f = v; }   // no-warning
};

int main() {
  Clean c;
  return c.read();
}
