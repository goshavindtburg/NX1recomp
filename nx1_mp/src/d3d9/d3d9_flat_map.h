/**
 * @file    d3d9_flat_map.h
 * @brief   Open-addressing uint64 -> V map, a drop-in for the renderer's hot std::unordered_map.
 *
 * The resource caches are probed ~21k times a frame (every bound sampler slot of every draw), and
 * the texture cache alone holds several thousand entries. std::unordered_map is node-based: each
 * probe hashes, indexes a bucket array, then chases a pointer into a separately-allocated node
 * that is almost never in cache. Measured at ~220 ns per GetTexture call, that pointer chase --
 * not the work it guards -- was the largest remaining cost in the texture phase.
 *
 * This stores keys in one dense array and values in another. A probe touches 8 keys per cache
 * line and only reaches the value array on a hit, which is the whole point: the scan stays hot
 * even when the values (a TextureEntry is ~80 bytes) do not.
 *
 * Keys are already hash-mixed by MixKey at the call sites, but raw guest addresses are also used
 * as keys and those are page-aligned -- their low bits are structural, not random. Every key is
 * therefore finalized again here so the probe sequence does not clump.
 *
 * API is the subset the callers use: find/operator[]/erase(iterator)/begin/end/size/clear.
 *
 * NOTE ON REFERENCE INVALIDATION -- the one behavioural difference from unordered_map, and it is
 * a real hazard rather than a technicality. unordered_map guarantees references to elements stay
 * valid across unrelated insertions; this does NOT, because a rehash moves every value. Holding a
 * `V&` from operator[] across another insertion into the SAME map is undefined here and fine
 * there. GetTexture holds exactly such a reference (`auto& entry = (*map)[key]`) across a long
 * stretch of work, which was audited to contain no further access to that map. Preserve that when
 * editing, or re-index instead of holding the reference.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

namespace nx1::d3d9 {

template <typename V>
class FlatMap {
 public:
  FlatMap() { Rehash(64); }

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  /// What iteration and find() hand back. Mirrors the `.first` / `.second` of a map value_type,
  /// but as references into the two parallel arrays rather than a stored pair.
  struct Ref {
    const uint64_t& first;
    V& second;
  };

  class iterator {
   public:
    iterator(FlatMap* m, size_t i) : m_(m), i_(i) { Skip(); }

    Ref operator*() const { return Ref{m_->keys_[i_], m_->vals_[i_]}; }
    /// operator-> needs something with a lifetime, so materialise the Ref and hand back its
    /// address; the proxy lives until the end of the full expression, which is all `it->second`
    /// needs.
    struct Arrow {
      Ref r;
      Ref* operator->() { return &r; }
    };
    Arrow operator->() const { return Arrow{**this}; }

    /// So std::next and friends work on it (the vertex-hash sweep uses std::next).
    using iterator_category = std::forward_iterator_tag;
    using value_type = Ref;
    using difference_type = std::ptrdiff_t;
    using pointer = Arrow;
    using reference = Ref;

    iterator& operator++() {
      ++i_;
      Skip();
      return *this;
    }
    bool operator==(const iterator& o) const { return i_ == o.i_; }
    bool operator!=(const iterator& o) const { return i_ != o.i_; }
    size_t index() const { return i_; }

   private:
    void Skip() {
      while (i_ < m_->cap_ && m_->state_[i_] != kFull) ++i_;
    }
    FlatMap* m_;
    size_t i_;
  };

  iterator begin() { return iterator(this, 0); }
  iterator end() { return iterator(this, cap_); }

  iterator find(uint64_t key) {
    const uint64_t k = Key(key);
    size_t i = Slot(k);
    for (;;) {
      const uint8_t s = state_[i];
      if (s == kEmpty) return end();
      if (s == kFull && keys_[i] == k) return iterator(this, i);
      i = (i + 1) & mask_;
    }
  }

  V& operator[](uint64_t key) {
    // Rehash on total occupancy, tombstones included: a map that is erased from as heavily as it
    // is inserted into (the age sweep evicts thousands) would otherwise fill with tombstones and
    // degrade to a linear scan while `size_` still looked small.
    if ((used_ + 1) * 4 >= cap_ * 3) {
      Rehash(size_ * 4 >= cap_ ? cap_ * 2 : cap_);
    }
    const uint64_t k = Key(key);
    size_t i = Slot(k);
    size_t tomb = kNoSlot;
    for (;;) {
      const uint8_t s = state_[i];
      if (s == kEmpty) break;
      if (s == kTomb) {
        if (tomb == kNoSlot) tomb = i;
      } else if (keys_[i] == k) {
        return vals_[i];
      }
      i = (i + 1) & mask_;
    }
    if (tomb != kNoSlot) {
      i = tomb;  // reuse a tombstone: does not grow occupancy
    } else {
      ++used_;
    }
    keys_[i] = k;
    state_[i] = kFull;
    vals_[i] = V();
    ++size_;
    return vals_[i];
  }

  /// Erase and return an iterator to the next live element, so the
  /// `it = map->erase(it)` sweep loops keep working unchanged.
  iterator erase(iterator it) {
    const size_t i = it.index();
    state_[i] = kTomb;
    vals_[i] = V();
    --size_;
    return iterator(this, i + 1);
  }

  void clear() {
    state_.assign(cap_, kEmpty);
    vals_.assign(cap_, V());
    size_ = 0;
    used_ = 0;
  }

 private:
  static constexpr uint8_t kEmpty = 0;
  static constexpr uint8_t kFull = 1;
  static constexpr uint8_t kTomb = 2;
  static constexpr size_t kNoSlot = ~size_t(0);

  /// Key 0 is legal for callers but would be indistinguishable from an unused slot if it were the
  /// sentinel, so slot state lives in its own array and keys are stored verbatim. This only
  /// finalizes: page-aligned guest addresses share low bits and must not probe the same slots.
  static uint64_t Key(uint64_t k) {
    k ^= k >> 33;
    k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 29;
    return k;
  }
  size_t Slot(uint64_t k) const { return size_t(k) & mask_; }

  void Rehash(size_t new_cap) {
    if (new_cap < 64) new_cap = 64;
    std::vector<uint64_t> old_keys;
    std::vector<uint8_t> old_state;
    std::vector<V> old_vals;
    old_keys.swap(keys_);
    old_state.swap(state_);
    old_vals.swap(vals_);

    cap_ = new_cap;
    mask_ = cap_ - 1;
    keys_.assign(cap_, 0);
    state_.assign(cap_, kEmpty);
    vals_.assign(cap_, V());
    size_ = 0;
    used_ = 0;

    for (size_t j = 0; j < old_state.size(); ++j) {
      if (old_state[j] != kFull) continue;  // tombstones are dropped here
      const uint64_t k = old_keys[j];
      size_t i = Slot(k);
      while (state_[i] != kEmpty) i = (i + 1) & mask_;
      keys_[i] = k;
      state_[i] = kFull;
      vals_[i] = static_cast<V&&>(old_vals[j]);
      ++size_;
      ++used_;
    }
  }

  std::vector<uint64_t> keys_;
  std::vector<uint8_t> state_;
  std::vector<V> vals_;
  size_t cap_ = 0;
  size_t mask_ = 0;
  size_t size_ = 0;
  size_t used_ = 0;  ///< full + tombstoned slots
};

}  // namespace nx1::d3d9
