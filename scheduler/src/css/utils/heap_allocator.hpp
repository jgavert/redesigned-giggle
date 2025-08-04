#pragma once
#include <algorithm>

//#define DEBUG_TLSF

#ifdef DEBUG_TLSF
#include <vector>
#include <tuple>
#endif

#if NDEBUG
#define CSS_ASSERT(cond) __assume(cond);
#else
#include <cassert>
#define CSS_ASSERT(cond) assert(cond);
#endif

namespace css
{
  class HeapAllocator {
    struct TLSFFreeBlock {
      // part of "free block header", should ignore if isn't freeblock.
      uintptr_t nextFree;
      uintptr_t previousFree;
    };
    struct alignas(16) TLSFHeader {
      uint64_t size : 30;
      uint64_t lastPhysicalBlock : 1; // marks if we are the last physical block, to prevent searching for next block with nextBlockHeader
      uint64_t freeBlock : 1;
      uint64_t identifier : 32;
      uintptr_t previousPhysBlock; // points to start of TLSFHeader

      bool isFreeBlock() const {
        return freeBlock > 0;
      }
      bool isLastPhysicalBlockInPool() const {
        return lastPhysicalBlock > 0;
      }
      TLSFFreeBlock& freePart() noexcept {
        CSS_ASSERT(freeBlock > 0);
        return *reinterpret_cast<TLSFFreeBlock*>(this + 1);
      }
      TLSFHeader* fetchPreviousPhysBlock() noexcept {
        return reinterpret_cast<TLSFHeader*>(previousPhysBlock);
      }
      static TLSFHeader* fromAddress(uintptr_t ptr) {
        return reinterpret_cast<TLSFHeader*>(ptr);
      }
      static TLSFHeader* fromDataPointer(void* ptr) {
        return reinterpret_cast<TLSFHeader*>(ptr) - 1;
      }
      TLSFHeader* nextBlockHeader() {
        CSS_ASSERT(!isLastPhysicalBlockInPool()); // we were the last block, ASSERT
        return reinterpret_cast<TLSFHeader*>(reinterpret_cast<char*>(this + 1) + size);
      }
      TLSFHeader* splittedHeader(size_t offsetWithinBlock) {
        return reinterpret_cast<TLSFHeader*>(reinterpret_cast<char*>(this + 1) + offsetWithinBlock);
      }
      void* data() {
        return reinterpret_cast<void*>(this + 1);
      }
    };
    struct TLSFSizeClass {
      size_t sizeClass = 0;
      uint64_t slBitmap = 0;
      TLSFHeader* freeBlocks[32] = {}; // pointer chase vector
    };

    struct TLSFControl {
      uint64_t flBitmap = 0;
      TLSFSizeClass sizeclasses[32] = {};
    };

    uintptr_t m_heap;
    size_t m_heapSize;
    uint32_t m_identifier;

    int fli = 0;        // first level index
    int sli = 0;        // second level index, typically 5
    unsigned sli_count;  // second level index, typically 5
    uint64_t mbs;        // minimum block size
    int min_fli;
    TLSFControl control;
    size_t m_usedSize;

#ifdef DEBUG_TLSF
    std::vector<std::tuple<TLSFHeader*, TLSFHeader, uintptr_t, bool>> event;
#endif

    inline int fls(uint64_t size) const noexcept {
      if (size == 0)
        return -1;
#if 1 
      unsigned long index;
      return _BitScanReverse64(&index, size) ? index : -1;
#else
      return  63 - __builtin_clzll(size);
#endif
    }

    inline int ffs(uint64_t size) const noexcept {
      if (size == 0)
        return -1;
#if 1 
      unsigned long index;
      return _BitScanForward64(&index, size) ? index : -1;
#else
      return __builtin_ctzll(size);
#endif
    }

    inline void mapping(uint64_t size, int& fl, int& sl) noexcept {
      fl = fls(size);
      sl = static_cast<int>((size ^ (1ull << fl)) >> (fl - sli));
      fl = first_level_index(fl);
    }

    inline int first_level_index(int fli) noexcept {
      if (fli < min_fli)
        return 0;
      return fli - min_fli;
    }

