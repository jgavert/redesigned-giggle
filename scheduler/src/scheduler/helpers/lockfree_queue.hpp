#pragma once

#include <atomic>
#include <array>
#include <vector>
#include <cassert>

// taken from github.com/apodus/rynx
// with only slight modifications to be able to take from both ends.
namespace rynx {
	namespace parallel {

    template<typename T, size_t MaxSize = 512>
    class per_thread_queue {
    static_assert((MaxSize& (MaxSize - 1)) == 0, "must be power of two");
    public:
      per_thread_queue() {
        m_data.resize(MaxSize);
      }

      void push_back(T&& t) noexcept {
        assert(((m_top_actual + 1) & (MaxSize - 1)) != ((m_bot_actual) & (MaxSize - 1)));
        auto index = m_top_actual.load();
        m_data[index & (MaxSize - 1)] = std::move(t);
        ++m_top_actual;
        if (m_bot_actual > ((1 << 30)+1)) [[unlikely]] {
          m_top_actual -= (1 << 30);
          m_bot_actual -= (1 << 30);
          m_bot_reserving -= (1 << 30);
        }
      }

      bool pop_back(T& ans) noexcept {
        {
          auto index = m_top_actual.fetch_sub(1);
          if (m_bot_reserving >= index) {
            // didn't get top
            ++m_top_actual;
            return false;
          }
        }

        {
          auto index = m_top_actual.load();
          ans = std::move(m_data[index & (MaxSize - 1)]);
          return true;
        }
      }

      bool pop_front(T& ans) noexcept {
        {
          auto index = m_bot_reserving.fetch_add(1);
          if (index >= m_top_actual) {
            // we didn't get it.
            --m_bot_reserving;
            return false;
          }
        }

        {
          auto index = m_bot_actual.fetch_add(1);
          ans = std::move(m_data[index & (MaxSize - 1)]);
          return true;
        }
      }

      uint32_t size() const {
        return m_top_actual - m_bot_actual;
      }

      bool empty() const {
        return size() == 0;
      }

    private:
      std::vector<T> m_data;
      alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> m_top_actual = 1;
      alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> m_bot_reserving = 1;
      alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> m_bot_actual = 1;
    };
	}
}