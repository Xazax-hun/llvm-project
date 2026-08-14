//===- LifetimeAnnotations.cpp -  -*--------------- C++------------------*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/OperatorKinds.h"
#include "llvm/ADT/StringSet.h"

namespace clang::lifetimes {

const FunctionDecl *
getDeclWithMergedLifetimeBoundAttrs(const FunctionDecl *FD) {
  return FD != nullptr ? FD->getMostRecentDecl() : nullptr;
}

const CXXMethodDecl *
getDeclWithMergedLifetimeBoundAttrs(const CXXMethodDecl *CMD) {
  const FunctionDecl *FD = CMD;
  return cast_if_present<CXXMethodDecl>(
      getDeclWithMergedLifetimeBoundAttrs(FD));
}

bool isNormalAssignmentOperator(const FunctionDecl *FD) {
  OverloadedOperatorKind OO = FD->getDeclName().getCXXOverloadedOperator();
  bool IsAssignment = OO == OO_Equal || isCompoundAssignmentOperator(OO);
  if (!IsAssignment)
    return false;
  QualType RetT = FD->getReturnType();
  if (!RetT->isLValueReferenceType())
    return false;
  ASTContext &Ctx = FD->getASTContext();
  QualType LHST;
  auto *MD = dyn_cast<CXXMethodDecl>(FD);
  if (MD && MD->isCXXInstanceMember())
    LHST = Ctx.getLValueReferenceType(MD->getFunctionObjectParameterType());
  else
    LHST = FD->getParamDecl(0)->getType();
  return Ctx.hasSameType(RetT, LHST);
}

bool isAssignmentOperatorLifetimeBound(const CXXMethodDecl *CMD) {
  CMD = getDeclWithMergedLifetimeBoundAttrs(CMD);
  return CMD && isNormalAssignmentOperator(CMD) && CMD->param_size() == 1 &&
         CMD->getParamDecl(0)->hasAttr<clang::LifetimeBoundAttr>();
}

/// Check if a function has a lifetimebound attribute on its function type
/// (which represents the implicit 'this' parameter for methods).
/// Returns the attribute if found, nullptr otherwise.
static const LifetimeBoundAttr *
getLifetimeBoundAttrFromFunctionType(const TypeSourceInfo &TSI) {
  // Walk through the type layers looking for a lifetimebound attribute.
  TypeLoc TL = TSI.getTypeLoc();
  while (true) {
    auto ATL = TL.getAsAdjusted<AttributedTypeLoc>();
    if (!ATL)
      break;
    if (auto *LBAttr = ATL.getAttrAs<LifetimeBoundAttr>())
      return LBAttr;
    TL = ATL.getModifiedLoc();
  }
  return nullptr;
}

const LifetimeBoundAttr *
getDirectImplicitObjectLifetimeBoundAttr(const FunctionDecl *FD) {
  if (const TypeSourceInfo *TSI = FD->getTypeSourceInfo())
    if (const auto *Attr = getLifetimeBoundAttrFromFunctionType(*TSI))
      return Attr;
  return nullptr;
}

const LifetimeBoundAttr *
getImplicitObjectParamLifetimeBoundAttr(const FunctionDecl *FD) {
  FD = getDeclWithMergedLifetimeBoundAttrs(FD);
  // Attribute merging doesn't work well with attributes on function types (like
  // 'this' param). We need to check all redeclarations.
  auto CheckRedecls = [](const FunctionDecl *F) -> const LifetimeBoundAttr * {
    for (const FunctionDecl *Redecl : F->redecls())
      if (const auto *Attr = getDirectImplicitObjectLifetimeBoundAttr(Redecl))
        return Attr;
    return nullptr;
  };

  if (const auto *Attr = CheckRedecls(FD))
    return Attr;
  if (const FunctionDecl *Pattern = FD->getTemplateInstantiationPattern())
    return CheckRedecls(Pattern);
  return nullptr;
}

bool implicitObjectParamIsLifetimeBound(const FunctionDecl *FD) {
  if (getImplicitObjectParamLifetimeBoundAttr(FD))
    return true;
  return isNormalAssignmentOperator(FD);
}

bool carriesDestructionOrderPromise(const FunctionDecl *FD) {
  if (!FD)
    return false;
  if (FD->hasAttr<DestructionOrderSafeAttr>())
    return true;
  // On a class, the attribute is a promise about that class's destructor.
  if (const auto *DD = dyn_cast<CXXDestructorDecl>(FD))
    return DD->getParent()->hasAttr<DestructionOrderSafeAttr>();
  return false;
}

bool isInStlNamespace(const Decl *D) {
  for (const DeclContext *DC = D->getDeclContext(); DC; DC = DC->getParent()) {
    if (DC->isStdNamespace())
      return true;
    if (const auto *ND = dyn_cast<NamespaceDecl>(DC))
      if (const IdentifierInfo *II = ND->getIdentifier()) {
        StringRef Name = II->getName();
        if (Name.size() >= 2 && Name.front() == '_' &&
            (Name[1] == '_' || isUppercase(Name[1])))
          return true;
      }
  }
  return false;
}

bool isPointerLikeType(QualType QT) {
  return isGslPointerType(QT) || QT->isPointerType() || QT->isNullPtrType();
}

bool isThisExpr(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  if (isa<CXXThisExpr>(E))
    return true;
  // A derived-to-base (or value-preserving) pointer cast of `this`, e.g.
  // `static_cast<Base*>(this)` / the C-style `(Base*)this`: an explicit cast
  // that IgnoreParenImpCasts does not strip. A member store through such a base
  // view of `this` still names a field of the enclosing object.
  if (const auto *CE = dyn_cast<CastExpr>(E))
    switch (CE->getCastKind()) {
    case CK_DerivedToBase:
    case CK_UncheckedDerivedToBase:
    case CK_BaseToDerived:
    case CK_NoOp:
      return isThisExpr(CE->getSubExpr());
    default:
      break;
    }
  // `*this`: a dereference of `this`.
  if (const auto *UO = dyn_cast<UnaryOperator>(E);
      UO && UO->getOpcode() == UO_Deref) {
    const Expr *Sub = UO->getSubExpr()->IgnoreParenImpCasts();
    // Collapse a `*&X` round-trip (`*&*this`, `*&this`, ...): the deref of an
    // address-of denotes X itself.
    if (const auto *Inner = dyn_cast<UnaryOperator>(Sub);
        Inner && Inner->getOpcode() == UO_AddrOf)
      return isThisExpr(Inner->getSubExpr());
    return isThisExpr(Sub);
  }
  return false;
}

const Expr *getArrayObjectOfElementAccess(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  // `arr[i]` / `i[arr]`: a genuine array subscript (base is array-typed). A
  // subscript of a real pointer is an ordinary indirection, not an element of a
  // known array object.
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    const Expr *Base = ASE->getBase()->IgnoreParenImpCasts();
    if (Base->getType()->isArrayType())
      return Base;
    return nullptr;
  }
  // `*(arr + i)`, `*arr`: a dereference of an array that has decayed to a
  // pointer, optionally offset by pointer arithmetic. `arr[i]` is defined as
  // `*(arr + i)`, so this denotes the same element storage. Look through the
  // arithmetic to the ArrayToPointerDecay of the array object. Note: we strip
  // only parens here (not implicit casts), because the ArrayToPointerDecay we
  // are looking for is itself an implicit cast.
  if (const auto *UO = dyn_cast<UnaryOperator>(E);
      UO && UO->getOpcode() == UO_Deref) {
    const Expr *Ptr = UO->getSubExpr()->IgnoreParens();
    // Peel additive pointer arithmetic (`arr + i`, `i + arr`), keeping the
    // pointer-typed operand.
    while (const auto *BO = dyn_cast<BinaryOperator>(Ptr)) {
      if (!BO->isAdditiveOp())
        break;
      const Expr *L = BO->getLHS()->IgnoreParens();
      const Expr *R = BO->getRHS()->IgnoreParens();
      Ptr = L->getType()->isPointerType() ? L : R;
    }
    // The pointer must be an array-to-pointer decay; its operand is the array
    // object expression.
    if (const auto *ICE = dyn_cast<ImplicitCastExpr>(Ptr);
        ICE && ICE->getCastKind() == CK_ArrayToPointerDecay)
      return ICE->getSubExpr()->IgnoreParenImpCasts();
  }
  return nullptr;
}