    inline void initialize() noexcept {
      fli = fls(m_heapSize);
      mbs = std::min(m_heapSize, mbs);
      min_fli = fls(mbs);
      control.flBitmap = 0;
      for (int i = min_fli; i < 32; ++i) {
        auto sizeClassIndex = first_level_index(i);
        size_t sizeClass = 1ull << static_cast<uint64_t>(i);
        control.sizeclasses[sizeClassIndex] = TLSFSizeClass{ sizeClass, 0, nullptr };
      }
    }

    inline void remove_bit(uint64_t& value, int index) noexcept { value = value ^ (1ull << static_cast<uint64_t>(index)); }
    inline void set_bit(uint64_t& value, int index) noexcept { value |= (1ull << static_cast<uint64_t>(index)); }
    inline bool is_bit_set(uint64_t& value, int index) noexcept { return ((value >> static_cast<uint64_t>(index)) & 1ull) > 0ull; }


    inline void insert(TLSFHeader* blockToInsert, int fl, int sl) noexcept {
      CSS_ASSERT(fl < 64 && fl >= 0); // "fl should be valid, was fl:%d", fl);
      auto& sizeClass = control.sizeclasses[fl];
      CSS_ASSERT(sizeClass.sizeClass <= blockToInsert->size && control.sizeclasses[fl + 1].sizeClass > blockToInsert->size);// "sizeclass should be smaller than next sizeclass");
      CSS_ASSERT(sl < 64 && sl >= 0); // "sl should be valid, was fl:%d sl:%d", fl, sl);
      auto* freeListHead = sizeClass.freeBlocks[sl];
      blockToInsert->freeBlock = 1;
      auto& insertedBlockFree = blockToInsert->freePart(); // this assumes that freepart actually does not contain any next freespaces, but it does
      insertedBlockFree.nextFree = reinterpret_cast<uintptr_t>(freeListHead); 
      insertedBlockFree.previousFree = 0;
      if (freeListHead) {
        CSS_ASSERT(freeListHead->identifier == m_identifier);
        auto& freePart = freeListHead->freePart();
        freePart.previousFree = reinterpret_cast<uintptr_t>(blockToInsert);
      }
      sizeClass.freeBlocks[sl] = blockToInsert;
      set_bit(sizeClass.slBitmap, sl);
      set_bit(control.flBitmap, fl);
    }

    inline TLSFHeader* search_suitable_block(size_t size, int fl, int sl) noexcept {
        CSS_ASSERT(size > 0 && fl >= 0 && sl >= 0);
        TLSFHeader* candidate = nullptr;
        auto& sc = control.sizeclasses[fl];

        // First, try scanning the free list within the current bucket.
        // Mask out bits below 'sl'
        uint64_t candidateMask = sc.slBitmap & ~((1ULL << sl) - 1);
        while (candidateMask) {
            // ffs returns the index of the least-significant set bit.
            int bitIndex = ffs(candidateMask);
            candidate = sc.freeBlocks[bitIndex];
            if (candidate && candidate->size >= size)
                return candidate;
            // Remove the bit we just processed.
            candidateMask &= candidateMask - 1;
        }

        // Next, scan higher buckets (first-level).
        // Create mask to ignore lower buckets (including the current one)
        uint64_t fl_mask = control.flBitmap & ~((1ULL << (fl + 1)) - 1);
        int fl2 = ffs(fl_mask);
        if (fl2 >= 0) {
            auto& sc2 = control.sizeclasses[fl2];
            int sl2 = ffs(sc2.slBitmap);
            if (sl2 >= 0) {
                candidate = sc2.freeBlocks[sl2];
                // In a correctly maintained TLSF, candidate should be large enough.
                CSS_ASSERT(candidate == nullptr || candidate->isFreeBlock());
                return candidate;
            }
        }
        return nullptr;
    }

