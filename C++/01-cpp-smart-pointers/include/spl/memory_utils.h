#ifndef SPL_MEMORY_UTILS_H
#define SPL_MEMORY_UTILS_H

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace spl {

template <typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

template <typename T>
std::unique_ptr<T[]> make_unique_array(std::size_t count) {
    return std::unique_ptr<T[]>(new T[count]());
}

template <typename T, typename U>
bool same_owner(const std::shared_ptr<T>& left, const std::shared_ptr<U>& right) {
    return !left.owner_before(right) && !right.owner_before(left);
}

template <typename T, typename U>
bool same_owner(const std::weak_ptr<T>& left, const std::weak_ptr<U>& right) {
    return !left.owner_before(right) && !right.owner_before(left);
}

template <typename T, typename U>
bool same_owner(const std::shared_ptr<T>& left, const std::weak_ptr<U>& right) {
    return !left.owner_before(right) && !right.owner_before(left);
}

template <typename T, typename U>
bool same_owner(const std::weak_ptr<T>& left, const std::shared_ptr<U>& right) {
    return !left.owner_before(right) && !right.owner_before(left);
}

}

#endif
