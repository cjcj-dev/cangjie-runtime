// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_BASE_BITMAP_H
#define MRT_BASE_BITMAP_H

// HotSpot utilities/bitMap.hpp + bitMap.inline.hpp, restricted to the surface
// the ZGC port consumes (zBitMap / zLiveMap / zRememberedSet): word view,
// par_at / par_set_bit, range set/clear, first/last set-bit search, forward
// iteration, ReverseIterator and the CHeap-owning flavour.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"

namespace MapleRuntime {

class BitMap {
public:
    typedef size_t idx_t;        // Type used for bit and word indices.
    typedef uintptr_t bm_word_t; // Element type of the word array.

    static constexpr idx_t BitsPerByte = 8;
    static constexpr idx_t BitsPerWord = sizeof(bm_word_t) * BitsPerByte;
    static constexpr idx_t LogBitsPerWord = 6;
    static_assert((idx_t(1) << LogBitsPerWord) == BitsPerWord, "bm_word_t must be 64 bits");

    // Threshold for performing small range operation, even when large range
    // operation was requested. Measured in words.
    static constexpr size_t small_range_words = 32;


protected:
    bm_word_t* _map; // First word in bitmap
    idx_t _size;     // Size of bitmap (in bits)

    static idx_t raw_to_words_align_up(idx_t bit) { return raw_to_words_align_down(bit + (BitsPerWord - 1)); }
    static idx_t raw_to_words_align_down(idx_t bit) { return bit >> LogBitsPerWord; }
    static idx_t to_words_align_up(idx_t bit) { return raw_to_words_align_up(bit); }
    static idx_t to_words_align_down(idx_t bit) { return raw_to_words_align_down(bit); }

    static bool is_small_range_of_words(idx_t beg_full_word, idx_t end_full_word)
    {
        return end_full_word - beg_full_word < small_range_words;
    }

    // Return the position of bit within the word that contains it.
    static idx_t bit_in_word(idx_t bit) { return bit & (BitsPerWord - 1); }

    // Return a mask that will select the specified bit, when applied to the word containing the bit.
    static bm_word_t bit_mask(idx_t bit) { return (bm_word_t)1 << bit_in_word(bit); }

    // Return the bit number of the first bit in the specified word.
    static idx_t bit_index(idx_t word) { return word << LogBitsPerWord; }

    // Return the array of bitmap words, or a specific word from it.
    bm_word_t* map() { return _map; }
    const bm_word_t* map() const { return _map; }

    // Return a pointer to the word containing the specified bit.
    bm_word_t* word_addr(idx_t bit) { return map() + to_words_align_down(bit); }
    const bm_word_t* word_addr(idx_t bit) const { return map() + to_words_align_down(bit); }

    void set_word(idx_t word, bm_word_t val) { _map[word] = val; }
    void clear_word(idx_t word) { _map[word] = 0; }

    static bm_word_t load_word_ordered(const volatile bm_word_t* const addr, std::memory_order memory_order)
    {
        if (memory_order == std::memory_order_relaxed || memory_order == std::memory_order_release) {
            return __atomic_load_n(addr, __ATOMIC_RELAXED);
        }
        return __atomic_load_n(addr, __ATOMIC_ACQUIRE);
    }

    // Ranges within a single word.
    bm_word_t inverted_bit_mask_for_range(idx_t beg, idx_t end) const
    {
        DCHECK(end != 0);
        DCHECK(beg == end || to_words_align_down(beg) == to_words_align_down(end - 1));
        // Note that mask = ~(2^(end - beg) - 1) << bit_in_word(beg) would be
        // wrong when end - beg == BitsPerWord.
        bm_word_t mask = bit_mask(beg) - 1; // low (right) bits
        if (bit_in_word(end) != 0) {
            mask |= ~(bit_mask(end) - 1); // high (left) bits
        }
        return mask;
    }
    void set_range_within_word(idx_t beg, idx_t end)
    {
        if (beg != end) {
            bm_word_t mask = inverted_bit_mask_for_range(beg, end);
            *word_addr(beg) |= ~mask;
        }
    }
    void clear_range_within_word(idx_t beg, idx_t end)
    {
        if (beg != end) {
            bm_word_t mask = inverted_bit_mask_for_range(beg, end);
            *word_addr(beg) &= mask;
        }
    }

