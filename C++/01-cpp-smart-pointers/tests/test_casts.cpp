#include <memory>
#include <type_traits>

#include "spl/plugin.h"
#include "spl/tracked.h"
#include "test_framework.h"

namespace {

struct DerivedCastTag {};

struct NonVirtualBase {
    NonVirtualBase() : base_value(3) {}
    ~NonVirtualBase() {}
    int base_value;
};

struct DerivedCast : public NonVirtualBase, public spl::Tracked<DerivedCastTag> {
    DerivedCast() : derived_value(5) {}
    ~DerivedCast() {}
    int derived_value;
};

struct VirtualBase {
    VirtualBase() : base_value(7) {}
    virtual ~VirtualBase() {}
    int base_value;
};

struct DerivedVirtual : public VirtualBase {
    DerivedVirtual() : derived_value(9) {}
    int derived_value;
};

class ConstructorProbe : public std::enable_shared_from_this<ConstructorProbe> {
public:
    ConstructorProbe() : threw(false) {
        try {
            shared_from_this();
        } catch (const std::bad_weak_ptr&) {
            threw = true;
        }
    }

    bool threw;
};

}

SPL_TEST(shared_ptr_conversion_keeps_the_original_deleter) {
    spl::Tracked<DerivedCastTag>::reset_counters();
    {
        std::shared_ptr<DerivedCast> derived = std::make_shared<DerivedCast>();
        std::shared_ptr<NonVirtualBase> base = derived;
        SPL_CHECK_EQ(base.use_count(), 2);
        SPL_CHECK_EQ(base->base_value, 3);

        derived.reset();
        SPL_CHECK_EQ(spl::Tracked<DerivedCastTag>::alive(), 1);
    }
    SPL_CHECK_EQ(spl::Tracked<DerivedCastTag>::alive(), 0);
}

SPL_TEST(unique_ptr_polymorphism_requires_a_virtual_destructor) {
    static_assert(std::has_virtual_destructor<VirtualBase>::value,
                  "VirtualBase must expose a virtual destructor");
    std::unique_ptr<VirtualBase> base(new DerivedVirtual());
    SPL_CHECK_EQ(base->base_value, 7);
    base.reset();
}

SPL_TEST(shared_ptr_dynamic_casts_preserve_ownership) {
    std::shared_ptr<spl::Plugin> plugin = std::make_shared<spl::InvertPlugin>();
    std::shared_ptr<spl::InvertPlugin> invert =
        std::dynamic_pointer_cast<spl::InvertPlugin>(plugin);
    std::shared_ptr<spl::ThresholdPlugin> threshold =
        std::dynamic_pointer_cast<spl::ThresholdPlugin>(plugin);

    SPL_CHECK(invert);
    SPL_CHECK(!threshold);
    SPL_CHECK_EQ(invert.use_count(), 2);
    SPL_CHECK_EQ(invert->name(), std::string("invert"));
}

SPL_TEST(shared_from_this_is_unavailable_in_the_constructor) {
    std::shared_ptr<ConstructorProbe> probe = std::make_shared<ConstructorProbe>();
    SPL_CHECK(probe->threw);
}