    inline TLSFHeader* split(TLSFHeader* block, size_t size) noexcept {
      // Spawn header at the split
      CSS_ASSERT(!block->isFreeBlock());
      if (block->previousPhysBlock > 0) {
        auto* prevBlock = block->fetchPreviousPhysBlock();
        CSS_ASSERT(prevBlock->identifier == m_identifier);
      }
      CSS_ASSERT(block->identifier == m_identifier);
      CSS_ASSERT(block->size > std::max(size,sizeof(TLSFFreeBlock)) + sizeof(TLSFHeader));
      TLSFHeader* split = block->splittedHeader(size);
      split->identifier = m_identifier;
      auto excessSize = block->size - size - sizeof(TLSFHeader); // need space for TLSFHeader and rest is free heap.
      split->size = excessSize;
      split->freeBlock = 0;
      split->previousPhysBlock = reinterpret_cast<uintptr_t>(block);
      if (split->previousPhysBlock > 0) {
        auto* prevBlock = split->fetchPreviousPhysBlock();
        CSS_ASSERT(prevBlock->identifier == m_identifier);
      }
      split->lastPhysicalBlock = block->lastPhysicalBlock; // inherited value
      // update the original block
      block->size = size;
      block->lastPhysicalBlock = 0; // since we splitted, we will never be last one.
      CSS_ASSERT(block->nextBlockHeader() == split); // ensure correct link with next block
      CSS_ASSERT(split->fetchPreviousPhysBlock() == block); // both ways correct links
      CSS_ASSERT(split->fetchPreviousPhysBlock()->identifier == m_identifier); // both ways correct links
      CSS_ASSERT(split->fetchPreviousPhysBlock()->lastPhysicalBlock == 0); // both ways correct links
      return split;
    }

    inline void remove(TLSFHeader* block) noexcept {
      if (block == nullptr)
        return;
      CSS_ASSERT(block->isFreeBlock());
      auto freepart = block->freePart();
      if (freepart.previousFree != 0) {
        // remove myself from chain
        TLSFHeader* freeListPrevious = TLSFHeader::fromAddress(freepart.previousFree);
        auto& head = freeListPrevious->freePart();
        head.nextFree = freepart.nextFree;
      }
      if (freepart.nextFree != 0) {
        TLSFHeader* freeListNext = TLSFHeader::fromAddress(freepart.nextFree);
        auto& next = freeListNext->freePart();
        next.previousFree = freepart.previousFree;
      }
      if (freepart.previousFree == 0) {
        int fl, sl;
        mapping(block->size, fl, sl);
        CSS_ASSERT(block == control.sizeclasses[fl].freeBlocks[sl]);
        control.sizeclasses[fl].freeBlocks[sl] = TLSFHeader::fromAddress(freepart.nextFree);
        if (control.sizeclasses[fl].freeBlocks[sl] == nullptr) {
          // need to remove bit
          CSS_ASSERT(is_bit_set(control.sizeclasses[fl].slBitmap, sl));
          remove_bit(control.sizeclasses[fl].slBitmap, sl);
          if (control.sizeclasses[fl].slBitmap == 0 && is_bit_set(control.flBitmap, fl)) {
            remove_bit(control.flBitmap, fl);
          }
        }
        else {
          control.sizeclasses[fl].freeBlocks[sl]->freePart().previousFree = 0;
        }
      }
      block->freeBlock = 0; // no longer free block
    }

    inline TLSFHeader* merge(TLSFHeader* block) noexcept {
      // check if we have previous block
      CSS_ASSERT(block != nullptr);
      CSS_ASSERT(!block->isFreeBlock());
      TLSFHeader* merged = block;
      TLSFHeader* previous = block->fetchPreviousPhysBlock();
      if (previous != nullptr) // detect cases where previous pointer is blown up
        CSS_ASSERT(previous->identifier == m_identifier);
      if (previous != nullptr && previous->identifier == m_identifier && previous->isFreeBlock())
      {
        remove(previous);
        merged = previous; // inherits previous phys block
        merged->lastPhysicalBlock = block->lastPhysicalBlock; // inherits as block came after
        merged->size += block->size + sizeof(TLSFHeader);
      }
      if (merged && !merged->isLastPhysicalBlockInPool()) {
        TLSFHeader* next = merged->nextBlockHeader();
        next->previousPhysBlock = reinterpret_cast<uintptr_t>(merged);
        if (next->isFreeBlock()) {
          remove(next);
          merged->lastPhysicalBlock = next->lastPhysicalBlock; // inherits as block came after
          merged->size += next->size + sizeof(TLSFHeader);

          if (!next->isLastPhysicalBlockInPool()) {
            TLSFHeader* next2 = next->nextBlockHeader();
            next2->previousPhysBlock = reinterpret_cast<uintptr_t>(merged);
          }
        }
      }
      return merged;
    }

