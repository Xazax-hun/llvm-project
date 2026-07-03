// annotations.h - lifetime-safety ownership annotations behind readable macros.
//
// The safe programming model classifies every type that can hold a borrow as
// either a resource *owner* or a non-owning *view*, spelled with the GSL
// attributes [[gsl::Owner]] and [[gsl::Pointer]]. This header hides those
// spellings behind intention-revealing macros so the entity types read as
// domain vocabulary rather than compiler annotations:
//
//   * SELF_CONTAINED - the type owns its storage/resource ([[gsl::Owner]]).
//     A borrow handed out by the object is tied to the object's lifetime.
//   * VIEW           - the type is a non-owning borrow into storage owned
//     elsewhere ([[gsl::Pointer]]); it must not outlive what it points into.
//
// Usage: place the macro where the attribute would go, e.g.
//   template <class T> struct SELF_CONTAINED SlotMap { ... };
//   struct VIEW StringRef { ... };
#ifndef LMT_ANNOTATIONS_H
#define LMT_ANNOTATIONS_H

#if defined(__clang__) && __has_cpp_attribute(gsl::Owner)
#define SELF_CONTAINED [[gsl::Owner]]
#define VIEW [[gsl::Pointer]]
#else
// On toolchains without the GSL lifetime attributes the annotations carry no
// semantics; expand to nothing so the code still compiles.
#define SELF_CONTAINED
#define VIEW
#endif

#endif // LMT_ANNOTATIONS_H
