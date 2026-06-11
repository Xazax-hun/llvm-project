//===- LifetimeAnnotations.h -  -*--------------- C++--------------------*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Helper functions to inspect and infer lifetime annotations.
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMEANNOTATIONS_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMEANNOTATIONS_H

#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "llvm/ADT/DenseMap.h"

namespace clang ::lifetimes {

// This function is needed because Decl::isInStdNamespace will return false for
// iterators in some STL implementations due to them being defined in a
// namespace outside of the std namespace.
bool isInStlNamespace(const Decl *D);

bool isPointerLikeType(QualType QT);

/// Returns the most recent declaration of the method to ensure all
/// lifetime-bound attributes from redeclarations are considered.
const FunctionDecl *getDeclWithMergedLifetimeBoundAttrs(const FunctionDecl *FD);

/// Returns the most recent declaration of the method to ensure all
/// lifetime-bound attributes from redeclarations are considered.
const CXXMethodDecl *
getDeclWithMergedLifetimeBoundAttrs(const CXXMethodDecl *CMD);

// Return true if this is an "normal" assignment operator.
// We assume that a normal assignment operator always returns *this, that is,
// an lvalue reference that is the same type as the implicit object parameter
// (or the LHS for a non-member operator==).
bool isNormalAssignmentOperator(const FunctionDecl *FD);

/// Returns true if this is an assignment operator where the parameter
/// has the lifetimebound attribute.
bool isAssignmentOperatorLifetimeBound(const CXXMethodDecl *CMD);

/// Returns the lifetimebound attribute for the implicit this parameter, if it
/// exists on the current type.
const LifetimeBoundAttr *
getDirectImplicitObjectLifetimeBoundAttr(const FunctionDecl *FD);

/// Returns the lifetimebound attribute for the implicit this parameter, if it
/// exists on any redeclaration.
const LifetimeBoundAttr *
getImplicitObjectParamLifetimeBoundAttr(const FunctionDecl *FD);

/// Returns true if the implicit object parameter (this) should be considered
/// lifetimebound, either due to an explicit lifetimebound attribute on the
/// method or because it's a normal assignment operator.
bool implicitObjectParamIsLifetimeBound(const FunctionDecl *FD);

// Returns true if the implicit object argument (this) of a method call should
// be tracked for GSL lifetime analysis. This applies to STL methods that return
// pointers or references that depend on the lifetime of the object, such as
// container iterators (begin, end), data accessors (c_str, data, get),
// element accessors (operator[], operator*, front, back, at), or propagating
// operations (operator+, operator-, operator++, operator--).
bool shouldTrackImplicitObjectArg(const Expr &ImplicitObjectArgument,
                                  const CXXMethodDecl *Callee,
                                  bool RunningUnderLifetimeSafety);

// Returns true if the first argument of a free function should be tracked for
// GSL lifetime analysis. This applies to STL free functions that take a pointer
// to a GSL Owner or Pointer and return a pointer or reference that depends on
// the lifetime of the argument, such as std::begin, std::data, std::get, or
// std::any_cast.
bool shouldTrackFirstArgument(const FunctionDecl *FD);

// Returns true if the second argument of a free function should be tracked for
// lifetime analysis. This applies to free operator functions that take a
// GSL Pointer as their second argument.
bool shouldTrackSecondArgument(const FunctionDecl *FD);

// Tells whether the type is annotated with [[gsl::Pointer]].
bool isGslPointerType(QualType QT);
// Tells whether the type is annotated with [[gsl::Owner]].
bool isGslOwnerType(QualType QT);
bool isGslOwnerType(const CXXRecordDecl *RD);

// Returns true if QT is a [[gsl::Owner]] class template specialization with a
// type template argument that is an indirection (pointer, reference, or
// gsl::Pointer), e.g. std::vector<int*> or std::array<std::string_view, N>.
// Such a container's elements hold borrows the analysis cannot track per
// element, so the safe programming model rejects it. The check recurses into
// owner template arguments (e.g. std::vector<std::vector<int*>>).
bool isGslOwnerOfIndirection(QualType QT);

// Returns true if the given gsl::Pointer (view) type has a pointee/element type
// that is itself an indirection (pointer, reference, gsl::Pointer, or a
// container of indirections), e.g. std::span<int*>. Such a view hands out
// borrows one level deeper than it tracks; those inner pointees are not modeled,
// so the safe programming model rejects it -- the gsl::Pointer analogue of
// isGslOwnerOfIndirection. The pointee is determined via a value_type/
// element_type typedef, else operator*/operator-> return type, else a sole
// template argument.
bool isGslPointerOfIndirection(QualType QT);

// Returns true if the given gsl::Pointer type can mutate the (non-const) owner
// it points to, i.e. it (or a base class) exposes operator*/operator[]
// returning a reference to a non-const gsl::Owner, or operator-> returning a
// pointer to a non-const gsl::Owner. Passing such a pointer to a function is
// assumed to invalidate borrows into the pointed-to owner.
bool pointsToMutableOwner(QualType GslPointerType);

// Returns true if QT is a user-defined record type that can hold a borrow (has
// a pointer/reference/gsl::Pointer member, transitively) but is annotated
// neither [[gsl::Owner]] nor [[gsl::Pointer]], so the analysis cannot tell
// whether it owns or merely refers to that storage. An incomplete record type
// is conservatively treated as having unknown ownership. \p Cache memoizes
// results by canonical type.
bool isUnknownOwnershipType(QualType QT,
                            llvm::DenseMap<const Type *, bool> &Cache);

// Returns true if the given method is std::unique_ptr::release().
// This is treated as a move in lifetime analysis to avoid false-positives
// when ownership is manually transferred.
bool isUniquePtrRelease(const CXXMethodDecl &MD);

// Returns true if the given method invalidates references tracked by lifetime
// analysis (e.g. vector::push_back). Methods that only invalidate iterators but
// not references (e.g. unordered_map::emplace) are not considered invalidating
// here.
//
// Container invalidation rules are based on:
// https://en.cppreference.com/w/cpp/container#Iterator_invalidation
bool isInvalidationMethod(const CXXMethodDecl &MD);

// Returns true if the given method is an STL container insertion method
// (e.g. push_back, insert, emplace). Such methods take the element by const
// reference or rvalue reference and copy/move it into the container, so the
// reference parameter is effectively noescape.
bool isStlContainerInsertionMethod(const CXXMethodDecl &MD);

// Returns true if the function is a std::basic_string concatenation operator:
// the member `operator+=` (append-assign) or a free `operator+` returning a
// basic_string. Both deep-copy their operands' characters into a string, so an
// operand reference (or character pointer) does not escape.
bool isStlStringConcatenationOperator(const FunctionDecl &FD);

// Returns true if the record is a standard container template (vector, map,
// basic_string, ...). A constructor of such a type copies/moves in the values
// it is given, so a by-reference parameter to a non-borrow-holding type does
// not escape.
bool isStlContainerType(const CXXRecordDecl *RD);

// Returns true if the function destroys its first argument
// (e.g., destructors via implicit 'this', std::destroy_at).
bool destructsFirstArg(const FunctionDecl &FD);

/// Returns true for standard library callable wrappers (e.g., std::function)
/// that can propagate the stored lambda's origins.
bool isStdCallableWrapperType(const CXXRecordDecl *RD);

/// Returns true for std reference-cast builtins (e.g., std::move). Their result
/// refers to the same object as the argument, so all origins propagate from
/// argument to result.
bool isStdReferenceCast(const FunctionDecl *FD);

} // namespace clang::lifetimes

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMEANNOTATIONS_H
