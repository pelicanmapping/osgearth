// string_view_compat.hpp
#pragma once

#include <string>
#include <stdexcept>
#include <cstddef>
#include <algorithm>
#include <type_traits>

namespace polyfillcpp14 {

template<class CharT, class Traits = std::char_traits<CharT>>
class basic_string_view {
public:
    using traits_type            = Traits;
    using value_type             = CharT;
    using pointer                = const CharT*;
    using const_pointer          = const CharT*;
    using reference              = const CharT&;
    using const_reference        = const CharT&;
    using const_iterator         = const CharT*;
    using iterator               = const_iterator;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    using reverse_iterator       = const_reverse_iterator;
    using size_type              = std::size_t;
    using difference_type        = std::ptrdiff_t;

    static const size_type npos = size_type(-1);

    // 1) constructors
    basic_string_view() noexcept
        : data_(nullptr), size_(0) {}

    basic_string_view(const CharT* s, size_type count)
        : data_(s), size_(count) {}

    basic_string_view(const CharT* s)
        : data_(s), size_(traits_type::length(s)) {}

    template<class Alloc>
    basic_string_view(const std::basic_string<CharT, Traits, Alloc>& s)
        : data_(s.data()), size_(s.size()) {}

    // 2) iterators
    const_iterator begin()  const noexcept { return data_; }
    const_iterator end()    const noexcept { return data_ + size_; }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend()   const noexcept { return end(); }

    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator rend()   const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const noexcept { return rbegin(); }
    const_reverse_iterator crend()   const noexcept { return rend(); }

    // 3) capacity
    size_type size()     const noexcept { return size_; }
    size_type length()   const noexcept { return size_; }
    size_type max_size() const noexcept { return (std::numeric_limits<size_type>::max)(); }
    bool empty()         const noexcept { return size_ == 0; }

    // 4) element access
    const_reference operator[](size_type pos) const { return data_[pos]; }

    const_reference at(size_type pos) const {
        if (pos >= size_) {
            throw std::out_of_range("basic_string_view::at");
        }
        return data_[pos];
    }

    const_reference front() const { return data_[0]; }
    const_reference back()  const { return data_[size_ - 1]; }
    const_pointer   data()  const noexcept { return data_; }

    // 5) modifiers
    void remove_prefix(size_type n) {
        if (n > size_) n = size_;
        data_  += n;
        size_  -= n;
    }

    void remove_suffix(size_type n) {
        if (n > size_) n = size_;
        size_ -= n;
    }

    void swap(basic_string_view& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
    }

    // 6) substring
    basic_string_view substr(size_type pos = 0, size_type count = npos) const {
        if (pos > size_) {
            throw std::out_of_range("basic_string_view::substr");
        }
        size_type rlen = (std::min)(count, size_ - pos);
        return basic_string_view(data_ + pos, rlen);
    }

    // 7) comparison
    int compare(basic_string_view other) const noexcept {
        const size_type rlen = (std::min)(size_, other.size_);
        int cmp = traits_type::compare(data_, other.data_, rlen);
        if (cmp != 0) return cmp;
        if (size_ < other.size_) return -1;
        if (size_ > other.size_) return 1;
        return 0;
    }

private:
    const CharT* data_;
    size_type    size_;
};

// non-member comparison operators
template<class CharT, class Traits>
inline bool operator==(basic_string_view<CharT, Traits> lhs,
                       basic_string_view<CharT, Traits> rhs) noexcept {
    return lhs.size() == rhs.size() && lhs.compare(rhs) == 0;
}

template<class CharT, class Traits>
inline bool operator!=(basic_string_view<CharT, Traits> lhs,
                       basic_string_view<CharT, Traits> rhs) noexcept {
    return !(lhs == rhs);
}

template<class CharT, class Traits>
inline bool operator<(basic_string_view<CharT, Traits> lhs,
                      basic_string_view<CharT, Traits> rhs) noexcept {
    return lhs.compare(rhs) < 0;
}

template<class CharT, class Traits>
inline bool operator<=(basic_string_view<CharT, Traits> lhs,
                       basic_string_view<CharT, Traits> rhs) noexcept {
    return lhs.compare(rhs) <= 0;
}

template<class CharT, class Traits>
inline bool operator>(basic_string_view<CharT, Traits> lhs,
                      basic_string_view<CharT, Traits> rhs) noexcept {
    return lhs.compare(rhs) > 0;
}

template<class CharT, class Traits>
inline bool operator>=(basic_string_view<CharT, Traits> lhs,
                       basic_string_view<CharT, Traits> rhs) noexcept {
    return lhs.compare(rhs) >= 0;
}

// aliases like the standard
using string_view  = basic_string_view<char>;
using wstring_view = basic_string_view<wchar_t>;

} // namespace polyfillcpp14
