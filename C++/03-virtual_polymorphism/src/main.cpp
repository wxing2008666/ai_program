#include "demo/abi_probe.hpp"
#include "risk/risk.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace {

void section(const char* title) {
    std::cout << "\n=== " << title << " ===\n";
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("check failed: " + message);
    }
}

void print_evaluation(const risk::Evaluation& evaluation) {
    std::cout << "  result=" << risk::to_string(evaluation.decision)
              << " score=" << evaluation.score << "\n";
    for (const auto& finding : evaluation.findings) {
        std::cout << "  - " << finding.rule_id << " => "
                  << risk::to_string(finding.decision) << " (+"
                  << finding.score << "): " << finding.reason << "\n";
    }
    for (const auto& audit : evaluation.audits) {
        std::cout << "  audit: " << audit << "\n";
    }
}

class RuleReporter : public risk::RuleVisitor {
public:
    explicit RuleReporter(std::ostream& out) : out_(out) {}

    void visit(const risk::MaxAmountRule& rule) override {
        line() << "MaxAmount(limit=" << rule.limit() << ")\n";
    }

    void visit(const risk::CountryBlockRule& rule) override {
        line() << "CountryBlock(blocked=" << rule.blocked().size() << ")\n";
    }

    void visit(const risk::VelocityRule& rule) override {
        line() << "Velocity(" << rule.id() << ")\n";
    }

    void visit(const risk::CompositeRule& rule) override {
        line() << "Composite(" << rule.id() << ", children="
               << rule.children().size() << ")\n";
        ++depth_;
        for (const auto& child : rule.children()) {
            child->accept(*this);
        }
        --depth_;
    }

private:
    std::ostream& line() {
        out_ << std::string(static_cast<std::size_t>(depth_) * 2, ' ');
        return out_;
    }

    std::ostream& out_;
    int depth_ = 0;
};

std::unique_ptr<risk::IRule> make_payment_guard() {
    std::unique_ptr<risk::CompositeRule> root(
        new risk::CompositeRule("payment_guard"));
    root->add(risk::RuleRegistry::instance().create("max_amount"));
    root->add(risk::RuleRegistry::instance().create("country_block"));
    root->add(risk::RuleRegistry::instance().create("velocity"));
    return std::unique_ptr<risk::IRule>(root.release());
}

risk::Context make_context(const std::string& country, double amount, int count) {
    risk::Context ctx;
    ctx.merchant_id = "m_1001";
    ctx.country = country;
    ctx.amount = amount;
    ctx.tx_count_1m = count;
    ctx.trusted_device = false;
    return ctx;
}

void run_engine_demo() {
    section("1. rule engine: prototype registry + composite + audit");

    const std::vector<std::string> keys = risk::RuleRegistry::instance().keys();
    std::cout << "registered:";
    for (const auto& key : keys) {
        std::cout << ' ' << key;
    }
    std::cout << "\n";

    std::unique_ptr<risk::IRule> root = make_payment_guard();
    RuleReporter reporter(std::cout);
    root->accept(reporter);

    risk::RuleEngine engine;
    engine.add(std::move(root));
    std::cout << "engine rules=" << engine.size() << "\n";

    std::vector<risk::Context> cases;
    cases.push_back(make_context("SG", 120.0, 2));
    cases.push_back(make_context("SG", 8000.0, 2));
    cases.push_back(make_context("IR", 120.0, 2));
    cases.push_back(make_context("US", 2000.0, 60));
    cases.push_back(make_context("US", 500.0, 25));

    for (std::size_t i = 0; i < cases.size(); ++i) {
        std::cout << "\ncase " << (i + 1) << ": country=" << cases[i].country
                  << " amount=" << cases[i].amount
                  << " tx_1m=" << cases[i].tx_count_1m << "\n";
        print_evaluation(engine.run(cases[i]));
    }
}

void run_rtti_demo() {
    section("2. RTTI: typeid, cross-cast, identity");

    risk::RuleRegistry& registry = risk::RuleRegistry::instance();
    std::unique_ptr<risk::IRule> max = registry.create("max_amount");
    std::unique_ptr<risk::IRule> country = registry.create("country_block");
    risk::IRule* base = max.get();

    std::cout << "typeid(IRule)     = "
              << demo::demangle(typeid(risk::IRule).name()) << "\n";
    std::cout << "typeid(*base)     = "
              << demo::demangle(typeid(*base).name()) << "\n";
    require(typeid(*base) == typeid(risk::MaxAmountRule), "dynamic typeid");
    std::cout << "type_index hash   = "
              << std::type_index(typeid(*base)).hash_code() << "\n";

    risk::MaxAmountRule* concrete = dynamic_cast<risk::MaxAmountRule*>(base);
    require(concrete != nullptr, "downcast to MaxAmountRule");
    require(dynamic_cast<risk::CountryBlockRule*>(base) == nullptr,
            "wrong downcast returns null");

    risk::IAuditable* auditable = dynamic_cast<risk::IAuditable*>(base);
    require(auditable != nullptr, "cross-cast to IAuditable");
    const std::ptrdiff_t offset = static_cast<std::ptrdiff_t>(
        reinterpret_cast<std::intptr_t>(auditable) -
        reinterpret_cast<std::intptr_t>(base));
    std::cout << "IRule* -> IAuditable* offset = " << offset << "\n";
    std::cout << "audit() = " << auditable->audit() << "\n";

    void* most = dynamic_cast<void*>(base);
    require(most == dynamic_cast<void*>(auditable),
            "most-derived pointer from both bases");
    require(most == static_cast<void*>(base), "IRule is the primary base");
    std::cout << "most-derived identity = " << (most == static_cast<void*>(base))
              << "\n";

    require(dynamic_cast<risk::IAuditable*>(country.get()) == nullptr,
            "CountryBlockRule is not auditable");
    std::cout << "CountryBlockRule cross-cast = null (expected)\n";

    try {
        risk::IAuditable& bad = dynamic_cast<risk::IAuditable&>(*country);
        (void)bad;
        require(false, "bad_cast expected");
    } catch (const std::bad_cast&) {
        std::cout << "reference cross-cast threw std::bad_cast (expected)\n";
    }

    risk::IRule* null_rule = nullptr;
    require(dynamic_cast<risk::IAuditable*>(null_rule) == nullptr,
            "null dynamic_cast returns null");
    std::cout << "typeid(pointer)   = "
              << demo::demangle(typeid(null_rule).name()) << " (static type)\n";
    try {
        const std::type_info& info = typeid(*null_rule);
        (void)info;
        require(false, "bad_typeid expected");
    } catch (const std::bad_typeid&) {
        std::cout << "typeid(*null polymorphic ptr) threw bad_typeid (expected)\n";
    }
}

