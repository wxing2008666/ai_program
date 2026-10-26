struct Base {
    virtual ~Base() {}
    virtual void handle(int) {}
};

struct Derived : Base {
    void handle(const char*) {}
};

void call(Derived& derived) {
    derived.handle(42);
}