static bool isReferenceOrPointerLikeType(QualType QT) {
  return QT->isReferenceType() || isPointerLikeType(QT);
}

bool shouldTrackImplicitObjectArg(const Expr &ImplicitObjectArgument,
                                  const CXXMethodDecl *Callee,
                                  bool RunningUnderLifetimeSafety) {
  if (!Callee)
    return false;
  // Check both the declaring class and the call-site object: a gsl::Owner
  // may inherit its accessors from a non-Owner base (e.g. libc++ optional).
  const bool IsGslOwnerImplicitObject =
      isGslOwnerType(Callee->getFunctionObjectParameterType()) ||
      (RunningUnderLifetimeSafety &&
       isGslOwnerType(ImplicitObjectArgument.getBestDynamicClassType()));
  if (auto *Conv = dyn_cast<CXXConversionDecl>(Callee))
    if (isGslPointerType(Conv->getConversionType()) && IsGslOwnerImplicitObject)
      return true;
  if (!isGslPointerType(Callee->getFunctionObjectParameterType()) &&
      !IsGslOwnerImplicitObject)
    return false;

  // Begin and end iterators.
  static const llvm::StringSet<> IteratorMembers = {
      "begin", "end", "rbegin", "rend", "cbegin", "cend", "crbegin", "crend"};
  static const llvm::StringSet<> InnerPointerGetters = {
      // Inner pointer getters.
      "c_str", "data", "get"};
  static const llvm::StringSet<> ContainerFindFns = {
      // Map and set types.
      "find", "equal_range", "lower_bound", "upper_bound"};
  // Track dereference operator and transparent functions like begin(), get(),
  // etc. for all GSL pointers. Only do so for lifetime safety analysis and not
  // for Sema's statement-local analysis as it starts to have false-positives.
  if (RunningUnderLifetimeSafety &&
      isGslPointerType(Callee->getFunctionObjectParameterType()) &&
      isReferenceOrPointerLikeType(Callee->getReturnType())) {
    // Propagate origins through GSL pointer arithmetic and dereference
    // operators.
    switch (Callee->getOverloadedOperator()) {
    case OO_Arrow:
    case OO_Star:
    case OO_Plus:
    case OO_Minus:
    case OO_PlusPlus:
    case OO_MinusMinus:
      return true;
    default:
      break;
    }
    if (Callee->getIdentifier() &&
        (IteratorMembers.contains(Callee->getName()) ||
         InnerPointerGetters.contains(Callee->getName())))
      return true;
  }

  // A [[gsl::Owner]]'s accessor that hands out a borrow of its pointee/contents
  // -- `operator*`/`operator->`/`get`/`data`/`c_str` returning a pointer or
  // reference -- behaves like a lifetimebound accessor of the owner, regardless
  // of namespace (a user-defined owning smart pointer makes the same contract as
  // std::unique_ptr). Recognize it so the borrow is rooted at the owner (not an
  // untracked Unknown loan): the const-subversion check relies on this to see
  // that a const method mutating through `owned->m()` reaches `this`.
  if (RunningUnderLifetimeSafety && IsGslOwnerImplicitObject &&
      isReferenceOrPointerLikeType(Callee->getReturnType())) {
    switch (Callee->getOverloadedOperator()) {
    case OO_Arrow:
    case OO_Star:
      return true;
    default:
      break;
    }
    if (Callee->getIdentifier() && InnerPointerGetters.contains(Callee->getName()))
      return true;
  }

  if (!isInStlNamespace(Callee->getParent()))
    return false;

  if (isPointerLikeType(Callee->getReturnType())) {
    if (!Callee->getIdentifier())
      // e.g., std::optional<T>::operator->() returns T*.
      return RunningUnderLifetimeSafety
                 ? IsGslOwnerImplicitObject &&
                       Callee->getOverloadedOperator() ==
                           OverloadedOperatorKind::OO_Arrow
                 : false;
    return IteratorMembers.contains(Callee->getName()) ||
           InnerPointerGetters.contains(Callee->getName()) ||
           ContainerFindFns.contains(Callee->getName());
  }
  if (Callee->getReturnType()->isReferenceType()) {
    if (!Callee->getIdentifier()) {
      auto OO = Callee->getOverloadedOperator();
      if (!IsGslOwnerImplicitObject)
        return false;
      return OO == OverloadedOperatorKind::OO_Subscript ||
             OO == OverloadedOperatorKind::OO_Star;
    }
    return llvm::StringSwitch<bool>(Callee->getName())
        .Cases({"front", "back", "at", "top", "value"}, true)
        .Default(false);
  }
  return false;
}