    // Ranges spanning entire words.
    void set_range_of_words(idx_t beg, idx_t end)
    {
        for (idx_t i = beg; i < end; ++i) _map[i] = ~(bm_word_t)0;
    }
    void clear_range_of_words(idx_t beg, idx_t end) { clear_range_of_words(_map, beg, end); }
    static void clear_range_of_words(bm_word_t* map, idx_t beg, idx_t end)
    {
        for (idx_t i = beg; i < end; ++i) map[i] = 0;
    }
    void clear_large_range_of_words(idx_t beg, idx_t end)
    {
        if (beg < end) {
            std::memset(_map + beg, 0, (end - beg) * sizeof(bm_word_t));
        }
    }

    // Set the map and size.
    void update(bm_word_t* map, idx_t size)
    {
        _map = map;
        _size = size;
    }

    // Protected constructor and destructor.
    BitMap(bm_word_t* map, idx_t size_in_bits) : _map(map), _size(size_in_bits) {}
    ~BitMap() {}

public:
    static idx_t calc_size_in_words(size_t size_in_bits) { return raw_to_words_align_up(size_in_bits); }

    idx_t size() const { return _size; }
    idx_t size_in_words() const { return calc_size_in_words(size()); }
    idx_t size_in_bytes() const { return size_in_words() * sizeof(bm_word_t); }

    void verify_index(idx_t bit) const { DCHECK_D(bit < _size, "BitMap index out of bounds"); }
    void verify_range(idx_t beg, idx_t end) const
    {
        DCHECK_D(beg <= end, "BitMap range error");
        DCHECK_D(end <= _size, "BitMap range error");
    }

    bool at(idx_t index) const
    {
        verify_index(index);
        return (*word_addr(index) & bit_mask(index)) != 0;
    }

    // memory_order must be memory_order_relaxed or memory_order_acquire.
    bool par_at(idx_t index, std::memory_order memory_order = std::memory_order_acquire) const
    {
        verify_index(index);
        const volatile bm_word_t* const addr = word_addr(index);
        return (load_word_ordered(addr, memory_order) & bit_mask(index)) != 0;
    }

    void set_bit(idx_t bit)
    {
        verify_index(bit);
        *word_addr(bit) |= bit_mask(bit);
    }
    void clear_bit(idx_t bit)
    {
        verify_index(bit);
        *word_addr(bit) &= ~bit_mask(bit);
    }

    // Atomically set the bit; returns true if this thread changed the value.
    bool par_set_bit(idx_t bit, std::memory_order memory_order = std::memory_order_seq_cst)
    {
        verify_index(bit);
        volatile bm_word_t* const addr = word_addr(bit);
        const bm_word_t mask = bit_mask(bit);
        bm_word_t old_val = load_word_ordered(addr, memory_order);

        do {
            const bm_word_t new_val = old_val | mask;
            if (new_val == old_val) {
                return false; // Someone else beat us to it.
            }
            bm_word_t expected = old_val;
            if (__atomic_compare_exchange_n(addr, &expected, new_val, false, static_cast<int>(memory_order),
                                            __ATOMIC_RELAXED)) {
                return true; // Success.
            }
            old_val = expected; // The value changed, try again.
        } while (true);
    }

    bool par_clear_bit(idx_t bit, std::memory_order memory_order = std::memory_order_seq_cst)
    {
        verify_index(bit);
        volatile bm_word_t* const addr = word_addr(bit);
        const bm_word_t mask = ~bit_mask(bit);
        bm_word_t old_val = load_word_ordered(addr, memory_order);

        do {
            const bm_word_t new_val = old_val & mask;
            if (new_val == old_val) {
                return false;
            }
            bm_word_t expected = old_val;
            if (__atomic_compare_exchange_n(addr, &expected, new_val, false, static_cast<int>(memory_order),
                                            __ATOMIC_RELAXED)) {
                return true;
            }
            old_val = expected;
        } while (true);
    }

    // Update a range of bits. Ranges are half-open [beg, end).
    void set_range(idx_t beg, idx_t end)
    {
        verify_range(beg, end);
        idx_t beg_full_word = to_words_align_up(beg);
        idx_t end_full_word = to_words_align_down(end);
        if (beg_full_word < end_full_word) {
            // The range includes at least one full word.
            set_range_within_word(beg, bit_index(beg_full_word));
            set_range_of_words(beg_full_word, end_full_word);
            set_range_within_word(bit_index(end_full_word), end);
        } else {
            // The range spans at most 2 partial words.
            idx_t boundary = bit_index(beg_full_word) < end ? bit_index(beg_full_word) : end;
            set_range_within_word(beg, boundary);
            set_range_within_word(boundary, end);
        }
    }

