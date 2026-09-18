// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety-soundness -verify %s
// Also verify the specific group enables it on its own.
// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety-lifetimebound-violation %s 2>&1 | FileCheck %s
// CHECK: conflicts with
// Off by default.
// RUN: %clang_cc1 -fsyntax-only -std=c++23 -verify=quiet %s

// quiet-no-diagnostics

// '[[clang::lifetimebound]]' and '[[clang::lifetimebound(pointee)]]' describe
// DIFFERENT relationships: one binds the result to the object, the other to what
// the object refers to. A redeclaration that changes the flavour therefore makes
// the same call mean two things depending on where it appears -- a caller before
// the redeclaration is checked against one contract and a caller after it against
// the other -- and nothing used to be said about the contradiction.

struct View {
  const char *m_p;
  // expected-note@+1 {{previous declaration is here}}
  const char *data() const [[clang::lifetimebound(pointee)]];
};

// The conflict also surfaces as an unverifiable promise: the body hands back the
// referent, which is not what plain 'lifetimebound' claims.
// expected-warning@+2 {{'[[clang::lifetimebound]]' here conflicts with '[[clang::lifetimebound(pointee)]]' on a previous declaration of this function}}
// expected-warning@+1 {{could not verify that the return value can be lifetime bound to the implicit this parameter}}
inline const char *View::data() const [[clang::lifetimebound]] { return m_p; }

// The other order is equally a conflict.
struct Other {
  const char *m_p;
  // The strict 'object' reading is the one that wins on a conflict, so it is the
  // one verified against the body -- and the body returns the referent.
  // expected-note@+2 {{previous declaration is here}}
  // expected-warning@+1 {{could not verify that the return value can be lifetime bound to the implicit this parameter}}
  const char *data() const [[clang::lifetimebound]];
};
// expected-warning@+1 {{'[[clang::lifetimebound(pointee)]]' here conflicts with '[[clang::lifetimebound]]' on a previous declaration of this function}}
inline const char *Other::data() const [[clang::lifetimebound(pointee)]] {
  return m_p;
}

//===----------------------------------------------------------------------===//
// Consistent declarations are silent: repeating the same flavour, and declaring
// the attribute on only one of the two declarations.
//===----------------------------------------------------------------------===//

struct Repeated {
  const char *m_p;
  const char *data() const [[clang::lifetimebound(pointee)]];
};
inline const char *Repeated::data() const [[clang::lifetimebound(pointee)]] {
  return m_p;
}

struct OnlyInClass {
  const char *m_p;
  const char *data() const [[clang::lifetimebound(pointee)]];
};
inline const char *OnlyInClass::data() const { return m_p; }

struct OnlyOnDefinition {
  const char *m_p;
  const char *data() const;
};
// expected-warning@+1 {{could not verify that the return value can be lifetime bound to the implicit this parameter}}
inline const char *OnlyOnDefinition::data() const [[clang::lifetimebound]] {
  return m_p;
}
