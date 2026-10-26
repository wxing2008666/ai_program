struct Base {
    virtual ~Base() {}
    virtual void run(int) const {}
};

struct Derived : Base {
    void run(double) const override {}
};
