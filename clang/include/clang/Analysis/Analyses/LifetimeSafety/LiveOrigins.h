//===- LiveOrigins.h - Live Origins Analysis -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the LiveOriginAnalysis, a backward dataflow analysis that
// determines which origins are "live" at each program point. An origin is
// "live" at a program point if there's a potential future use of a pointer it
// is associated with. Liveness is "generated" by a use of an origin (e.g., a
// `UseFact` from a read of a pointer) and is "killed" (i.e., it stops being
// live) when the origin is replaced by flowing a different origin into it
// (e.g., an OriginFlow from an assignment that kills the destination).
//
// This information is used for detecting use-after-free errors, as it allows us
// to check if a live origin holds a loan to an object that has already expired.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_LIVE_ORIGINS_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_LIVE_ORIGINS_H

#include "clang/Analysis/Analyses/LifetimeSafety/Facts.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Origins.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Utils.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/Debug.h"

namespace clang::lifetimes::internal {

using CausingFactType =
    ::llvm::PointerUnion<const UseFact *, const OriginEscapesFact *>;

enum class LivenessKind : uint8_t {
  Dead,  // Not alive
  Maybe, // Live on some path but not all paths (may-be-live)
  Must   // Live on all paths (must-be-live)
};

/// Information about why an origin is live at a program point.
struct LivenessInfo {
  /// The use that makes the origin live. If liveness is propagated from
  /// multiple uses along different paths, this will point to the use appearing
  /// earlier in the translation unit.
  /// This is 'null' when the origin is not live.
  CausingFactType CausingFact;

  /// The kind of liveness of the origin.
  /// `Must`: The origin is live on all control-flow paths from the current
  /// point to the function's exit (i.e. the current point is dominated by a set
  /// of uses).
  /// `Maybe`: indicates it is live on some but not all paths.
  ///
  /// This determines the diagnostic's confidence level.
  /// `Must`-be-alive at expiration implies a definite use-after-free,
  /// while `Maybe`-be-alive suggests a potential one on some paths.
  LivenessKind Kind;

  /// Whether some use FORWARD of this point FOLLOWS the pointer (`*p`, `p->m`,
  /// `p[i]`, `p->method()`) rather than only reading its value (`p == q`,
  /// `if (p)`, `++p`). An escape counts, since it hands the pointer somewhere
  /// that may follow it.
  ///
  /// Accumulated over every forward use, joined by OR, because `CausingFact` is
  /// only the *nearest* use: a harmless one between a lifetime-ending event and a
  /// real dereference would otherwise answer for both.
  ///
  /// KNOWN: because the anchor is still `CausingFact`, the "later used here" note
  /// points at that nearest use -- the increment in
  /// `for (S *c = b; c != e; ++c) c->~S();` -- rather than at the dereference
  /// that makes it an error. Carrying the following use here to anchor with
  /// instead lost 10 corpus reports and was reverted; it needs its own look.
  ///
  /// Consulted ONLY where an object's lifetime ended while its STORAGE SURVIVES
  /// (an explicit destructor call) -- the one case where merely holding the
  /// pointer afterwards is still fine. Where the storage itself is gone, holding
  /// the borrow at all is the error and every use reports, so nothing asks.
  ///
  /// NOTHING CLEARS IT. Pointer arithmetic used to, on the theory that a
  /// dereference after `++p` reaches a different element and bounds are out of
  /// scope. That assumes the new element is valid, which is not known -- after
  /// `delete p` the whole block is gone, and `++p; --p;` nets to zero -- and
  /// every version of the assumption produced a hole. Arithmetic now merely
  /// fails to set it, which is enough to keep a bare `p->~S(); ++p;` quiet.
  bool FollowedSameTarget = false;

  LivenessInfo() : CausingFact(nullptr), Kind(LivenessKind::Dead) {}
  LivenessInfo(CausingFactType CF, LivenessKind K, bool FollowedSameTarget)
      : CausingFact(CF), Kind(K), FollowedSameTarget(FollowedSameTarget) {}

  bool operator==(const LivenessInfo &Other) const {
    return CausingFact == Other.CausingFact && Kind == Other.Kind &&
           FollowedSameTarget == Other.FollowedSameTarget;
  }
  bool operator!=(const LivenessInfo &Other) const { return !(*this == Other); }

  void Profile(llvm::FoldingSetNodeID &IDBuilder) const {
    IDBuilder.AddPointer(CausingFact.getOpaqueValue());
    IDBuilder.Add(Kind);
    IDBuilder.AddBoolean(FollowedSameTarget);
  }
};

using LivenessMap = utils::MapTy<OriginID, LivenessInfo>;

/// The live origins at a program point, in the two halves the analysis keeps
/// apart: those that cross block boundaries and those confined to one block.
///
/// Deliberately not iterable as one range: a block-local origin can still be
/// live at a point inside its own block, so a consumer that visited only
/// `Persistent` would silently lose reports. Use `allLive()` unless you have a
/// reason to name a half.
struct LiveOriginSet {
  LivenessMap Persistent;
  LivenessMap BlockLocal;

  /// Every live origin, from both halves. This is what a consumer asking
  /// "what is live here?" wants.
  llvm::SmallVector<std::pair<OriginID, LivenessInfo>> allLive() const {
    llvm::SmallVector<std::pair<OriginID, LivenessInfo>> Result;
    for (const LivenessMap &Live : {Persistent, BlockLocal})
      for (const auto &[OID, Info] : Live)
        Result.emplace_back(OID, Info);
    return Result;
  }
};

class LiveOriginsAnalysis {
public:
  LiveOriginsAnalysis(const CFG &C, AnalysisDeclContext &AC, FactManager &F,
                      LivenessMap::Factory &SF);
  ~LiveOriginsAnalysis();

  /// Returns the set of origins that are live at a specific program point,
  /// along with the the details of the liveness.
  LiveOriginSet getLiveOriginsAt(ProgramPoint P) const;

  // Dump liveness values on all test points in the program.
  void dump(llvm::raw_ostream &OS,
            const llvm::StringMap<ProgramPoint> &TestPoints) const;

private:
  class Impl;
  std::unique_ptr<Impl> PImpl;
};

} // namespace clang::lifetimes::internal

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_LIVE_ORIGINS_H
