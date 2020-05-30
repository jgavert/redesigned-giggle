#pragma once
#include "scheduler/helpers/heap_allocator.hpp"
#include <vector>

class DynamicHeapAllocator
{
  struct DynamicHeap
  {
    DynamicHeap(){}
    DynamicHeap(void* heap, HeapAllocatorRaw allocator) : heap(heap), allocator(allocator){}
    void* heap;
    HeapAllocatorRaw allocator;
  };
  std::vector<DynamicHeap> m_heaps;
  size_t m_heapSize;
  uint16_t m_identifier;

  uint32_t combine(uint16_t ident, uint16_t index) {
    uint32_t res = static_cast<uint32_t>(ident);
    res = res << 16;
    res |= static_cast<uint32_t>(index);
    return res;
  }
  uint16_t getIdent(uint32_t ident) {
    return static_cast<uint16_t>(ident >> 16);
  }
  uint16_t getIndex(uint32_t ident) {
    return static_cast<uint16_t>(ident);
  }

  void newAllocator() {
    uint16_t index = static_cast<uint16_t>(m_heaps.size());
    void* heap = malloc(m_heapSize);
    auto combined = combine(m_identifier, index);
    assert(m_identifier == getIdent(combined));
    assert(index == getIndex(combined));
    m_heaps.emplace_back(heap, HeapAllocatorRaw(heap, m_heapSize, combined));
  }
public:
  DynamicHeapAllocator(){}
  DynamicHeapAllocator(uint16_t identifier, size_t heapSize = 16 * 1024 * 1024)
    : m_identifier(identifier), m_heapSize(heapSize) {
    newAllocator();
  }
  DynamicHeapAllocator(const DynamicHeapAllocator& other) :m_heapSize(other.m_heapSize), m_identifier(other.m_identifier) { newAllocator(); }
  DynamicHeapAllocator(DynamicHeapAllocator&& other) :m_heaps(std::move(other.m_heaps)), m_heapSize(other.m_heapSize), m_identifier(other.m_identifier) {}
  DynamicHeapAllocator& operator=(const DynamicHeapAllocator& other) {
    m_heapSize = other.m_heapSize;
    m_identifier = other.m_identifier;
    newAllocator();
    return *this;
  }
  DynamicHeapAllocator& operator=(DynamicHeapAllocator&& other) {
    m_heaps = std::move(other.m_heaps);
    m_heapSize = std::move(other.m_heapSize);
    m_identifier = std::move(other.m_identifier);
    return *this;
  }
  ~DynamicHeapAllocator() {
    for (auto&& heap : m_heaps)
      free(heap.heap);
  }

  void* allocate(size_t sz) {
    for (auto&& heap : m_heaps) {
      auto alloc = heap.allocator.allocate(sz);
      if (alloc != nullptr)
        return alloc;
    }
    newAllocator();
    return m_heaps.back().allocator.allocate(sz);
  }

  void deallocate(void* ptr, size_t sz) {
    auto ident = m_heaps.front().allocator.allocationIdentifier(ptr);
    assert(m_identifier == getIdent(ident));
    auto index = getIndex(ident);
    m_heaps[index].allocator.free(ptr);
  }

  void deallocate(void* ptr) {
    auto ident = m_heaps.front().allocator.allocationIdentifier(ptr);
    assert(m_identifier == getIdent(ident));
    auto index = getIndex(ident);
    m_heaps[index].allocator.free(ptr);
  }
};