struct ArgBase {
    virtual ~ArgBase() {}
    virtual int adjust(int x = 10) const { return x; }
};

struct ArgDerived : ArgBase {
    int adjust(int x = 20) const override { return x * 2; }
};

void run_default_arg_demo() {
    section("3. default arguments are bound statically");

    ArgDerived derived;
    ArgBase& base = derived;
    std::cout << "base.adjust()    = " << base.adjust()
              << "  (default 10 from ArgBase, body from ArgDerived)\n";
    std::cout << "derived.adjust() = " << derived.adjust()
              << "  (default 20 from ArgDerived)\n";
    require(base.adjust() == 20, "default arg from static type");
}

struct DispatchBase {
    DispatchBase() { std::cout << "  Base ctor -> " << who() << "\n"; }
    virtual ~DispatchBase() { std::cout << "  Base dtor -> " << who() << "\n"; }
    virtual const char* who() const { return "Base"; }
};

struct DispatchDerived : DispatchBase {
    DispatchDerived() { std::cout << "  Derived ctor -> " << who() << "\n"; }
    ~DispatchDerived() override {
        std::cout << "  Derived dtor -> " << who() << "\n";
    }
    const char* who() const override { return "Derived"; }
};

void run_ctor_dtor_demo() {
    section("4. virtual calls during construction/destruction");
    std::cout << "constructing:\n";
    DispatchDerived object;
    std::cout << "destroying:\n";
}

struct SliceBase {
    virtual ~SliceBase() {}
    virtual const char* label() const { return "SliceBase"; }
    int base_value = 1;
};

struct SliceDerived : SliceBase {
    const char* label() const override { return "SliceDerived"; }
    int derived_value = 2;
};

void consume_by_value(SliceBase value) {
    std::cout << "  by value: label=" << value.label()
              << " base=" << value.base_value << " sizeof=" << sizeof(value)
              << "\n";
}

void run_slicing_demo() {
    section("5. object slicing");
    SliceDerived derived;
    consume_by_value(derived);
    SliceBase& ref = derived;
    std::cout << "  by ref  : label=" << ref.label() << " sizeof=" << sizeof(ref)
              << "\n";
    std::cout << "sizeof(SliceBase)=" << sizeof(SliceBase)
              << " sizeof(SliceDerived)=" << sizeof(SliceDerived) << "\n";
    require(std::string(ref.label()) == "SliceDerived", "reference preserves type");
}

struct HideBase {
    virtual ~HideBase() {}
    virtual void handle(int value) const {
        std::cout << "  HideBase::handle(" << value << ")\n";
    }
};

struct HideDerived : HideBase {
    using HideBase::handle;
    void handle(const std::string& text) const {
        std::cout << "  HideDerived::handle(" << text << ")\n";
    }
};

void run_hiding_demo() {
    section("6. overload hiding and using-declaration");
    HideDerived object;
    object.handle(7);
    object.handle(std::string("text"));
    HideBase& base = object;
    base.handle(7);
}

struct PureDtorBase {
    virtual ~PureDtorBase() = 0;
};

PureDtorBase::~PureDtorBase() {
    std::cout << "  ~PureDtorBase() (pure virtual but defined)\n";
}

struct PureDtorDerived : PureDtorBase {
    ~PureDtorDerived() override {
        std::cout << "  ~PureDtorDerived()\n";
    }
};

void run_destructor_demo() {
    section("7. virtual destructor and pure virtual destructor");

    static_assert(std::has_virtual_destructor<risk::IRule>::value,
                  "IRule must have a virtual destructor");
    static_assert(std::is_polymorphic<risk::IRule>::value,
                  "IRule must be polymorphic");
    static_assert(std::is_abstract<risk::IRule>::value,
                  "IRule must be abstract");
    static_assert(std::is_base_of<risk::IAuditable, risk::MaxAmountRule>::value,
                  "MaxAmountRule implements IAuditable");

    std::cout << "IRule has virtual destructor = "
              << std::has_virtual_destructor<risk::IRule>::value << "\n";

    {
        std::unique_ptr<risk::IRule> owned =
            risk::RuleRegistry::instance().create("max_amount");
        std::cout << "deleting MaxAmountRule through IRule*:\n";
    }

    {
        std::unique_ptr<PureDtorBase> owned(new PureDtorDerived());
        std::cout << "deleting PureDtorDerived through a pure virtual base:\n";
    }
}

}  // namespace

int main() {
    try {
        std::cout << std::boolalpha;
        run_engine_demo();
        run_rtti_demo();
        run_default_arg_demo();
        run_ctor_dtor_demo();
        run_slicing_demo();
        run_hiding_demo();
        run_destructor_demo();
        demo::run_abi_probe();
        std::cout << "\nAll checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << "\n";
        return 1;
    }
}