bool shouldTrackFirstArgument(const FunctionDecl *FD) {
  if (!FD->getIdentifier() || FD->getNumParams() < 1)
    return false;
  if (!FD->isInStdNamespace())
    return false;
  // Track std:: algorithm functions that return an iterator whose lifetime is
  // bound to the first argument.
  if (FD->getNumParams() >= 2 && FD->isInStdNamespace() &&
      isGslPointerType(FD->getReturnType())) {
    if (llvm::StringSwitch<bool>(FD->getName())
            .Cases(
                {
                    "find",
                    "find_if",
                    "find_if_not",
                    "find_first_of",
                    "adjacent_find",
                    "search",
                    "find_end",
                    "lower_bound",
                    "upper_bound",
                    "partition_point",
                },
                true)
            .Default(false))
      return true;
  }
  const auto *RD = FD->getParamDecl(0)->getType()->getPointeeCXXRecordDecl();
  if (!RD || !RD->isInStdNamespace())
    return false;
  if (!RD->hasAttr<PointerAttr>() && !RD->hasAttr<OwnerAttr>())
    return false;

  if (FD->getNumParams() != 1)
    return false;

  if (FD->getReturnType()->isPointerType() ||
      isGslPointerType(FD->getReturnType())) {
    return llvm::StringSwitch<bool>(FD->getName())
        .Cases({"begin", "rbegin", "cbegin", "crbegin"}, true)
        .Cases({"end", "rend", "cend", "crend"}, true)
        .Case("data", true)
        .Default(false);
  }
  if (FD->getReturnType()->isReferenceType()) {
    return llvm::StringSwitch<bool>(FD->getName())
        .Cases({"get", "any_cast"}, true)
        .Default(false);
  }
  return false;
}

bool shouldTrackSecondArgument(const FunctionDecl *FD) {
  if (FD->getNumParams() < 2)
    return false;
  const auto *RD = FD->getParamDecl(1)->getType()->getAsCXXRecordDecl();
  if (!RD)
    return false;
  // For free-standing `+`/`-` operators annotated with `gsl::Pointer`, track
  // the second parameter when its type matches the return type.
  return RD->hasAttr<PointerAttr>() &&
         (FD->getOverloadedOperator() == OO_Plus ||
          FD->getOverloadedOperator() == OO_Minus) &&
         ASTContext::hasSameUnqualifiedType(FD->getParamDecl(1)->getType(),
                                            FD->getReturnType()) &&
         !isa<CXXMethodDecl>(FD);
}

template <typename T> static bool isRecordWithAttr(const CXXRecordDecl *RD) {
  if (!RD)
    return false;
  // Generally, if a primary template class declaration is annotated with an
  // attribute, all its specializations generated from template instantiations
  // should inherit the attribute.
  //
  // However, since lifetime analysis occurs during parsing, we may encounter
  // cases where a full definition of the specialization is not required. In
  // such cases, the specialization declaration remains incomplete and lacks the
  // attribute. Therefore, we fall back to checking the primary template class.
  //
  // Note: it is possible for a specialization declaration to have an attribute
  // even if the primary template does not.
  //
  // FIXME: What if the primary template and explicit specialization
  // declarations have conflicting attributes? We should consider diagnosing
  // this scenario.
  bool Result = RD->hasAttr<T>();

  if (auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(RD))
    Result |= CTSD->getSpecializedTemplate()->getTemplatedDecl()->hasAttr<T>();

  return Result;
}

template <typename T> static bool isRecordWithAttr(QualType Type) {
  return isRecordWithAttr<T>(Type->getAsCXXRecordDecl());
}

bool isGslPointerType(QualType QT) { return isRecordWithAttr<PointerAttr>(QT); }
bool isGslOwnerType(QualType QT) { return isRecordWithAttr<OwnerAttr>(QT); }
bool isGslOwnerType(const CXXRecordDecl *RD) {
  return isRecordWithAttr<OwnerAttr>(RD);
}

/// The declared deref/owned type written in a [[gsl::Owner(T)]] /
/// [[gsl::Pointer(T)]] attribute's optional type argument, or a null QualType
/// when the argument is absent. Mirrors isRecordWithAttr's fallback to the
/// primary template for a specialization that did not re-declare the attribute.
template <typename AttrT> static QualType gslAttrDerefType(QualType QT) {
  const CXXRecordDecl *RD = QT->getAsCXXRecordDecl();
  if (!RD)
    return QualType();
  const AttrT *A = RD->getAttr<AttrT>();
  if (!A)
    if (const auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(RD))
      A = CTSD->getSpecializedTemplate()->getTemplatedDecl()->getAttr<AttrT>();
  if (!A || !A->getDerefTypeLoc())
    return QualType();
  return A->getDerefType();
}

