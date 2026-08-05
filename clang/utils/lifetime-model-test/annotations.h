// annotations.h - lifetime-safety vocabulary behind readable macros.
//
// This header hides the two spellings the safe programming model relies on --
// the GSL ownership attributes and the soundness-region pragmas -- behind
// intention-revealing macros, so the source reads as domain vocabulary rather
// than compiler incantations.
//
// Ownership vocabulary (place where the attribute would go):
//   * SELF_CONTAINED - the type owns its storage/resource ([[gsl::Owner]]).
//     A borrow handed out by the object is tied to the object's lifetime.
//   * VIEW           - the type is a non-owning borrow into storage owned
//     elsewhere ([[gsl::Pointer]]); it must not outlive what it points into.
//
//       template <class T> struct SELF_CONTAINED SlotMap { ... };
//       struct VIEW StringRef { ... };
//
//   * PRESERVES_BORROWS - this non-const member function does not invalidate
//     borrows into the object ([[clang::lifetime_non_invalidating]]). Any
//     non-const member call on an owner is otherwise conservatively assumed to
//     reallocate it, which would dangle every borrow taken from it -- including
//     the one the call itself returns. Place it before the declaration.
//
//       PRESERVES_BORROWS T &at(std::uint32_t i) [[clang::lifetimebound]];
//
// Region markers (bracket a block of code):
//   * LIFETIME_SAFE_START / LIFETIME_SAFE_END - enable the whole
//     -Wlifetime-safety-soundness group *as errors* between the markers.
//     Enabling the group also turns the analysis on, so bracketed code needs
//     no special command-line flags. System headers are #included *outside*
//     the region, so the STL produces no noise.
//   * LIFETIME_UNSAFE_BEGIN / LIFETIME_UNSAFE_END - carve a localized
//     escape hatch *inside* a safe region: the soundness group is ignored
//     between these markers. Use it sparingly around unavoidable raw-pointer
//     FFI (syscalls, C APIs that carry no lifetime annotations).
//
//       LIFETIME_SAFE_START
//       ... code that must be lifetime-safe ...
//         LIFETIME_UNSAFE_BEGIN
//         ... raw-pointer FFI opted out of the model ...
//         LIFETIME_UNSAFE_END
//       LIFETIME_SAFE_END
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

#if defined(__clang__) && __has_cpp_attribute(clang::lifetime_non_invalidating)
#define PRESERVES_BORROWS [[clang::lifetime_non_invalidating]]
#else
#define PRESERVES_BORROWS
#endif

#if defined(__clang__)
#define LIFETIME_SAFE_START                                                    \
  _Pragma("clang diagnostic push")                                             \
  _Pragma("clang diagnostic error \"-Wlifetime-safety-soundness\"")
#define LIFETIME_SAFE_END _Pragma("clang diagnostic pop")

#define LIFETIME_UNSAFE_BEGIN                                                   \
  _Pragma("clang diagnostic push")                                             \
  _Pragma("clang diagnostic ignored \"-Wlifetime-safety-soundness\"")
#define LIFETIME_UNSAFE_END _Pragma("clang diagnostic pop")
#else
// Non-clang toolchains don't have the analysis; the markers are inert.
#define LIFETIME_SAFE_START
#define LIFETIME_SAFE_END
#define LIFETIME_UNSAFE_BEGIN
#define LIFETIME_UNSAFE_END
#endif

#endif // LMT_ANNOTATIONS_H
