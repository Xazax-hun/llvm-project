// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

// On a CONSTRUCTOR, [[clang::lifetimebound]] describes the constructed object: it
// declares that the object may refer to the argument. On a [[gsl::Owner]] that is a
// borrow resting in an owner -- the same statement `lifetime_capture_by` naming an
// owner is already refused for, differently spelled.
//
// It is also worse than saying nothing, because the annotation SILENCES the demand
// to annotate the parameter. An owner that is handed a pointer and then frees it in
// its destructor left the caller's own pointer dangling with nothing reported, while
// the same constructor with no annotation at all was refused.
//
// The suggestion machinery had to change with it: for an unannotated owner
// constructor whose parameter escapes to a field it recommended exactly this
// annotation, so banning it while still advising it would have left the two
// disagreeing. The unannotated demand stands instead -- an owner that takes a borrow
// it may then free has no annotation that makes it safe, and the type is what has to
// change.

volatile int sink;

//===----------------------------------------------------------------------===//
// Refused on an owner.
//===----------------------------------------------------------------------===//

// The reported shape: the owner frees what it was handed.
class [[gsl::Owner(int)]] Box {
  int *p;

public:
  // expected-warning@+1 {{'[[clang::lifetimebound]]' on a constructor parameter of a [[gsl::Owner]] type is not supported}}
  explicit Box(int *q [[clang::lifetimebound]]) : p(q) {}
  ~Box() { delete p; }
};

// The annotation is refused whether or not the destructor frees anything: what it
// declares -- a borrow resting in an owner -- is the part that is unsupported.
class [[gsl::Owner(int)]] Keeps {
  int *p;

public:
  // expected-warning@+1 {{'[[clang::lifetimebound]]' on a constructor parameter of a [[gsl::Owner]] type is not supported}}
  explicit Keeps(int *q [[clang::lifetimebound]]) : p(q) {}
};

// A second parameter is reported on its own.
class [[gsl::Owner(int)]] Two {
  int *a;
  int *b;

public:
  // expected-warning@+2 {{'[[clang::lifetimebound]]' on a constructor parameter of a [[gsl::Owner]] type is not supported}}
  // expected-warning@+1 {{'[[clang::lifetimebound]]' on a constructor parameter of a [[gsl::Owner]] type is not supported}}
  Two(int *x [[clang::lifetimebound]], int *y [[clang::lifetimebound]]) : a(x), b(y) {}
};

// A constructor TEMPLATE makes the same declaration, and RD->methods() does not
// list one -- so it is enumerated the same way the capture_by check enumerates.
class [[gsl::Owner(int)]] Templated {
  int *p;

public:
  // expected-warning@+2 {{'[[clang::lifetimebound]]' on a constructor parameter of a [[gsl::Owner]] type is not supported}}
  template <class T>
  explicit Templated(T *q [[clang::lifetimebound]]) : p(q) {}
};

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// A VIEW is what the annotation is for: the constructed object really does borrow.
class [[gsl::Pointer(int)]] Ref {
  int *p;

public:
  explicit Ref(int *q [[clang::lifetimebound]]) : p(q) {} // no-warning
};

// A non-constructor member of an owner is a different statement -- there the
// annotation describes the RETURN VALUE, not the object.
class [[gsl::Owner(int)]] Accessor {
  int *p;

public:
  explicit Accessor(int *q [[clang::noescape]]); // declared, not defined
  // The member is seeded with a placeholder rather than a tracked borrow, so the
  // return cannot be verified -- unrelated to the constructor question.
  // expected-warning@+1 {{could not verify that the return value can be lifetime bound}}
  int *get() const [[clang::lifetimebound]] { return p; }
};

// A plain (unannotated) record is not an owner, so nothing is refused.
struct Plain {
  int *p;
  explicit Plain(int *q [[clang::lifetimebound]]) : p(q) {} // no-warning
};

// An owner constructor with no borrow-carrying parameter at all.
class [[gsl::Owner(int)]] FromSize {
  int *p;

public:
  explicit FromSize(int n) : p(new int(n)) {} // no-warning
  ~FromSize() { delete p; }
};
