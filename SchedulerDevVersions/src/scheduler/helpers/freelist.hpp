#pragma once
#include <vector>
#include <cassert>

template<typename T>
class AllocatingFreelist
{
  std::vector<T*> m_values;
public:
  AllocatingFreelist()
  {

  }
  AllocatingFreelist(AllocatingFreelist&) = delete;
  AllocatingFreelist(AllocatingFreelist&& other) noexcept : m_values(std::move(other.m_values)) { other.m_values.clear(); }
  ~AllocatingFreelist() {
    for (auto&& it : m_values) {
      delete it;
    }
    m_values.clear();
  }

  [[nodiscard]] T* allocate()
  {
    if (m_values.empty())
    {
      return new T();
    }
    auto val = m_values.back();
    m_values.pop_back();
    return val;
  }

  void release(T* val)
  {
    assert(val != nullptr);
    m_values.push_back(val);
    assert(!m_values.empty());
  }

  bool empty() const noexcept
  {
    return m_values.empty();
  }

  size_t size() const noexcept
  {
    return m_values.size();
  }

  size_t max_size() const noexcept
  {
    return m_values.max_size();
  }
};