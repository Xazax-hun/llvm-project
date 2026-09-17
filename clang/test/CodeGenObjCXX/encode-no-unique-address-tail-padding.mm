// RUN: %clang_cc1 -Wno-objc-root-class -std=gnu++20 %s -triple=x86_64-apple-darwin10 -emit-llvm -o - | FileCheck %s

// A [[no_unique_address]] member can be placed in a previous member's tail
// padding, so when the Objective-C type encoder walks a record's fields by
// offset, the next offset may PRECEDE the running one. That tripped an assert in
// getObjCEncodingForStructureImpl and aborted any Objective-C method signature
// mentioning such a type -- libc++'s std::expected is one, since it marks its
// bool __has_val_ [[no_unique_address]].
//
// `~Repr()` is load-bearing: it is what makes Repr non-standard-layout, so its
// data size (17) is smaller than its size (24) and the padding is reusable.
// `extra` then lands at offset 17, inside `u`'s tail padding.

struct Big { void *a; bool b; };
union U { Big val; void *unex; };
struct Repr { U u; bool has; ~Repr() {} };
struct Wrap { [[no_unique_address]] Repr r; };
struct Exp { [[no_unique_address]] Wrap w; bool extra; };

@interface Foo
- (Exp)get;
@end

@implementation Foo
// The overlap is a legitimate layout, so the encoding is still emitted, and it
// still describes every member -- including the `B` for `extra`, which is the one
// sitting in the padding.
// CHECK: {Exp={Wrap={Repr=(U={Big=^vB}^v)B}}B}
- (Exp)get { return Exp{}; }
@end