static bool lambdaCapturesBorrow(const CXXRecordDecl *Lambda,
                                 llvm::DenseMap<const Type *, bool> &Cache);

// Whether a record field / lambda capture / template-argument of type T holds a
// borrow the enclosing record cannot track through it: a raw pointer/reference
// or a gsl::Pointer view (both via isPointerLikeType), a type-erased callable
// wrapper (std::function / std::move_only_function -- its captures are invisible
// and may be borrows), a closure that captures a borrow, or another complete,
// unannotated borrow-holding user type (unknown ownership). An *incomplete*
// record is conservatively NOT counted: an uninstantiated deleter/allocator
// (e.g. unique_ptr<T>'s default_delete) or a forward-declared pimpl element
// cannot be shown to hold a borrow, and isUnknownOwnershipType reports
// incomplete types as unknown, which would spuriously flag those.
//
// This is the single predicate shared by isUnknownOwnershipType,
// lambdaCapturesBorrow, and isIndirectionElement so all three agree on what
// "holds a borrow" means -- in particular, all recognize callable wrappers.
// Without a shared definition a std::function member hid a captured borrow from
// the ownership checks (e.g. unique_ptr<struct{std::function<...>}> was trusted
// as a plain owner).
static bool fieldTypeHoldsBorrow(QualType T,
                                 llvm::DenseMap<const Type *, bool> &Cache) {
  // Peel array dimensions: `P a[N]` holds a borrow iff `P` does.
  while (const ArrayType *AT = T->getAsArrayTypeUnsafe())
    T = AT->getElementType();
  if (isPointerLikeType(T) || T->isReferenceType())
    return true;
  const CXXRecordDecl *RD = T->getAsCXXRecordDecl();
  if (!RD || !RD->hasDefinition())
    return false;
  if (isStdCallableWrapperType(RD))
    return true;
  if (RD->isLambda())
    return lambdaCapturesBorrow(RD, Cache);
  return isUnknownOwnershipType(T, Cache);
}

// Whether an element/template-argument type counts as an "indirection" the
// analysis cannot track per element. Besides anything that holds a borrow (see
// fieldTypeHoldsBorrow), this includes a container-/owner-of-indirection element
// (e.g. std::vector<std::string_view>), which hands out borrows one level deeper
// than the outer container tracks. Shared by the gsl::Owner-of-indirection check
// and the std::variant/any check below.
static bool isIndirectionElement(QualType T,
                                 llvm::DenseMap<const Type *, bool> &Cache) {
  if (isGslOwnerOfIndirection(T, Cache))
    return true;
  return fieldTypeHoldsBorrow(T, Cache);
}

bool isGslOwnerOfIndirection(QualType QT,
                             llvm::DenseMap<const Type *, bool> &Cache) {
  if (!isGslOwnerType(QT))
    return false;
  // A [[gsl::Owner(T)]] whose declared owned type T is itself an indirection
  // (e.g. struct [[gsl::Owner(std::string_view)]] S) holds a borrow it cannot
  // track per element -- and the annotation is self-contradictory (an owner of
  // a view). Treat it like a container of indirections. This is the
  // attribute-argument analogue of the template-argument check below.
  if (QualType D = gslAttrDerefType<OwnerAttr>(QT);
      !D.isNull() && isIndirectionElement(D, Cache))
    return true;
  const auto *CTSD = dyn_cast_if_present<ClassTemplateSpecializationDecl>(
      QT->getAsCXXRecordDecl());
  if (!CTSD)
    return false;
  for (const TemplateArgument &Arg : CTSD->getTemplateArgs().asArray()) {
    if (Arg.getKind() != TemplateArgument::Type)
      continue;
    QualType ArgT = Arg.getAsType();
    // A type template argument that is itself an indirection means the
    // container's elements hold borrows the analysis cannot track per element.
    // This is the case for a pointer, reference, or gsl::Pointer; for a nested
    // container of indirections (e.g. vector<vector<int*>>), handled by
    // recursing; and for a complete, unannotated user type that holds a borrow
    // (unknown ownership, e.g. unique_ptr<Box> where Box has a view member) --
    // the element type itself must be annotated [[gsl::Owner]]/[[gsl::Pointer]]
    // to be modeled, so reject the container until it is.
    if (isIndirectionElement(ArgT, Cache))
      return true;
  }
  return false;
}

/// The type a gsl::Pointer view dereferences to (its pointee / element type).
/// Tries, in order: the [[gsl::Pointer(T)]] attribute's optional type argument
/// T (the authoritative declared pointee), a `value_type`/`element_type` member
/// typedef (std views and iterators), the return type of `operator*`/`operator->`
/// (a custom view that lacks the typedef), and finally -- only for a
/// single-type-argument specialization -- that sole template argument. Returns a
/// null QualType if the pointee cannot be determined.
static QualType gslPointerPointeeType(QualType QT) {
  // (0) [[gsl::Pointer(T)]] declares the pointee type directly.
  if (QualType D = gslAttrDerefType<PointerAttr>(QT); !D.isNull())
    return D;
  const auto *RD = QT->getAsCXXRecordDecl();
  if (!RD)
    return QualType();
  if (const auto *Def = RD->getDefinition())
    RD = Def;
  // (1) value_type / element_type member typedef.
  for (const Decl *D : RD->decls())
    if (const auto *TND = dyn_cast<TypedefNameDecl>(D))
      if (TND->getName() == "value_type" || TND->getName() == "element_type")
        return TND->getUnderlyingType();
  // (2) operator* / operator-> return type (the actual dereference result). For
  // operator-> (returns a pointer to the pointee) strip one pointer level.
  for (const CXXMethodDecl *M : RD->methods()) {
    OverloadedOperatorKind OO = M->getOverloadedOperator();
    if (OO == OO_Star)
      return M->getReturnType().getNonReferenceType();
    if (OO == OO_Arrow && M->getReturnType()->isPointerType())
      return M->getReturnType()->getPointeeType();
  }
  // (3) Fallback: a sole type template argument.
  if (const auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(RD)) {
    QualType Sole;
    for (const TemplateArgument &Arg : CTSD->getTemplateArgs().asArray()) {
      if (Arg.getKind() != TemplateArgument::Type)
        continue;
      if (!Sole.isNull())
        return QualType(); // more than one type arg: ambiguous, give up
      Sole = Arg.getAsType();
    }
    return Sole;
  }
  return QualType();
}

