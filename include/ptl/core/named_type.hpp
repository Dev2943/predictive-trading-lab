#pragma once

/// \file named_type.hpp
/// Zero-overhead strong typedefs.
///
/// Price, Qty, Bps and Notional are all doubles. Nothing stops you writing
/// `qty / price` where you meant `price / qty`, or handing basis points to a
/// parameter expecting a fraction -- and in a P&L engine that class of bug is
/// silent. Distinct types make it a compile error.
///
/// The skills are separate empty class templates rather than sharing a common
/// CRTP base on purpose: multiple base subobjects OF THE SAME empty type
/// cannot share an address, so a shared base would defeat empty-base
/// optimisation and grow sizeof(Price) past sizeof(double). Distinct empty
/// bases are all collapsed. tests/unit/test_named_type.cpp asserts this.

#include <compare>
#include <cstddef>
#include <functional>
#include <utility>

namespace ptl {

template <class T, class Tag, template <class> class... Skills>
class NamedType;

/// Extracts the underlying scalar of a NamedType.
///
/// A skill cannot write `typename T::underlying_type`: skills are instantiated
/// as base classes of NamedType, so at that point NamedType is still an
/// incomplete type and has no members to name. Matching on the template-id
/// instead needs only the template arguments, which are available.
template <class T>
struct UnderlyingOf;

template <class T, class Tag, template <class> class... Skills>
struct UnderlyingOf<NamedType<T, Tag, Skills...>> {
    using type = T;
};

template <class T>
using underlying_t = typename UnderlyingOf<T>::type;

template <class T, class Tag, template <class> class... Skills>
class NamedType : public Skills<NamedType<T, Tag, Skills...>>... {
public:
    using underlying_type = T;

    constexpr NamedType() noexcept = default;
    constexpr explicit NamedType(T value) noexcept : value_(value) {}

    [[nodiscard]] constexpr T&       get()       noexcept { return value_; }
    [[nodiscard]] constexpr const T& get() const noexcept { return value_; }
    [[nodiscard]] constexpr explicit operator T() const noexcept { return value_; }

private:
    T value_{};
};

// --- skills ---------------------------------------------------------------
// Each defines hidden friends, found by ADL because the skill is a base of the
// NamedType and base classes contribute to the associated-class set.

template <class T>
struct Addable {
    [[nodiscard]] friend constexpr T operator+(T a, T b) noexcept {
        return T{a.get() + b.get()};
    }
    constexpr T& operator+=(T other) noexcept {
        auto& self = static_cast<T&>(*this);
        self.get() += other.get();
        return self;
    }
};

template <class T>
struct Subtractable {
    [[nodiscard]] friend constexpr T operator-(T a, T b) noexcept {
        return T{a.get() - b.get()};
    }
    constexpr T& operator-=(T other) noexcept {
        auto& self = static_cast<T&>(*this);
        self.get() -= other.get();
        return self;
    }
};

template <class T>
struct Negatable {
    [[nodiscard]] friend constexpr T operator-(T a) noexcept { return T{-a.get()}; }
};

/// Scaling by a bare scalar is allowed -- half a quantity is a quantity.
/// Multiplying two strong types is NOT: Price * Qty has different units and
/// must go through the explicit `notional()` conversion in types.hpp.
template <class T>
struct ScalarScalable {
    using U = underlying_t<T>;
    [[nodiscard]] friend constexpr T operator*(T a, U k) noexcept { return T{a.get() * k}; }
    [[nodiscard]] friend constexpr T operator*(U k, T a) noexcept { return T{a.get() * k}; }
    [[nodiscard]] friend constexpr T operator/(T a, U k) noexcept { return T{a.get() / k}; }
    /// Ratio of two same-unit quantities is dimensionless.
    [[nodiscard]] friend constexpr U operator/(T a, T b) noexcept { return a.get() / b.get(); }
};

template <class T>
struct Comparable {
    [[nodiscard]] friend constexpr bool operator==(T a, T b) noexcept {
        return a.get() == b.get();
    }
    [[nodiscard]] friend constexpr auto operator<=>(T a, T b) noexcept {
        return a.get() <=> b.get();
    }
};

template <class T>
struct Arithmetic : Addable<T>, Subtractable<T>, Negatable<T>, ScalarScalable<T> {};

template <class T>
struct Hashable {
    static constexpr bool ptl_is_hashable = true;
};

}  // namespace ptl

namespace std {
template <class T, class Tag, template <class> class... Skills>
    requires requires { ptl::NamedType<T, Tag, Skills...>::ptl_is_hashable; }
struct hash<ptl::NamedType<T, Tag, Skills...>> {
    [[nodiscard]] size_t operator()(
        const ptl::NamedType<T, Tag, Skills...>& v) const noexcept {
        return std::hash<T>{}(v.get());
    }
};
}  // namespace std
