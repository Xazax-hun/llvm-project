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

//===----------------------------------------------------------------------===//
// The ban is a property of the DECLARATION, so a body is not required.
//
// It used to be checked from the per-function analysis, which needs one -- so
// the case that matters most went unsaid: a function declared in a header and
// defined in another translation unit. Every call site dropped the capture, and
// the annotation the caller was trusting bought nothing.
//===----------------------------------------------------------------------===//

// Declared only; defined in some other TU.
void captures_global_decl_only(int *a [[clang::lifetime_capture_by(global)]]); // expected-warning {{'lifetime_capture_by(global)' is not supported by the safe programming model}}
void captures_unknown_decl_only(int *a [[clang::lifetime_capture_by(unknown)]]); // expected-warning {{'lifetime_capture_by(unknown)' is not supported by the safe programming model}}

// Annotated on the declaration, defined here without repeating the attribute.
// The declaration is where the annotation is written, so that is where it is
// reported -- and reported once, not once per redeclaration.
void captures_global_split(int *a [[clang::lifetime_capture_by(global)]]); // expected-warning {{'lifetime_capture_by(global)' is not supported by the safe programming model}}
// The definition repeats the signature without the attribute, so ITS parameter
// really is unannotated -- the annotation does not carry across to it.
void captures_global_split(int *a) { g_p = a; } // expected-warning {{not annotated for lifetime safety}}

// A member function declared in the class and defined out of line.
struct SplitMember {
  void take(int *a [[clang::lifetime_capture_by(unknown)]]); // expected-warning {{'lifetime_capture_by(unknown)' is not supported by the safe programming model}}
};
void SplitMember::take(int *a) { (void)a; } // expected-warning {{not annotated for lifetime safety}}

// A function template is reported once, on the pattern -- not again per
// instantiation.
template <class T>
void captures_global_tmpl(int *a [[clang::lifetime_capture_by(global)]]) { g_p = a; } // expected-warning {{'lifetime_capture_by(global)' is not supported by the safe programming model}}
void instantiate_tmpl() {
  captures_global_tmpl<int>(nullptr);
  captures_global_tmpl<char>(nullptr);
}

// Capturing by a named parameter is supported and stays clean.
void captures_by_param(int *a [[clang::lifetime_capture_by(out)]], int *&out); // no-warning