bool isGslPointerOfIndirection(QualType QT,
                               llvm::DenseMap<const Type *, bool> &Cache) {
  if (!isGslPointerType(QT))
    return false;
  QualType Pointee = gslPointerPointeeType(QT);
  if (Pointee.isNull())
    return false;
  // A view whose pointee is itself an indirection (e.g. std::span<int*>) hands
  // out borrows one level deeper than the view tracks; those inner pointees are
  // not modeled. Recurse so a view-of-views is caught too; a complete,
  // unannotated borrow-holding pointee (unknown ownership) is likewise rejected.
  // The pointee must be complete to inspect for borrows (see
  // isGslOwnerOfIndirection).
  const CXXRecordDecl *PointeeRD = Pointee->getAsCXXRecordDecl();
  bool PointeeUnknownOwnership = PointeeRD && PointeeRD->hasDefinition() &&
                                 isUnknownOwnershipType(Pointee, Cache);
  return isPointerLikeType(Pointee) || Pointee->isReferenceType() ||
         isGslOwnerOfIndirection(Pointee, Cache) ||
         isGslPointerOfIndirection(Pointee, Cache) || PointeeUnknownOwnership;
}

static QualType findNestedOwnerOrPointerOfIndirectionImpl(
    QualType QT, llvm::DenseMap<const Type *, bool> &Cache,
    llvm::SmallPtrSetImpl<const Type *> &Visited, bool &IsPointer);

// Inspects one template argument: a type argument is descended into; a pack
// argument (variadic template, e.g. std::tuple<...>) is flattened into its
// elements; other argument kinds are ignored.
static QualType findIndirectionInTemplateArg(
    const TemplateArgument &Arg, llvm::DenseMap<const Type *, bool> &Cache,
    llvm::SmallPtrSetImpl<const Type *> &Visited, bool &IsPointer) {
  if (Arg.getKind() == TemplateArgument::Pack) {
    for (const TemplateArgument &Elt : Arg.pack_elements())
      if (QualType Found =
              findIndirectionInTemplateArg(Elt, Cache, Visited, IsPointer);
          !Found.isNull())
        return Found;
    return QualType();
  }
  if (Arg.getKind() != TemplateArgument::Type)
    return QualType();
  return findNestedOwnerOrPointerOfIndirectionImpl(Arg.getAsType(), Cache,
                                                   Visited, IsPointer);
}

static QualType findNestedOwnerOrPointerOfIndirectionImpl(
    QualType QT, llvm::DenseMap<const Type *, bool> &Cache,
    llvm::SmallPtrSetImpl<const Type *> &Visited, bool &IsPointer) {
  // A type that is itself an owner-/pointer-of-indirection is the answer. (For a
  // recognized owner/view these already recurse through their own template
  // arguments, so the explicit descent below only matters for a non-owner
  // aggregate such as std::pair/std::tuple, whose arguments nothing inspects.)
  if (isGslOwnerOfIndirection(QT, Cache)) {
    IsPointer = false;
    return QT;
  }
  if (isGslPointerOfIndirection(QT, Cache)) {
    IsPointer = true;
    return QT;
  }
  const CXXRecordDecl *RD = QT->getAsCXXRecordDecl();
  if (!RD || !RD->hasDefinition())
    return QualType();
  if (!Visited.insert(QT.getCanonicalType().getTypePtr()).second)
    return QualType();
  // std::variant / std::any keep their value in a union / type-erased buffer
  // that the origin model does not expand, so a borrow-holding alternative is
  // invisible -- unlike std::pair/std::tuple, whose members ARE tracked, or
  // std::optional, a [[gsl::Owner]] already recognized by
  // isGslOwnerOfIndirection. Treat such a holder of an indirection alternative
  // like a container of indirections. (Without this,
  // std::variant<int, std::string_view> is silently untracked while the
  // equivalent std::optional<std::string_view> is flagged.)
  if (const auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(RD);
      CTSD && isInStlNamespace(RD) &&
      (RD->getName() == "variant" || RD->getName() == "any")) {
    llvm::SmallVector<TemplateArgument, 8> Worklist(
        CTSD->getTemplateArgs().asArray());
    while (!Worklist.empty()) {
      TemplateArgument A = Worklist.pop_back_val();
      if (A.getKind() == TemplateArgument::Pack)
        Worklist.append(A.pack_elements().begin(), A.pack_elements().end());
      else if (A.getKind() == TemplateArgument::Type &&
               isIndirectionElement(A.getAsType(), Cache)) {
        // A variant/any is a value container (like std::optional, a recognized
        // owner-of-indirection), not a view -- report it as owner-of-indirection.
        IsPointer = false;
        return A.getAsType();
      }
    }
  }
  // Descend the template arguments of a non-owner aggregate such as
  // std::pair/std::tuple/std::unique_ptr, whose arguments nothing else inspects.
  if (const auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(RD))
    for (const TemplateArgument &Arg : CTSD->getTemplateArgs().asArray())
      if (QualType Found =
              findIndirectionInTemplateArg(Arg, Cache, Visited, IsPointer);
          !Found.isNull())
        return Found;
  // Descend plain (non-template) data members: a user aggregate `struct Outer {
  // Inner inner; }` whose member is itself (or transitively contains) an owner-/
  // pointer-of-indirection is just as untrackable, but the field walk that flags
  // the inner record's own definition is suppressed in system headers, so the
  // borrow would slip when Outer is used (local/parameter/return/member).
  for (const FieldDecl *F : RD->fields()) {
    QualType FT = F->getType();
    while (const ArrayType *AT = FT->getAsArrayTypeUnsafe())
      FT = AT->getElementType();
    if (QualType Found = findNestedOwnerOrPointerOfIndirectionImpl(
            FT, Cache, Visited, IsPointer);
        !Found.isNull())
      return Found;
  }
  return QualType();
}

