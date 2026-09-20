// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety-soundness -verify %s

// '[[clang::lifetimebound]]' on the implicit object says the result may refer to
// the OBJECT. For a view that is wrong: the result refers to storage the view
// does not own, so it survives the view. Saying it anyway makes every by-value
// view parameter look like it hands out a borrow of a dying local --
// '[[clang::lifetimebound(pointee)]]' is how a view says what it means.

volatile char sink;

//===----------------------------------------------------------------------===//
// The reported false positive: a by-value view parameter whose accessor is
// pointee-bound, stored into a member with the capture declared.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] View {
  const char *m_p;
  unsigned m_len;
  const char *data() const [[clang::lifetimebound(pointee)]] { return m_p; }
};

struct Token {
  const char *m_chars;
  unsigned m_len;

  void capture(View v [[clang::lifetime_capture_by(this)]]) {
    m_len = v.m_len;
    // `v` dies with the call, but `v.data()` does not point into `v`.
    m_chars = v.data();
  }
};

//===----------------------------------------------------------------------===//
// The borrow is TRACKED, not dropped: the referent is what gets reported, and it
// is named instead of the view.
//===----------------------------------------------------------------------===//

struct Holder {
  const char *m_chars; // expected-note {{this field dangles}}
};

// expected-warning@+1 {{parameter that can hold a borrow is not annotated}}
void referent_dangles(Holder &out) {
  char local[8] = "hi";
  // The borrow is reported against the REFERENT, `local` -- not against `v`.
  // expected-warning@+1 {{stack memory associated with local variable 'local' escapes to the field 'm_chars' which will dangle}}
  View v{local, 2};
  out.m_chars = v.data();
}

// Same shape, but the referent outlives everything: nothing to report.
static const char g_buf[] = "hi";
// expected-warning@+1 {{parameter that can hold a borrow is not annotated}}
void referent_is_static(Holder &out) {
  View v{g_buf, 2};
  out.m_chars = v.data();
}

//===----------------------------------------------------------------------===//
// The promise is verified against the body, in both directions.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] Verified {
  const char *m_p;
  char m_inline[4];

  // Honest: hands back the referent.
  const char *honest() const [[clang::lifetimebound(pointee)]] { return m_p; }

  // A lie: `m_inline` is the object's OWN storage, so the result dies with the
  // object -- exactly what `pointee` promises it does not.
  // expected-warning@+1 {{could not verify that the return value can be lifetime bound to what the implicit this parameter refers to}}
  const char *lying() const [[clang::lifetimebound(pointee)]] { return m_inline; }

  // Plain 'lifetimebound' claims the result refers to the object, which handing
  // back the referent does not do. Unchanged behaviour, and the reason the
  // `pointee` spelling has to exist.
  // expected-warning@+1 {{could not verify that the return value can be lifetime bound to the implicit this parameter}}
  const char *over_constrained() const [[clang::lifetimebound]] { return m_p; }
};

// An owner's accessor really does hand out its own storage, so plain
// 'lifetimebound' is right there and stays verified.
struct [[gsl::Owner(char)]] Buf {
  char m_buf[8];
  const char *data() const [[clang::lifetimebound]] { return m_buf; }
};

//===----------------------------------------------------------------------===//
// The advice names the form the body calls for, rather than listing both.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] Advice {
  const char *m_p;
  char m_inline[4];
  bool m_flag;

  // expected-warning@+1 {{mark the implicit object 'lifetimebound(pointee)', since the result refers to what the object refers to and not to the object}}
  const char *returns_referent() const { return m_p; }

  // expected-warning@+2 {{mark the implicit object 'lifetimebound'}}
  // expected-warning@+1 {{implicit this in intra-TU function should be marked}}
  const char *returns_own_storage() const { return m_inline; } // expected-note {{param returned here}}

  // Both relationships occur, so neither annotation alone describes it. The
  // fix-it in -Wlifetime-safety-suggestions still offers the conservative
  // 'lifetimebound' here, which is the safe half of the ambiguity.
  // expected-warning@+2 {{mark the implicit object 'lifetimebound' (if the result refers to the object) or 'lifetimebound(pointee)' (if it refers to what the object refers to)}}
  // expected-warning@+1 {{implicit this in intra-TU function should be marked}}
  const char *returns_either() const { return m_flag ? m_p : m_inline; } // expected-note {{param returned here}}
};

