struct VBase {
    virtual ~VBase() {}
};

struct Mid : virtual VBase {};
struct Derived : Mid {};

Derived* downcast(VBase* base) {
    return static_cast<Derived*>(base);
}