    void clear_range(idx_t beg, idx_t end)
    {
        verify_range(beg, end);
        idx_t beg_full_word = to_words_align_up(beg);
        idx_t end_full_word = to_words_align_down(end);
        if (beg_full_word < end_full_word) {
            clear_range_within_word(beg, bit_index(beg_full_word));
            clear_range_of_words(beg_full_word, end_full_word);
            clear_range_within_word(bit_index(end_full_word), end);
        } else {
            idx_t boundary = bit_index(beg_full_word) < end ? bit_index(beg_full_word) : end;
            clear_range_within_word(beg, boundary);
            clear_range_within_word(boundary, end);
        }
    }

    void clear_large_range(idx_t beg, idx_t end)
    {
        verify_range(beg, end);
        idx_t beg_full_word = to_words_align_up(beg);
        idx_t end_full_word = to_words_align_down(end);
        if (is_small_range_of_words(beg_full_word, end_full_word)) {
            clear_range(beg, end);
            return;
        }
        // The range includes at least one full word.
        clear_range_within_word(beg, bit_index(beg_full_word));
        clear_large_range_of_words(beg_full_word, end_full_word);
        clear_range_within_word(bit_index(end_full_word), end);
    }

    // Clearing
    void clear() { clear_range_of_words(0, size_in_words()); }
    void clear_large() { clear_large_range_of_words(0, size_in_words()); }

    // Return the index of the first set bit in the range [beg, end), or end if none found.
    idx_t find_first_set_bit(idx_t beg, idx_t end) const
    {
        verify_range(beg, end);
        if (beg < end) {
            // Get the word containing beg, and shift out low bits.
            idx_t word_index = to_words_align_down(beg);
            bm_word_t cword = _map[word_index] >> bit_in_word(beg);
            if ((cword & 1) != 0) { // Test the beg bit.
                return beg;
            }
            // Position of bit0 of cword in the bitmap. Initially for shifted first word.
            idx_t cword_pos = beg;
            if (cword == 0) { // Test other bits in the first word.
                idx_t word_limit = to_words_align_up(end);
                while (++word_index < word_limit) {
                    cword = _map[word_index];
                    if (cword != 0) {
                        cword_pos = bit_index(word_index);
                        break;
                    }
                }
            }
            if (cword != 0) {
                idx_t result = cword_pos + static_cast<idx_t>(__builtin_ctzl(cword));
                if (result < end) return result;
            }
        }
        return end;
    }
    idx_t find_first_set_bit(idx_t beg) const { return find_first_set_bit(beg, size()); }

    // Return the index of the last set bit in the range [beg, end), or end if none found.
    idx_t find_last_set_bit(idx_t beg, idx_t end) const
    {
        verify_range(beg, end);
        if (beg < end) {
            // Get the last partial word in the range.
            idx_t last_bit_index = end - 1;
            idx_t word_index = to_words_align_down(last_bit_index);
            bm_word_t cword = _map[word_index];
            // Mask for extracting and testing bits of last word.
            bm_word_t last_bit_mask = bm_word_t(1) << bit_in_word(last_bit_index);
            if ((cword & last_bit_mask) != 0) { // Test last bit.
                return last_bit_index;
            }
            // Extract prior bits, clearing those above last_bit_index.
            cword &= (last_bit_mask - 1);
            if (cword == 0) { // Test other bits in the last word.
                idx_t word_limit = to_words_align_down(beg);
                while (word_index-- > word_limit) {
                    cword = _map[word_index];
                    if (cword != 0) break;
                }
            }
            if (cword != 0) {
                idx_t result = bit_index(word_index) +
                    static_cast<idx_t>(BitsPerWord - 1 - static_cast<idx_t>(__builtin_clzl(cword)));
                if (result >= beg) return result;
            }
        }
        return end;
    }
    idx_t find_last_set_bit(idx_t beg) const { return find_last_set_bit(beg, size()); }

    // Apply function to the index of each set bit in [beg, end), in increasing
    // order. A function returning bool stops the iteration when it returns
    // false; a void function never stops early. Returns true if the whole
    // range was visited.
    template<typename Function>
    bool iterate(Function function, idx_t beg, idx_t end) const
    {
        for (idx_t index = beg; true; ++index) {
            index = find_first_set_bit(index, end);
            if (index >= end) {
                return true;
            } else if (!Invoke(function, index)) {
                return false;
            }
        }
    }

