#pragma once

#include <array>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

namespace fastfix::base {

template <typename T, std::size_t InlineCapacity>
class InlineSplitVector {
  public:
    static_assert(InlineCapacity > 0U, "InlineSplitVector requires positive inline capacity");

    class iterator {
      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        iterator() = default;

        auto operator*() const -> reference {
            return owner_->operator[](index_);
        }

        auto operator->() const -> pointer {
            return &owner_->operator[](index_);
        }

        auto operator++() -> iterator& {
            ++index_;
            return *this;
        }

        auto operator++(int) -> iterator {
            auto copy = *this;
            ++(*this);
            return copy;
        }

        [[nodiscard]] auto operator==(const iterator& other) const -> bool {
            return owner_ == other.owner_ && index_ == other.index_;
        }

        [[nodiscard]] auto operator!=(const iterator& other) const -> bool {
            return !(*this == other);
        }

      private:
        friend class InlineSplitVector;

        iterator(InlineSplitVector* owner, std::size_t index)
            : owner_(owner), index_(index) {
        }

        InlineSplitVector* owner_{nullptr};
        std::size_t index_{0U};
    };

    class const_iterator {
      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = const T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        const_iterator() = default;

        const_iterator(iterator other)
            : owner_(other.owner_), index_(other.index_) {
        }

        auto operator*() const -> reference {
            return owner_->operator[](index_);
        }

        auto operator->() const -> pointer {
            return &owner_->operator[](index_);
        }

        auto operator++() -> const_iterator& {
            ++index_;
            return *this;
        }

        auto operator++(int) -> const_iterator {
            auto copy = *this;
            ++(*this);
            return copy;
        }

        [[nodiscard]] auto operator==(const const_iterator& other) const -> bool {
            return owner_ == other.owner_ && index_ == other.index_;
        }

        [[nodiscard]] auto operator!=(const const_iterator& other) const -> bool {
            return !(*this == other);
        }

      private:
        friend class InlineSplitVector;

        const_iterator(const InlineSplitVector* owner, std::size_t index)
            : owner_(owner), index_(index) {
        }

        const InlineSplitVector* owner_{nullptr};
        std::size_t index_{0U};
    };

    [[nodiscard]] auto empty() const -> bool {
        return size() == 0U;
    }

    [[nodiscard]] auto size() const -> std::size_t {
        return inline_size_ + overflow_.size();
    }

    auto begin() -> iterator {
        return iterator(this, 0U);
    }

    auto end() -> iterator {
        return iterator(this, size());
    }

    auto begin() const -> const_iterator {
        return const_iterator(this, 0U);
    }

    auto end() const -> const_iterator {
        return const_iterator(this, size());
    }

    auto clear() -> void {
        inline_size_ = 0U;
        overflow_.clear();
    }

    auto reserve(std::size_t count) -> void {
        if (count > InlineCapacity) {
            overflow_.reserve(count - InlineCapacity);
        }
    }

    auto push_back(const T& value) -> void {
        if (inline_size_ < InlineCapacity) {
            inline_storage_[inline_size_++] = value;
            return;
        }
        overflow_.push_back(value);
    }

    auto push_back(T&& value) -> void {
        if (inline_size_ < InlineCapacity) {
            inline_storage_[inline_size_++] = std::move(value);
            return;
        }
        overflow_.push_back(std::move(value));
    }

    [[nodiscard]] auto front() -> T& {
        return (*this)[0U];
    }

    [[nodiscard]] auto front() const -> const T& {
        return (*this)[0U];
    }

    [[nodiscard]] auto back() -> T& {
        return (*this)[size() - 1U];
    }

    [[nodiscard]] auto back() const -> const T& {
        return (*this)[size() - 1U];
    }

    [[nodiscard]] auto operator[](std::size_t index) -> T& {
        return index < inline_size_ ? inline_storage_[index] : overflow_[index - inline_size_];
    }

    [[nodiscard]] auto operator[](std::size_t index) const -> const T& {
        return index < inline_size_ ? inline_storage_[index] : overflow_[index - inline_size_];
    }

  private:
    std::array<T, InlineCapacity> inline_storage_{};
    std::size_t inline_size_{0U};
    std::vector<T> overflow_;
};

}  // namespace fastfix::base