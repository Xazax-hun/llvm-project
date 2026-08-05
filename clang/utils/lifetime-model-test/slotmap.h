// slotmap.h - a fixed-capacity, generation-tagged object pool.
//
// The project's one hand-written owner container (SELF_CONTAINED, i.e.
// [[gsl::Owner]]; see annotations.h), exercising the safe programming model on:
//
//   * user-defined ownership (SELF_CONTAINED on a class template),
//   * returning borrows into the owner via `[[clang::lifetimebound]]` on `this`,
//   * stable cross-frame references expressed as integer *handles* rather than
//     pointers (so nothing is ever a `T*` stored in a container).
//
// Storage is backed by std::vector rather than raw new[]/delete[]. That is not
// just convenience: the safe model's lifetimebound *validation* can only verify
// that `at()`/`find()` return a borrow tied to `this` when the borrow flows
// through a well-modeled owner (std::vector's annotated operator[]/data()). A
// raw `T*` member carries no such provenance, so returning `data_[i]` from it is
// rejected by -Wlifetime-safety-lifetimebound-violation. See NOTES.md.
//
// Capacity is fixed at construction, so the vectors allocate once and the
// per-frame hot path stays allocation-free.
#ifndef LMT_SLOTMAP_H
#define LMT_SLOTMAP_H

#include <cstdint>
#include <type_traits>
#include <vector>

#include "annotations.h"

LIFETIME_SAFE_START

namespace ast {

// A handle is a value type holding two integers. It *cannot* hold a borrow, so
// it needs no ownership annotation and can be freely stored in containers,
// copied, and returned -- which is the entire point of using handles instead of
// pointers under the safe model.
struct SlotHandle {
  std::uint32_t index = 0;
  std::uint32_t gen = 0; // 0 == never-valid sentinel

  bool valid() const { return gen != 0; }
  friend bool operator==(SlotHandle a, SlotHandle b) {
    return a.index == b.index && a.gen == b.gen;
  }
};

template <class T>
struct SELF_CONTAINED SlotMap {
  static_assert(std::is_trivially_copyable_v<T>,
                "SlotMap stores PODs by value; entities are trivially copyable");

  explicit SlotMap(std::uint32_t capacity)
      : data_(capacity), gens_(capacity, 0u), freeList_(capacity),
        cap_(capacity) {
    // Slot i is free; gen 0 (even) means "free, never handed out".
    for (std::uint32_t i = 0; i < cap_; ++i)
      freeList_[i] = cap_ - 1 - i; // pop low indices first
    freeCount_ = cap_;
  }

  // Owner: single ownership, no copies. (Move intentionally omitted -- moving an
  // owner is unmodeled by the analysis; we never need to move the pool.)
  SlotMap(const SlotMap &) = delete;
  SlotMap &operator=(const SlotMap &) = delete;

  std::uint32_t capacity() const { return cap_; }
  std::uint32_t size() const { return cap_ - freeCount_; }
  bool full() const { return freeCount_ == 0; }

  // A slot's parity encodes liveness: odd gen == alive, even gen == free.
  bool aliveAt(std::uint32_t i) const { return (gens_[i] & 1u) != 0u; }

  bool contains(SlotHandle h) const {
    return h.index < cap_ && h.gen != 0 && gens_[h.index] == h.gen;
  }

  // Insert by value (PODs are cheap to copy and this sidesteps any question of
  // a reference parameter escaping into storage). Returns an invalid handle if
  // the pool is full -- the caller drops the spawn rather than growing.
  SlotHandle insert(T value) {
    if (freeCount_ == 0)
      return SlotHandle{};
    std::uint32_t i = freeList_[--freeCount_];
    gens_[i] += 1; // even -> odd: now alive, new generation
    data_[i] = value;
    return SlotHandle{i, gens_[i]};
  }

  void erase(SlotHandle h) {
    if (!contains(h))
      return;
    gens_[h.index] += 1; // odd -> even: now free, generation bumped
    freeList_[freeCount_++] = h.index;
  }

  // Erase by raw slot index (the broad-phase grid hands back indices, not
  // handles). No-op if the slot is already free or out of range.
  void eraseAt(std::uint32_t i) {
    if (i >= cap_ || (gens_[i] & 1u) == 0u)
      return;
    gens_[i] += 1; // odd -> even
    freeList_[freeCount_++] = i;
  }

  // Borrows into the pool. lifetimebound on `this`: the returned reference is
  // valid only as long as the pool is. Index form is used by the hot iteration
  // loop (caller guards with aliveAt); handle form is the safe random-access
  // path that validates the generation.
  // PRESERVES_BORROWS: handing out a reference does not reallocate the pool, so
  // borrows taken from it (including this one) stay valid across the call.
  PRESERVES_BORROWS T &at(std::uint32_t i) [[clang::lifetimebound]] {
    return data_[i];
  }
  const T &at(std::uint32_t i) const [[clang::lifetimebound]] {
    return data_[i];
  }

  // Returns nullptr for a stale/invalid handle. Pointer is bound to `this`.
  PRESERVES_BORROWS T *find(SlotHandle h) [[clang::lifetimebound]] {
    return contains(h) ? &data_[h.index] : nullptr;
  }
  const T *find(SlotHandle h) const [[clang::lifetimebound]] {
    return contains(h) ? &data_[h.index] : nullptr;
  }

private:
  std::vector<T> data_;               // cap_ elements
  std::vector<std::uint32_t> gens_;   // per-slot generation (parity = liveness)
  std::vector<std::uint32_t> freeList_; // stack of free slot indices
  std::uint32_t cap_ = 0;
  std::uint32_t freeCount_ = 0;
};

} // namespace ast

LIFETIME_SAFE_END

#endif // LMT_SLOTMAP_H