    template<typename Function>
    bool iterate(Function function) const
    {
        return iterate(function, 0, size());
    }

    bool is_empty() const
    {
        const idx_t words = size_in_words();
        for (idx_t i = 0; i < words; ++i) {
            if (_map[i] != 0) return false;
        }
        return true;
    }

    class ReverseIterator;

private:
    // bitMap.inline.hpp:318-333 IterateInvoker: a bool function may stop the
    // iteration, a void function never does.
    template<typename Function>
    static bool Invoke(Function& function, idx_t index)
    {
        return InvokeImpl(function, index, static_cast<decltype(function(index))*>(nullptr));
    }
    template<typename Function>
    static bool InvokeImpl(Function& function, idx_t index, bool*)
    {
        return function(index);
    }
    template<typename Function>
    static bool InvokeImpl(Function& function, idx_t index, void*)
    {
        function(index);
        return true;
    }
};

// Provides iteration over the indices of the set bits in a range of a bitmap,
// in decreasing order (bitMap.hpp:491-527).
class BitMap::ReverseIterator {
    const BitMap* _map;
    idx_t _cur_beg;
    idx_t _cur_end;

    static idx_t initial_end(const BitMap& map, idx_t beg, idx_t end)
    {
        idx_t pos = map.find_last_set_bit(beg, end);
        return (pos < end) ? (pos + 1) : beg;
    }

public:
    ReverseIterator() : _map(nullptr), _cur_beg(0), _cur_end(0) {}
    explicit ReverseIterator(const BitMap& map) : ReverseIterator(map, 0, map.size()) {}
    ReverseIterator(const BitMap& map, idx_t beg, idx_t end)
        : _map(&map), _cur_beg(beg), _cur_end(initial_end(map, beg, end))
    {}

    bool is_empty() const { return _cur_beg == _cur_end; }

    idx_t index() const
    {
        DCHECK(!is_empty());
        return _cur_end - 1;
    }

    void step()
    {
        DCHECK(!is_empty());
        idx_t lastpos = index();
        idx_t pos = _map->find_last_set_bit(_cur_beg, lastpos);
        _cur_end = (pos < lastpos) ? (pos + 1) : _cur_beg;
    }
};

// The BitMapView is used when the backing storage is managed externally (bitMap.hpp:605-609).
class BitMapView : public BitMap {
public:
    BitMapView() : BitMapView(nullptr, 0) {}
    BitMapView(bm_word_t* map, idx_t size_in_bits) : BitMap(map, size_in_bits) {}
};

// A BitMap with storage in the C heap (bitMap.hpp:641-658 CHeapBitMap over
// GrowableBitMap::initialize). Copy and assignment are disabled so the
// allocated memory cannot leak out to other instances.
class CHeapBitMap : public BitMap {
public:
    CHeapBitMap() : BitMap(nullptr, 0) {}
    explicit CHeapBitMap(idx_t size_in_bits, bool clear = true) : BitMap(nullptr, 0)
    {
        initialize(size_in_bits, clear);
    }
    ~CHeapBitMap() { free(map(), size_in_words()); }
    CHeapBitMap(const CHeapBitMap&) = delete;
    CHeapBitMap& operator=(const CHeapBitMap&) = delete;

    // Set up and optionally clear the bitmap memory. Precondition: the bitmap
    // was default constructed and has not yet had memory allocated.
    void initialize(idx_t size_in_bits, bool clear = true)
    {
        DCHECK(map() == nullptr);
        DCHECK(size() == 0);
        const idx_t new_size_in_words = calc_size_in_words(size_in_bits);
        bm_word_t* new_map = allocate(new_size_in_words);
        if (clear) {
            clear_range_of_words(new_map, 0, new_size_in_words);
        }
        update(new_map, size_in_bits);
    }

    static bm_word_t* allocate(idx_t size_in_words)
    {
        if (size_in_words == 0) {
            return nullptr;
        }
        void* memory = std::malloc(size_in_words * sizeof(bm_word_t));
        CHECK_DETAIL(memory != nullptr, "CHeapBitMap allocation failed: %zu words", size_in_words);
        return static_cast<bm_word_t*>(memory);
    }
    static void free(bm_word_t* map, idx_t size_in_words)
    {
        (void)size_in_words;
        std::free(map);
    }
};

} // namespace MapleRuntime

#endif // MRT_BASE_BITMAP_H
