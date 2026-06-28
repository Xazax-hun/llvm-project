// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-setjmp -verify %s

// setjmp/longjmp introduce non-local control flow that the CFG does not model
// as a back-edge (longjmp transferring back to the setjmp point), so a borrow
// invalidated before the jump and used after it would be missed. Such calls are
// rejected under the safe programming model.

extern "C" int setjmp(void *buf) __attribute__((returns_twice));
extern "C" [[noreturn]] void longjmp(void *buf, int val);

extern char buf[64];

void uses_setjmp() {
  setjmp(buf); // expected-warning {{'setjmp'/'longjmp' is not modeled by lifetime safety analysis}}
}

void uses_longjmp() {
  longjmp(buf, 1); // expected-warning {{'setjmp'/'longjmp' is not modeled by lifetime safety analysis}}
}

// Control: an ordinary call (even to a [[noreturn]] function that is not the
// longjmp family) is not flagged.
[[noreturn]] void abort_now();
void ordinary() {
  abort_now(); // no-warning
}