//===----------------------------------------------------------------------===//
// A type with no modelled referent. `pointee` says the result refers to what the
// object refers TO, and here there is no origin to refer to -- the record has no
// tracked members, so its origin node has no pointee child. Asking for the
// referent's origin asserted (and would have dereferenced null in a release
// build).
//
// The borrow is REFUSED rather than dropped, and rather than falling back to
// binding the result to the OBJECT: that fallback would assert exactly the
// relationship this annotation denies, reviving the false positive above for
// every by-value parameter of such a type.
//===----------------------------------------------------------------------===//

class NoMembers {
  const char16_t *span16() const [[clang::lifetimebound(pointee)]];
  // expected-warning@+2 {{lifetime safety cannot track this value here}}
  // expected-warning@+1 {{member function returning 'const char16_t *' is not annotated for lifetime safety}}
  const char16_t *unsafeSpan16() const { return span16(); }
};

//===----------------------------------------------------------------------===//
// Verification is a MAY property, refutation a MUST one.
//
// One referent-bound return path used to put the method in the "verified" set,
// and nothing could take it out again -- so a second path returning the object's
// OWN storage was licensed too, while the call site dropped the object binding for
// both. That is an ASan-confirmed stack-use-after-scope with no diagnostic.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] Hybrid {
  const char *m_ref;
  char m_own[8];

  // One path hands back the referent, the other the object's own storage. The
  // second refutes the promise, whichever the walk reaches first.
  // expected-warning@+1 {{could not verify that the return value can be lifetime bound to what the implicit this parameter refers to}}
  const char *both(bool own) const [[clang::lifetimebound(pointee)]] {
    return own ? m_own : m_ref;
  }

  // Same, spelled as a branch rather than a conditional operator.
  // expected-warning@+1 {{could not verify that the return value can be lifetime bound to what the implicit this parameter refers to}}
  const char *branchy(bool own) const [[clang::lifetimebound(pointee)]] {
    if (own)
      return m_own;
    return m_ref;
  }

  // Unconditionally honest: still accepted.
  const char *honest_only() const [[clang::lifetimebound(pointee)]] {
    return m_ref;
  }
};

//===----------------------------------------------------------------------===//
// A virtual override must not change the flavour.
//
// Needs no lying annotation: each body agrees with its own annotation, yet a
// virtual call is checked against the BASE's flavour, so an override binding to
// the object has that binding dropped while dispatch really does return the
// object's storage.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] Base {
  const char *m_ref;
  virtual ~Base() = default;
  // expected-note@+1 {{overridden virtual function is here}}
  virtual const char *data() const [[clang::lifetimebound(pointee)]] {
    return m_ref;
  }
};

struct [[gsl::Pointer(char)]] Derived : Base {
  char m_own[8];
  // expected-warning@+1 {{this overriding member function binds its return value to the object ('[[clang::lifetimebound]]'), but the overridden method binds it to what the object refers to}}
  const char *data() const [[clang::lifetimebound]] override { return m_own; }
};

// The reverse direction only over-approximates at the call site -- callers assume
// the stricter object binding -- so it is deliberately NOT flagged.
struct [[gsl::Pointer(char)]] Base2 {
  const char *m_ref;
  char m_own[8];
  virtual ~Base2() = default;
  virtual const char *data() const [[clang::lifetimebound]] { return m_own; }
};
struct [[gsl::Pointer(char)]] Derived2 : Base2 {
  const char *data() const [[clang::lifetimebound(pointee)]] override {
    return m_ref;
  }
};