  public:
    HeapAllocator()
      : m_heap(0), m_heapSize(0), m_identifier(0), mbs(16), sli(1), fli(fls(1)), min_fli(fls(1)), sli_count(1 << 3), m_usedSize(0) {
    }

    HeapAllocator(void* heap, uintptr_t size, uint32_t identifier = 0, size_t minimumBlockSize = 16, int sli = 3)
      : m_heap(reinterpret_cast<uintptr_t>(heap)), m_heapSize(size), m_identifier(identifier), mbs(minimumBlockSize), sli(sli), sli_count(1 << sli), m_usedSize(0) {
      initialize();
      TLSFHeader* header = reinterpret_cast<TLSFHeader*>(heap);
      header->size = m_heapSize - sizeof(TLSFHeader);
      header->lastPhysicalBlock = 1;
      header->previousPhysBlock = 0;
      header->freeBlock = 1;
      header->identifier = static_cast<uint64_t>(m_identifier);
      int fl, sl;
      mapping(header->size, fl, sl);
      insert(header, fl, sl);
    }

    template <typename T>
    [[nodiscard]] T* allocObj() noexcept {
      return new (allocate(sizeof(T))) T();
    }

    [[nodiscard]] void* allocate(size_t size) noexcept
    {
      size_t orig_size = size;
      size = std::max(std::max(mbs, size), sizeof(TLSFFreeBlock)) + std::max(mbs, sizeof(TLSFHeader));
      int fl, sl, fl2, sl2;
      TLSFHeader* found_block, * remaining_block;
      mapping(size, fl, sl); // O(1)
      found_block = search_suitable_block(size, fl, sl);// O(1)
      remove(found_block); // O(1)
      if (found_block && found_block->size > size + sizeof(TLSFHeader)) {
        CSS_ASSERT(found_block->freeBlock == 0);// "block shouldnt be free ");
        auto prevSize = found_block->size;
        remaining_block = split(found_block, size);
        mapping(remaining_block->size, fl2, sl2);
        CSS_ASSERT(remaining_block->freeBlock == 0);// "block shouldnt be free ");
        CSS_ASSERT(prevSize - size > mbs);
        insert(remaining_block, fl2, sl2); // O(1)
        CSS_ASSERT(remaining_block->freeBlock == 1);// "block should be free at this point");
      }
      CSS_ASSERT(found_block == nullptr || found_block->freeBlock == 0);// "block should be free at this point");
      if (found_block && found_block->previousPhysBlock)
      {
        auto prevBlock = found_block->fetchPreviousPhysBlock();
        CSS_ASSERT(prevBlock->identifier == m_identifier);
      }
      if (found_block) {
#ifdef DEBUG_TLSF
        event.push_back(std::make_tuple(found_block, *found_block, reinterpret_cast<uintptr_t>(found_block), true));
#endif
        CSS_ASSERT(found_block->size >= size);
      }
      if (found_block) m_usedSize += found_block->size + sizeof(TLSFHeader);
      return found_block ? found_block->data() : nullptr;
    }

    template<typename T>
    void freeObj(T* obj) noexcept {
      obj->~T();
      free(obj);
    }

    uint32_t allocationIdentifier(void* block) noexcept {
      TLSFHeader* header = TLSFHeader::fromDataPointer(block);
      return header->identifier;
    }

    void free(void* block) noexcept {
#ifdef DEBUG_TLSF
      event.push_back(std::make_tuple(nullptr, *TLSFHeader::fromDataPointer(block), reinterpret_cast<uintptr_t>(TLSFHeader::fromDataPointer(block)), false));
#endif
      CSS_ASSERT(block != nullptr);
      TLSFHeader* header = TLSFHeader::fromDataPointer(block);
      if (header) m_usedSize -= (header->size + sizeof(TLSFHeader));
      CSS_ASSERT(header->size > 0);
      CSS_ASSERT(header->identifier == m_identifier);
      TLSFHeader* bigBlock = merge(header);
      int fl, sl;
      mapping(bigBlock->size, fl, sl);
      insert(bigBlock, fl, sl);
    }

    size_t findLargestAllocation() const noexcept;

    inline size_t size() const noexcept {
      return m_heapSize - m_usedSize;
    }
    inline size_t max_size() const noexcept {
      return m_heapSize;
    }
    inline size_t size_allocated() const noexcept {
      return m_usedSize;
    }
  };
}