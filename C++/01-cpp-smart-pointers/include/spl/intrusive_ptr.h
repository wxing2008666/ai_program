#ifndef SPL_INTRUSIVE_PTR_H
#define SPL_INTRUSIVE_PTR_H

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace spl {

class RefCounted {
public:
    void retain() const {
        refs_.fetch_add(1, std::memory_order_relaxed);
    }

    bool release() const {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
            return true;
        }
        return false;
    }

    long use_count() const {
        return refs_.load(std::memory_order_acquire);
    }

protected:
    RefCounted() : refs_(0) {}
    virtual ~RefCounted() {}

private:
    RefCounted(const RefCounted&);
    RefCounted& operator=(const RefCounted&);

    mutable std::atomic<long> refs_;
};

template <typename T>
class intrusive_ptr {
    static_assert(std::is_base_of<RefCounted, T>::value,
                  "intrusive_ptr<T> requires T to derive from RefCounted");

public:
    typedef T element_type;
    typedef T* pointer;

    intrusive_ptr() : p_(0) {}

    intrusive_ptr(std::nullptr_t) : p_(0) {}

    explicit intrusive_ptr(T* p, bool add_ref = true) : p_(p) {
        if (p_ && add_ref) {
            p_->retain();
        }
    }

    intrusive_ptr(const intrusive_ptr& other) : p_(other.p_) {
        if (p_) {
            p_->retain();
        }
    }

    intrusive_ptr(intrusive_ptr&& other) : p_(other.p_) {
        other.p_ = 0;
    }

    template <typename U,
              typename std::enable_if<std::is_convertible<U*, T*>::value, int>::type = 0>
    intrusive_ptr(const intrusive_ptr<U>& other) : p_(other.get()) {
        if (p_) {
            p_->retain();
        }
    }

    template <typename U,
              typename std::enable_if<std::is_convertible<U*, T*>::value, int>::type = 0>
    intrusive_ptr(intrusive_ptr<U>&& other) : p_(other.detach()) {}

    ~intrusive_ptr() {
        if (p_) {
            p_->release();
        }
    }

    intrusive_ptr& operator=(const intrusive_ptr& other) {
        intrusive_ptr(other).swap(*this);
        return *this;
    }

    intrusive_ptr& operator=(intrusive_ptr&& other) {
        intrusive_ptr(std::move(other)).swap(*this);
        return *this;
    }

    intrusive_ptr& operator=(std::nullptr_t) {
        reset();
        return *this;
    }

    template <typename U,
              typename std::enable_if<std::is_convertible<U*, T*>::value, int>::type = 0>
    intrusive_ptr& operator=(const intrusive_ptr<U>& other) {
        intrusive_ptr(other).swap(*this);
        return *this;
    }

    template <typename U,
              typename std::enable_if<std::is_convertible<U*, T*>::value, int>::type = 0>
    intrusive_ptr& operator=(intrusive_ptr<U>&& other) {
        intrusive_ptr(std::move(other)).swap(*this);
        return *this;
    }

    void reset() {
        intrusive_ptr().swap(*this);
    }

    void reset(T* p, bool add_ref = true) {
        if (p == p_) {
            return;
        }
        intrusive_ptr(p, add_ref).swap(*this);
    }

    T* detach() {
        T* result = p_;
        p_ = 0;
        return result;
    }

    T* get() const {
        return p_;
    }

    T& operator*() const {
        return *p_;
    }

    T* operator->() const {
        return p_;
    }

    explicit operator bool() const {
        return p_ != 0;
    }

    long use_count() const {
        return p_ ? p_->use_count() : 0;
    }

    void swap(intrusive_ptr& other) {
        T* temp = p_;
        p_ = other.p_;
        other.p_ = temp;
    }

private:
    T* p_;
};

template <typename T>
void swap(intrusive_ptr<T>& left, intrusive_ptr<T>& right) {
    left.swap(right);
}

template <typename T, typename... Args>
intrusive_ptr<T> make_intrusive(Args&&... args) {
    return intrusive_ptr<T>(new T(std::forward<Args>(args)...));
}

template <typename T, typename U>
intrusive_ptr<T> static_pointer_cast(const intrusive_ptr<U>& source) {
    return intrusive_ptr<T>(static_cast<T*>(source.get()));
}

template <typename T, typename U>
intrusive_ptr<T> dynamic_pointer_cast(const intrusive_ptr<U>& source) {
    T* target = dynamic_cast<T*>(source.get());
    if (!target) {
        return intrusive_ptr<T>();
    }
    return intrusive_ptr<T>(target);
}

template <typename T, typename U>
intrusive_ptr<T> const_pointer_cast(const intrusive_ptr<U>& source) {
    return intrusive_ptr<T>(const_cast<T*>(source.get()));
}

}

#endif