QualType findNestedOwnerOrPointerOfIndirection(
    QualType QT, llvm::DenseMap<const Type *, bool> &Cache, bool &IsPointer) {
  llvm::SmallPtrSet<const Type *, 8> Visited;
  return findNestedOwnerOrPointerOfIndirectionImpl(QT, Cache, Visited,
                                                   IsPointer);
}


// A lambda closure HOLDS a borrow when any of its captures is one: a
// by-reference capture (a reference field), a by-value capture of a
// pointer/view, a `this` capture (a pointer field), or a nested borrow-capturing
// closure. `isUnknownOwnershipType` treats every lambda as known-safe (line
// below) because a lambda *value* is modeled directly (its captures flow into
// the closure's origin). But when a closure is a *member* of an aggregate, that
// modeling does not reach it, and the aggregate's ownership is judged from its
// fields -- so the containing record must see the captured borrow. This inspects
// the capture fields the lambda exemption would otherwise skip.
static bool lambdaCapturesBorrow(const CXXRecordDecl *Lambda,
                                 llvm::DenseMap<const Type *, bool> &Cache) {
  for (const FieldDecl *F : Lambda->fields())
    if (fieldTypeHoldsBorrow(F->getType(), Cache))
      return true;
  return false;
}

bool isUnknownOwnershipType(QualType QT,
                            llvm::DenseMap<const Type *, bool> &Cache) {
  const CXXRecordDecl *RD = QT->getAsCXXRecordDecl();
  if (!RD)
    return false;
  // Annotated/recognized types have known semantics.
  if (isGslOwnerType(QT) || isGslPointerType(QT) || isStdCallableWrapperType(RD))
    return false;
  if (RD->isLambda())
    return false;

  const Type *Key = QT.getCanonicalType().getTypePtr();
  if (auto It = Cache.find(Key); It != Cache.end())
    return It->second;
  // Tentatively cache `false` to break any cyclic recursion.
  Cache[Key] = false;

  // An incomplete type cannot be inspected, so its ownership is conservatively
  // unknown.
  const CXXRecordDecl *Def = RD->getDefinition();
  bool Result = !Def;
  if (Def) {
    // The type's ownership is "unknown" if it (or a base) has a member that
    // holds a borrow the record cannot track through it (see
    // fieldTypeHoldsBorrow): a pointer/reference/view, a type-erased callable
    // wrapper, a closure capturing a borrow, or another unknown-ownership type.
    auto RecordHoldsBorrow = [&](const CXXRecordDecl *R) {
      for (const FieldDecl *F : R->fields())
        if (fieldTypeHoldsBorrow(F->getType(), Cache))
          return true;
      return false;
    };
    Result = RecordHoldsBorrow(Def);
    if (!Result)
      Def->forallBases([&](const CXXRecordDecl *Base) {
        if (RecordHoldsBorrow(Base)) {
          Result = true;
          return false; // Stop traversing bases.
        }
        return true;
      });
  }
  Cache[Key] = Result;
  return Result;
}

bool pointsToMutableOwner(QualType GslPointerType) {
  const CXXRecordDecl *RD = GslPointerType->getAsCXXRecordDecl();
  if (!RD || !RD->hasDefinition())
    return false;
  // A gsl::Pointer can mutate its pointee if it exposes mutable access to a
  // non-const owner: operator*/operator[] returning a non-const owner
  // reference, or operator-> returning a non-const owner pointer.
  auto ExposesMutableOwner = [](const CXXRecordDecl *R) {
    for (const CXXMethodDecl *M : R->methods()) {
      QualType Ret = M->getReturnType();
      switch (M->getOverloadedOperator()) {
      case OO_Star:
      case OO_Subscript:
        if (Ret->isLValueReferenceType() &&
            !Ret->getPointeeType().isConstQualified() &&
            isGslOwnerType(Ret->getPointeeType()))
          return true;
        break;
      case OO_Arrow:
        if (Ret->isPointerType() &&
            !Ret->getPointeeType().isConstQualified() &&
            isGslOwnerType(Ret->getPointeeType()))
          return true;
        break;
      default:
        break;
      }
    }
    return false;
  };
  if (ExposesMutableOwner(RD))
    return true;
  // The access operators may be inherited from base classes.
  bool Found = false;
  RD->forallBases([&](const CXXRecordDecl *Base) {
    if (ExposesMutableOwner(Base)) {
      Found = true;
      return false; // Stop traversing bases.
    }
    return true;
  });
  return Found;
}

bool isMutableOwnerType(QualType QT) {
  QT = QT.getNonReferenceType();
  while (QT->isArrayType())
    QT = QT->getAsArrayTypeUnsafe()->getElementType();
  return isGslOwnerType(QT) && !QT.isConstQualified();
}

