// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A store into a member of `this` is recognized (and routed to the field's
// real, decl-anchored origin) only when the member-access base is `this`. But
// `this->m`, `(*this).m`, and `static_cast<Base*>(this)->m` all name the same
// field of the same object. Previously only the direct `this->m` form was
// recognized; the `*this` and base-cast spellings routed the borrow to a fresh,
// disconnected origin and dropped it -- so a dangling field bound through them
// was silent (a real use-after-scope). All spellings are now treated alike.

struct [[gsl::Owner]] Holder {
  const int *p = nullptr; // expected-note 3 {{this field dangles}}

  // (*this).p : deref of this.
  int via_deref() {
    {
      int local = 7;
      (*this).p = &local; // expected-warning {{escapes to the field 'p' which will dangle}}
    }
    return *p;
  }

  // (*&*this).p : a `*&` round-trip wrapping this.
  int via_deref_addrof() {
    {
      int local = 7;
      (*&*this).p = &local; // expected-warning {{escapes to the field 'p' which will dangle}}
    }
    return *p;
  }

  // Control: this->p (already recognized).
  int via_arrow() {
    {
      int local = 7;
      this->p = &local; // expected-warning {{escapes to the field 'p' which will dangle}}
    }
    return *p;
  }
};

// Base-class field stored through a derived-to-base cast of `this`.
struct [[gsl::Owner]] Base {
  const int *q = nullptr; // expected-note 2 {{this field dangles}}
};
struct [[gsl::Owner]] Derived : Base {
  // static_cast<Base*>(this)->q
  int via_static_cast() {
    {
      int local = 7;
      static_cast<Base *>(this)->q = &local; // expected-warning {{escapes to the field 'q' which will dangle}}
    }
    return *q;
  }
  // C-style ((Base*)this)->q
  int via_cstyle_cast() {
    {
      int local = 7;
      ((Base *)this)->q = &local; // expected-warning {{escapes to the field 'q' which will dangle}}
    }
    return *q;
  }
};
