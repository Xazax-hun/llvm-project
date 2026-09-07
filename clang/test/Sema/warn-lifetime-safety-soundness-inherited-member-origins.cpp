// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

// A base subobject's members are members of the derived object too, so a borrow
// parked in an inherited one is just as reachable as one in a directly declared
// member. But the origin tree walked only `RD->fields()`, so inherited members
// got no origins: a store into one was dropped and the dangling read after it
// went unreported, while the identical member declared directly in the derived
// class was caught.
//
// It was not about access: a private or protected member declared directly in a
// TU-local [[gsl::Owner]] is tracked. Only where the member was DECLARED mattered,
// and the base being an owner itself made no difference either.
//
// isTrackedField is asked about the record being expanded, not the one declaring
// the field, so an inherited non-public member of a TU-local owner is tracked
// exactly as a directly declared one is -- and a library owner's members stay
// opaque either way.

volatile int sink;

//===----------------------------------------------------------------------===//
// Caught however the member is inherited.
//===----------------------------------------------------------------------===//

struct ProtBase {
protected:
  const int *d = nullptr;
};
struct PubBase {
  const int *d = nullptr; // expected-warning {{public data member 'd' of a [[gsl::Owner]] type can hold a borrow}}
};
struct [[gsl::Owner(int)]] OwnerBase {
protected:
  const int *d = nullptr;
};

// Declared directly in the owner, which was caught all along.
class [[gsl::Owner(int)]] Direct {
  const int *d = nullptr;

public:
  static void run() {
    int *h = new int(3); // expected-warning {{allocated object does not live long enough}}
    Direct c;
    c.d = h;
    delete h; // expected-note {{freed here}}
    sink = *c.d; // expected-note {{later used here}}
  }
};

// The reported shape: a protected member of a non-owner base.
class [[gsl::Owner(int)]] FromProtBase : ProtBase {
public:
  static void run() {
    int *h = new int(3); // expected-warning {{allocated object does not live long enough}}
    FromProtBase c;
    c.d = h;
    delete h; // expected-note {{freed here}}
    sink = *c.d; // expected-note {{later used here}}
  }
};

// A public base member reaches the object the same way.
class [[gsl::Owner(int)]] FromPubBase : PubBase {
public:
  static void run() {
    int *h = new int(3); // expected-warning {{allocated object does not live long enough}}
    FromPubBase c;
    c.d = h;
    delete h; // expected-note {{freed here}}
    sink = *c.d; // expected-note {{later used here}}
  }
};

// ...and so does one from a base that is itself an owner.
class [[gsl::Owner(int)]] FromOwnerBase : OwnerBase {
public:
  static void run() {
    int *h = new int(3); // expected-warning {{allocated object does not live long enough}}
    FromOwnerBase c;
    c.d = h;
    delete h; // expected-note {{freed here}}
    sink = *c.d; // expected-note {{later used here}}
  }
};

// Two levels of inheritance: the field is still a member of the object.
struct MidBase : ProtBase {};

class [[gsl::Owner(int)]] FromGrandBase : MidBase {
public:
  static void run() {
    int *h = new int(3); // expected-warning {{allocated object does not live long enough}}
    FromGrandBase c;
    c.d = h;
    delete h; // expected-note {{freed here}}
    sink = *c.d; // expected-note {{later used here}}
  }
};

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

static const int kImmortal = 5;

class [[gsl::Owner(int)]] FromImmortal : ProtBase {
public:
  static void run() {
    FromImmortal c;
    c.d = &kImmortal; // no-warning
    sink = *c.d;
  }
};

// The borrow outlives the read, so nothing dangles.
class [[gsl::Owner(int)]] ReadBeforeDeath : ProtBase {
public:
  static void run() {
    int x = 1;
    ReadBeforeDeath c;
    c.d = &x;
    sink = *c.d; // no-warning: `x` is still alive here
  }
};