bool recordContainsMutableOwner(
    const CXXRecordDecl *RD,
    llvm::SmallPtrSet<const CXXRecordDecl *, 8> &Visited) {
  if (!RD || !RD->hasDefinition())
    return false;
  if (!Visited.insert(RD->getCanonicalDecl()).second)
    return false;
  for (const CXXBaseSpecifier &B : RD->bases())
    if (recordContainsMutableOwner(B.getType()->getAsCXXRecordDecl(), Visited))
      return true;
  for (const FieldDecl *FD : RD->fields()) {
    QualType DeclT = FD->getType();
    if (isMutableOwnerType(DeclT))
      return true;
    // A non-const pointer/reference member whose pointee is (or contains) a
    // mutable owner: the owner can be reallocated through the indirection
    // (`v->push_back(...)`). A const pointee cannot be mutated, so it is
    // excluded.
    if (DeclT->isPointerType() || DeclT->isReferenceType()) {
      QualType Pointee = DeclT->getPointeeType();
      if (!Pointee.isConstQualified() &&
          (isMutableOwnerType(Pointee) ||
           recordContainsMutableOwner(Pointee->getAsCXXRecordDecl(), Visited)))
        return true;
    }
    // A gsl::Pointer member (e.g. an iterator) that exposes mutable access to a
    // non-const owner pointee.
    if (isGslPointerType(DeclT.getNonReferenceType()) &&
        pointsToMutableOwner(DeclT.getNonReferenceType()))
      return true;
    // Recurse into a non-owner record field (e.g. an aggregate sub-object that
    // itself holds an owner). Owners are leaves -- we never descend into them.
    QualType FT = DeclT.getNonReferenceType();
    while (FT->isArrayType())
      FT = FT->getAsArrayTypeUnsafe()->getElementType();
    if (!isGslOwnerType(FT) &&
        recordContainsMutableOwner(FT->getAsCXXRecordDecl(), Visited))
      return true;
  }
  return false;
}

bool recordHasGslOwnerField(QualType QT) {
  QT = QT.getNonReferenceType();
  // The implicit object argument of a member call is the `this` pointer
  // (type `S*`); peel it to reach the record.
  if (QT->isPointerType())
    QT = QT->getPointeeType();
  llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
  return recordContainsMutableOwner(QT->getAsCXXRecordDecl(), Visited);
}

static StringRef getName(const CXXRecordDecl &RD) {
  if (const auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(&RD))
    return CTSD->getSpecializedTemplate()->getName();
  if (RD.getIdentifier())
    return RD.getName();
  return "";
}

static StringRef getName(const FunctionDecl &FD) {
  if (FD.getIdentifier())
    return FD.getName();
  return "";
}

static bool isStdUniquePtr(const CXXRecordDecl &RD) {
  return RD.isInStdNamespace() && getName(RD) == "unique_ptr";
}

bool isUniquePtrRelease(const CXXMethodDecl &MD) {
  return MD.getIdentifier() && MD.getName() == "release" &&
         MD.getNumParams() == 0 && isStdUniquePtr(*MD.getParent());
}

bool isInvalidationMethod(const CXXMethodDecl &MD) {
  const CXXRecordDecl *RD = MD.getParent();
  if (!isInStlNamespace(RD))
    return false;

  // `pop_back` is excluded: it only invalidates references to the removed
  // element, not to other elements.
  static const llvm::StringSet<> Vector = {// Insertion
                                           "insert", "emplace", "emplace_back",
                                           "push_back", "insert_range",
                                           "append_range",
                                           // Removal
                                           "erase", "clear",
                                           // Memory management
                                           "reserve", "resize", "shrink_to_fit",
                                           // Assignment
                                           "assign", "assign_range"};

  // `pop_*` methods are excluded: they only invalidate references to the
  // removed element, not to other elements.
  static const llvm::StringSet<> Deque = {// Insertion
                                          "insert", "emplace", "insert_range",
                                          // Removal
                                          "erase", "clear",
                                          // Memory management
                                          "resize", "shrink_to_fit",
                                          // Assignment
                                          "assign", "assign_range"};

  static const llvm::StringSet<> String = {
      // Insertion
      "insert", "push_back", "append", "replace", "replace_with_range",
      "insert_range", "append_range",
      // Removal
      "pop_back", "erase", "clear",
      // Memory management
      "reserve", "resize", "resize_and_overwrite", "shrink_to_fit",
      // Assignment
      "swap", "assign", "assign_range"};

  // FIXME: Add queue and stack and check for underlying container
  // (e.g. no invalidation for std::list).
  static const llvm::StringSet<> PriorityQueue = {// Insertion
                                                  "push", "emplace",
                                                  "push_range",
                                                  // Removal
                                                  "pop"};

  // `erase` and `extract` are excluded: they only affect the removed element,
  // not to other elements.
  static const llvm::StringSet<> NodeBased = {// Removal
                                              "clear"};

  // For `flat_*` container adaptors, `try_emplace` and `insert_or_assign`
  // only exist on `flat_map`. Listing them here is harmless since the methods
  // won't be found on other types.
  static const llvm::StringSet<> Flat = {// Insertion
                                         "insert", "emplace", "emplace_hint",
                                         "try_emplace", "insert_or_assign",
                                         "insert_range", "merge",
                                         // Removal
                                         "extract", "erase", "clear",
                                         // Assignment
                                         "replace"};

  static const llvm::StringSet<> UniquePtr = {// Reallocation
                                              "reset"};

  const StringRef RecordName = getName(*RD);
  // TODO: Consider caching this lookup by CXXMethodDecl pointer if this
  // StringSwitch becomes a performance bottleneck.
  const llvm::StringSet<> *InvalidatingMethods =
      llvm::StringSwitch<const llvm::StringSet<> *>(RecordName)
          .Case("vector", &Vector)
          .Case("basic_string", &String)
          .Case("deque", &Deque)
          .Case("priority_queue", &PriorityQueue)
          .Cases({"set", "multiset", "map", "multimap", "unordered_set",
                  "unordered_multiset", "unordered_map", "unordered_multimap"},
                 &NodeBased)
          .Cases({"flat_map", "flat_set", "flat_multimap", "flat_multiset"},
                 &Flat)
          .Case("unique_ptr", &UniquePtr)
          .Default(nullptr);

  if (!InvalidatingMethods)
    return false;

  // Handle Operators via OverloadedOperatorKind
  OverloadedOperatorKind OO = MD.getOverloadedOperator();
  if (OO != OO_None) {
    switch (OO) {
    case OO_Equal:     // operator= : Always invalidates (Assignment)
    case OO_PlusEqual: // operator+= : Append (String/Vector)
      return true;
    case OO_Subscript: // operator[] : Invalidation only for
                       // `flat_map` (Insert-or-access).
                       // `map` and `unordered_map` are excluded.
      return RecordName == "flat_map";
    default:
      return false;
    }
  }

  if (!MD.getIdentifier())
    return false;

  return InvalidatingMethods->contains(MD.getName());
}

