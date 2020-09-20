#pragma once
#include <algorithm>
#include <cassert>

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
        assert(freeBlock > 0);
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
        assert(!isLastPhysicalBlockInPool()); // we were the last block, ASSERT
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
      assert(fl < 64 && fl >= 0); // "fl should be valid, was fl:%d", fl);
      auto& sizeClass = control.sizeclasses[fl];
      assert(sizeClass.sizeClass <= blockToInsert->size && control.sizeclasses[fl + 1].sizeClass > blockToInsert->size);// "sizeclass should be smaller than next sizeclass");
      assert(sl < 64 && sl >= 0); // "sl should be valid, was fl:%d sl:%d", fl, sl);
      auto* freeListHead = sizeClass.freeBlocks[sl];
      blockToInsert->freeBlock = 1;
      auto& insertedBlockFree = blockToInsert->freePart();
      insertedBlockFree.nextFree = reinterpret_cast<uintptr_t>(freeListHead);
      insertedBlockFree.previousFree = 0;
      if (freeListHead) {
        auto& freePart = freeListHead->freePart();
        freePart.previousFree = reinterpret_cast<uintptr_t>(blockToInsert);
      }
      sizeClass.freeBlocks[sl] = blockToInsert;
      set_bit(sizeClass.slBitmap, sl);
      set_bit(control.flBitmap, fl);
    }

    inline TLSFHeader* search_suitable_block(size_t size, int fl, int sl) noexcept {
      // first step, assume we got something at fl / sl location
      assert(size > 0 && fl >= 0 && sl >= 0);
      auto& secondLevel = control.sizeclasses[fl];
      auto candidatePtr = secondLevel.freeBlocks[sl];
      if (candidatePtr == nullptr || candidatePtr->size < size) {
        sl = ffs(secondLevel.slBitmap);
        candidatePtr = sl >= 0 ? secondLevel.freeBlocks[sl] : nullptr;
        if (sl < 0 || candidatePtr == nullptr) { // still didn't find
          // second step, scan bitmaps for empty slots
          // create mask to ignore first bits, could be wrong
          uint64_t mask = ~((1 << (fl + 1)) - 1);
          auto fl2 = ffs(control.flBitmap & mask);
          if (fl2 >= 0) {
            auto& secondLevel2 = control.sizeclasses[fl2];
            assert(secondLevel2.sizeClass >= size && secondLevel2.slBitmap != 0);// "bitmap expected to have something");
            auto sl2 = ffs(secondLevel2.slBitmap);
            assert(secondLevel2.freeBlocks[sl2] != nullptr);// "freeblocks expected to contain something");
            candidatePtr = sl2 >= 0 ? secondLevel2.freeBlocks[sl2] : nullptr;
            if (candidatePtr == nullptr || candidatePtr->size < size)
              return nullptr;
          }
          if (candidatePtr == nullptr || candidatePtr->size < size)
            return nullptr;
        }
        if (candidatePtr == nullptr || candidatePtr->size < size)
          return nullptr;
      }
      if (candidatePtr == nullptr || candidatePtr->size < size)
        return nullptr;
      return candidatePtr;
    }

    inline TLSFHeader* split(TLSFHeader* block, size_t size) noexcept {
      // Spawn header at the split
      TLSFHeader* split = block->splittedHeader(size);
      split->identifier = m_identifier;
      auto excessSize = block->size - size - sizeof(TLSFHeader); // need space for TLSFHeader and rest is free heap.
      split->size = excessSize;
      split->freeBlock = 0;
      split->previousPhysBlock = reinterpret_cast<uintptr_t>(block);
      split->lastPhysicalBlock = block->lastPhysicalBlock; // inherited value
      // update the original block
      block->size = size;
      block->lastPhysicalBlock = 0; // since we splitted, we will never be last one.
      assert(block->nextBlockHeader() == split); // ensure correct link with next block
      assert(split->fetchPreviousPhysBlock() == block); // both ways correct links
      return split;
    }

    inline void remove(TLSFHeader* block) noexcept {
      if (block == nullptr)
        return;
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
        assert(block == control.sizeclasses[fl].freeBlocks[sl]);
        control.sizeclasses[fl].freeBlocks[sl] = TLSFHeader::fromAddress(freepart.nextFree);
        if (control.sizeclasses[fl].freeBlocks[sl] == nullptr) {
          // need to remove bit
          assert(is_bit_set(control.sizeclasses[fl].slBitmap, sl));
          remove_bit(control.sizeclasses[fl].slBitmap, sl);
          if (control.sizeclasses[fl].slBitmap == 0 && is_bit_set(control.flBitmap, fl)) {
            remove_bit(control.flBitmap, fl);
          }
        }
      }
      block->freeBlock = 0; // no longer free block
    }

    inline TLSFHeader* merge(TLSFHeader* block) noexcept {
      // check if we have previous block
      TLSFHeader* merged = block;
      TLSFHeader* previous = block->fetchPreviousPhysBlock();
      if (previous != nullptr && previous->isFreeBlock())
      {
        remove(previous);
        merged = previous; // inherits previous phys block
        merged->lastPhysicalBlock = block->lastPhysicalBlock; // inherits as block came after
        merged->size += block->size + sizeof(TLSFHeader);
      }
      if (merged == previous && !merged->isLastPhysicalBlockInPool()) {
        TLSFHeader* next = merged->nextBlockHeader();
        if (next->isFreeBlock()) {
          remove(next);
          merged->lastPhysicalBlock = next->lastPhysicalBlock; // inherits as block came after
          merged->size += next->size + sizeof(TLSFHeader);
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
      size = std::max(mbs, size);
      int fl, sl, fl2, sl2;
      TLSFHeader* found_block, * remaining_block;
      mapping(size, fl, sl); // O(1)
      found_block = search_suitable_block(size, fl, sl);// O(1)
      remove(found_block); // O(1)
      if (found_block && found_block->size > size + mbs + sizeof(TLSFHeader)) {
        assert(found_block->freeBlock == 0);// "block shouldnt be free ");
        auto prevSize = found_block->size;
        remaining_block = split(found_block, size);
        mapping(remaining_block->size, fl2, sl2);
        assert(remaining_block->freeBlock == 0);// "block shouldnt be free ");
        assert(prevSize - size > mbs);
        insert(remaining_block, fl2, sl2); // O(1)
        assert(remaining_block->freeBlock == 1);// "block should be free at this point");
      }
      assert(found_block == nullptr || found_block->freeBlock == 0);// "block should be free at this point");
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
      assert(block != nullptr);
      TLSFHeader* header = TLSFHeader::fromDataPointer(block);
      assert(header->size > 0);
      assert(header->identifier == m_identifier);
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