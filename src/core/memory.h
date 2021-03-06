//
// Created by Mike Smith on 2021/2/3.
//

#pragma once

#include <span>
#include <vector>
#include <memory>

#include <core/platform.h>
#include <core/concepts.h>
#include <core/logging.h>
#include <core/mathematics.h>

namespace luisa {

class Arena : public concepts::Noncopyable {

public:
    static constexpr auto block_size = static_cast<size_t>(256ul * 1024ul);

private:
    std::vector<std::byte *> _blocks;
    uint64_t _ptr{0ul};
    size_t _total{0ul};

public:
    Arena() noexcept = default;
    Arena(Arena &&) noexcept = default;
    Arena &operator=(Arena &&) noexcept = default;
    ~Arena() noexcept {
        for (auto p : _blocks) { aligned_free(p); }
    }

    [[nodiscard]] auto total_size() const noexcept { return _total; }

    template<typename T = std::byte, size_t alignment = alignof(T)>
    [[nodiscard]] auto allocate(size_t n = 1u) {

        static_assert(std::is_trivially_destructible_v<T>);
        static constexpr auto size = sizeof(T);

        auto byte_size = n * size;
        auto aligned_p = reinterpret_cast<std::byte *>((_ptr + alignment - 1u) / alignment * alignment);
        if (_blocks.empty() || aligned_p + byte_size > _blocks.back() + block_size) {
            static constexpr auto alloc_alignment = std::max(alignment, sizeof(void *));
            static_assert((alloc_alignment & (alloc_alignment - 1u)) == 0, "Alignment should be power of two.");
            auto alloc_size = (std::max(block_size, byte_size) + alloc_alignment - 1u) / alloc_alignment * alloc_alignment;
            aligned_p = static_cast<std::byte *>(aligned_alloc(alloc_alignment, alloc_size));
            if (aligned_p == nullptr) { LUISA_ERROR_WITH_LOCATION("Failed to allocate memory: size = {}, alignment = {}, count = {}", size, alignment, n); }
            _blocks.emplace_back(aligned_p);
            _total += alloc_size;
        }
        _ptr = reinterpret_cast<uint64_t>(aligned_p + byte_size);
        return reinterpret_cast<T *>(aligned_p);
    }

    template<typename T, typename... Args>
    [[nodiscard]] T *create(Args &&...args) {
        return luisa::construct_at(allocate<T>(1u), std::forward<Args>(args)...);
    }
};

template<typename T>
class ArenaVector : public concepts::Noncopyable {

    static_assert(std::is_trivially_destructible_v<T>);

private:
    Arena &_arena;
    T *_data{nullptr};
    size_t _capacity{0u};
    size_t _size{0u};

public:
    explicit ArenaVector(Arena &arena, size_t capacity = 16u) noexcept
        : _arena{arena},
          _data{arena.allocate<T>(capacity)},
          _capacity{capacity} {}

    template<typename U>
    requires concepts::Container<U> && concepts::Constructible<T, typename std::remove_cvref_t<U>::value_type>
    ArenaVector(Arena &arena, U &&span, size_t capacity = 0u)
        : ArenaVector{arena, std::max(span.size(), capacity)} {
        std::uninitialized_copy_n(span.begin(), span.size(), _data);
        _size = span.size();
    }

    template<typename U>
    requires concepts::Constructible<T, U> explicit ArenaVector(Arena &arena, std::initializer_list<U> init, size_t capacity = 0u)
        : ArenaVector{arena, std::max(init.size(), capacity)} {
        std::uninitialized_copy_n(init.begin(), init.size(), _data);
        _size = init.size();
    }

    ArenaVector(ArenaVector &&) noexcept = default;
    ArenaVector &operator=(ArenaVector &&) noexcept = default;

    [[nodiscard]] auto empty() const noexcept { return _size == 0u; }
    [[nodiscard]] auto capacity() const noexcept { return _capacity; }
    [[nodiscard]] auto size() const noexcept { return _size; }

    [[nodiscard]] T *data() noexcept { return _data; }
    [[nodiscard]] const T *data() const noexcept { return _data; }

    [[nodiscard]] T &operator[](size_t i) noexcept { return _data[i]; }
    [[nodiscard]] const T &operator[](size_t i) const noexcept { return _data[i]; }

    template<typename... Args>
    T &emplace_back(Args &&...args) {
        if (_size == _capacity) {
            _capacity = next_pow2(_capacity * 2u);
            LUISA_VERBOSE_WITH_LOCATION("Capacity of ArenaVector exceeded, reallocating for {}.", _capacity);
            auto new_data = _arena.allocate<T>(_capacity);
            std::uninitialized_move_n(_data, _size, new_data);
            _data = new_data;
        }
        return *luisa::construct_at(_data + _size++, std::forward<Args>(args)...);
    }

    void pop_back() noexcept { _size--; /* trivially destructible */ }

    [[nodiscard]] T &front() noexcept { return _data[0]; }
    [[nodiscard]] const T &front() const noexcept { return _data[0]; }
    [[nodiscard]] T &back() noexcept { return _data[_size - 1u]; }
    [[nodiscard]] const T &back() const noexcept { return _data[_size - 1u]; }

    [[nodiscard]] T *begin() noexcept { return _data; }
    [[nodiscard]] T *end() noexcept { return _data + _size; }
    [[nodiscard]] const T *cbegin() const noexcept { return _data; }
    [[nodiscard]] const T *cend() const noexcept { return _data + _size; }
};

template<typename T>
requires concepts::SpanConvertible<T>
ArenaVector(Arena &, T &&) -> ArenaVector<typename std::remove_cvref_t<T>::value_type>;

template<typename T>
ArenaVector(Arena &, std::initializer_list<T>) -> ArenaVector<T>;

struct ArenaString : public std::string_view {

    ArenaString(Arena &arena, std::string_view s) noexcept
        : std::string_view{std::strncpy(arena.allocate<char>(s.size() + 1), s.data(), s.size()), s.size()} {}

    [[nodiscard]] const char *c_str() const noexcept { return data(); }
};

namespace detail {

template<bool thread_safe = false>
class PoolThreadSafety {

public:
    template<typename F>
    decltype(auto) with_thread_safety(F &&f) { return f(); }
};

template<>
class PoolThreadSafety<true> {

private:
    std::mutex _mutex;

public:
    template<typename F>
    decltype(auto) with_thread_safety(F &&f) {
        std::scoped_lock lock{_mutex};
        return f();
    }
};

}// namespace detail

template<typename T, bool thread_safe = true>
class Pool : detail::PoolThreadSafety<thread_safe> {

    struct Node {
        T object;
        Node *next;
        [[nodiscard]] static auto of(T *data) noexcept {
            return reinterpret_cast<Node *>(data);
        }
    };

private:
    static_assert(std::is_trivially_destructible_v<T>);
    Arena _arena;
    Node *_head{nullptr};

public:
    template<typename... Args>
    [[nodiscard]] auto obtain(Args &&...args) {
        auto node = this->with_thread_safety([this] {
            if (_head == nullptr) {// empty pool
                return _arena.allocate<Node>();
            }
            auto node = _head;
            _head = _head->next;
            return node;
        });
        return luisa::construct_at(&node->object, std::forward<Args>(args)...);
    }

    void recycle(T *object) noexcept {
        this->with_thread_safety([&] {
            auto node = Node::of(object);
            node->next = _head;
            _head = node;
        });
    }
};

}// namespace luisa