bool isStlContainerInsertionMethod(const CXXMethodDecl &MD) {
  const CXXRecordDecl *RD = MD.getParent();
  if (!isInStlNamespace(RD) || !MD.getIdentifier())
    return false;
  // Insertion methods take the element by const reference or rvalue reference
  // and copy/move it into the container; the reference parameter does not
  // escape. This is a curated name-based heuristic across STL containers.
  static const llvm::StringSet<> Insertion = {
      "push_back",          "push_front",  "emplace_back",
      "emplace_front",      "push",        "insert",
      "emplace",            "insert_or_assign", "try_emplace",
      "append",             "assign",      "insert_range",
      "append_range",       "prepend_range", "push_range",
      "replace",            "replace_with_range", "insert_after",
      "emplace_after",      "merge",       "assign_range"};
  return Insertion.contains(MD.getName());
}

// Returns true if \p RD is a std::basic_string specialization.
static bool isStlBasicStringType(const CXXRecordDecl *RD) {
  return RD && isInStlNamespace(RD) && getName(*RD) == "basic_string";
}

bool isStlStringConcatenationOperator(const FunctionDecl &FD) {
  if (const auto *MD = dyn_cast<CXXMethodDecl>(&FD))
    // Member `s += x`: appends a copy of x's characters; x does not escape.
    return MD->getOverloadedOperator() == OO_PlusEqual &&
           isStlBasicStringType(MD->getParent());
  // Free `a + b`: builds a new string from copies of its operands. Recognized by
  // a basic_string result in the STL namespace (covers the string/char*/char
  // overloads); iterator `operator+` returns an iterator, not a string.
  return FD.getOverloadedOperator() == OO_Plus && isInStlNamespace(&FD) &&
         isStlBasicStringType(FD.getReturnType()->getAsCXXRecordDecl());
}

bool isStlStringMemberCall(const FunctionDecl *FD) {
  // CXXConstructorDecl is-a CXXMethodDecl, so this also covers the
  // basic_string(const char*/string_view/...) constructors.
  const auto *MD = dyn_cast<CXXMethodDecl>(FD);
  return MD && isStlBasicStringType(MD->getParent());
}

bool isStringSourceType(QualType T) {
  T = T.getNonReferenceType();
  if (T->isPointerType() && T->getPointeeType()->isCharType())
    return true; // const char*
  const CXXRecordDecl *RD = T->getAsCXXRecordDecl();
  if (!RD || !isInStlNamespace(RD))
    return false;
  StringRef Name = getName(*RD);
  return Name == "basic_string" || Name == "basic_string_view";
}

bool isStlContainerType(const CXXRecordDecl *RD) {
  if (!RD || !isInStlNamespace(RD))
    return false;
  // Curated set of standard container templates. A constructor of one of these
  // builds the container by copying/moving the elements it is given in, so a
  // by-(const-)reference parameter whose referent cannot itself hold a borrow
  // does not escape.
  static const llvm::StringSet<> Containers = {
      "vector",         "deque",
      "list",           "forward_list",
      "array",          "map",
      "multimap",       "set",
      "multiset",       "unordered_map",
      "unordered_multimap", "unordered_set",
      "unordered_multiset", "basic_string",
      "stack",          "queue",
      "priority_queue", "flat_map",
      "flat_multimap",  "flat_set",
      "flat_multiset"};
  return Containers.contains(getName(*RD));
}

bool destructsFirstArg(const FunctionDecl &FD) {
  if (isa<CXXDestructorDecl>(FD))
    return true;
  // `std::destroy_at(p)` runs p's destructor.
  if (isInStlNamespace(&FD) && getName(FD) == "destroy_at")
    return true;
  // A direct call to an `operator delete` / `operator delete[]` deallocation
  // function frees its first argument (a `delete`/`delete[]` *expression* is a
  // CXXDeleteExpr, handled separately; this is the explicit-call form, e.g.
  // `::operator delete(p)`). The `__builtin_operator_delete` builtin is the
  // same deallocation but is not spelled as an overloaded operator.
  OverloadedOperatorKind OO = FD.getOverloadedOperator();
  if (OO == OO_Delete || OO == OO_Array_Delete)
    return true;
  if (FD.getBuiltinID() == Builtin::BI__builtin_operator_delete)
    return true;
  // The C deallocators `free`/`realloc` (and the BSD/macOS variants
  // `reallocf`/`cfree`) free their first argument. Recognize them at global (the
  // usual `extern "C"` declaration) or std scope. `realloc`/`reallocf`
  // additionally return a fresh allocation, but invalidating the old pointer is
  // what matters for borrows into it.
  if (getName(FD) == "free" || getName(FD) == "realloc" ||
      getName(FD) == "reallocf" || getName(FD) == "cfree") {
    const DeclContext *DC = FD.getDeclContext()->getRedeclContext();
    return DC->isTranslationUnit() || isInStlNamespace(&FD);
  }
  return false;
}

bool isStdCallableWrapperType(const CXXRecordDecl *RD) {
  if (!RD || !isInStlNamespace(RD))
    return false;
  StringRef Name = getName(*RD);
  return Name == "function" || Name == "move_only_function";
}

bool isStdReferenceCast(const FunctionDecl *FD) {
  if (!FD)
    return false;
  switch (FD->getBuiltinID()) {
  case Builtin::BImove:
  case Builtin::BImove_if_noexcept:
  case Builtin::BIforward:
  case Builtin::BIforward_like:
  case Builtin::BIas_const:
    return true;
  default:
    return false;
  }
}

} // namespace clang::lifetimes
