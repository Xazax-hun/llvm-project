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

void S::leak_field() {
  g_int = &f; // expected-warning {{a borrow of the enclosing object or one of its members escapes to global or static storage 'g_int'}}
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

// A [[clang::lifetimebound]] (or [[clang::lifetime_capture_by]]) parameter
// describes a return/capture relationship, not a global capture: if its borrow
// also escapes to a global, the annotation does not cover it and the caller is
// unaware.
int *g_p;
int *lifetimebound_to_global(int *a [[clang::lifetimebound]]) { // expected-warning {{parameter escapes to global or static storage 'g_p', which its lifetime annotation does not describe}}
  g_p = a;
  return a;
}

// lifetime_capture_by(global) is itself rejected: the analysis cannot track a
// borrow captured into global storage (see -Wlifetime-safety-global-capture).
void capture_by_global_banned(int *a [[clang::lifetime_capture_by(global)]]) { // expected-warning {{'lifetime_capture_by(global)' is not supported by the safe programming model}}
  g_p = a;
}

// lifetime_capture_by(unknown) is likewise rejected: the captured borrow goes to
// an unspecified location the analysis cannot track, so it may dangle undetected.
void capture_by_unknown_banned(int *a [[clang::lifetime_capture_by(unknown)]]) { // expected-warning {{'lifetime_capture_by(unknown)' is not supported by the safe programming model}}
  (void)a;
}

// A lifetimebound parameter that does not escape to a global is fine.
int *lifetimebound_no_escape(int *a [[clang::lifetimebound]]) {
  return a; // no-warning
}

int main() {
  Clean c;
  return c.read();
}
