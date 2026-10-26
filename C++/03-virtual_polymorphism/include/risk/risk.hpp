#ifndef RISK_RISK_HPP
#define RISK_RISK_HPP

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace risk {

enum class Decision { Allow, Review, Reject };
const char* to_string(Decision value);

struct Context {
    std::string merchant_id;
    std::string country;
    double amount = 0.0;
    int tx_count_1m = 0;
    int hour = 12;
    bool trusted_device = false;
};

struct Result {
    Decision decision;
    int score;
    std::string reason;

    Result(Decision d = Decision::Allow, int s = 0, std::string r = std::string())
        : decision(d), score(s), reason(std::move(r)) {}
};

class RuleVisitor;
class MaxAmountRule;
class CountryBlockRule;
class VelocityRule;
class CompositeRule;

// 业务接口：只暴露值语义的决策能力，不暴露实现细节。
class IRule {
public:
    virtual ~IRule() = default;

    virtual const char* id() const = 0;
    virtual Result evaluate(const Context& ctx) const = 0;

    // 原型模式：调用者负责 delete；协变返回类型让派生类返回更具体的指针。
    virtual IRule* clone() const = 0;

    // 访问者模式的双分派入口。
    virtual void accept(RuleVisitor& visitor) const = 0;
};

// 独立的能力接口；与 IRule 组合成多继承，演示 this 指针调整与 cross-cast。
class IAuditable {
public:
    virtual ~IAuditable() = default;
    virtual std::string audit() const = 0;
};

class RuleVisitor {
public:
    virtual ~RuleVisitor() = default;
    virtual void visit(const MaxAmountRule& rule) = 0;
    virtual void visit(const CountryBlockRule& rule) = 0;
    virtual void visit(const VelocityRule& rule) = 0;
    virtual void visit(const CompositeRule& rule) = 0;
};

inline std::unique_ptr<IRule> clone_rule(const IRule& rule) {
    return std::unique_ptr<IRule>(rule.clone());
}

// NVI：evaluate 是 public + final 的模板方法，doEvaluate 是 private 的定制点。
class RuleBase : public IRule {
public:
    ~RuleBase() override = default;
    Result evaluate(const Context& ctx) const final;

private:
    virtual Result doEvaluate(const Context& ctx) const = 0;
};

class MaxAmountRule final : public RuleBase, public IAuditable {
public:
    explicit MaxAmountRule(double limit = 5000.0);
    const char* id() const override;
    MaxAmountRule* clone() const override;
    void accept(RuleVisitor& visitor) const override;
    std::string audit() const override;
    double limit() const { return limit_; }

private:
    Result doEvaluate(const Context& ctx) const override;
    double limit_;
};

class CountryBlockRule final : public RuleBase {
public:
    CountryBlockRule();
    const char* id() const override;
    CountryBlockRule* clone() const override;
    void accept(RuleVisitor& visitor) const override;
    void block(const std::string& country);
    const std::vector<std::string>& blocked() const { return blocked_; }

private:
    Result doEvaluate(const Context& ctx) const override;
    std::vector<std::string> blocked_;
};

class VelocityRule final : public RuleBase {
public:
    explicit VelocityRule(int review_at = 20, int reject_at = 50,
                          double high_amount = 1000.0);
    const char* id() const override;
    VelocityRule* clone() const override;
    void accept(RuleVisitor& visitor) const override;

private:
    Result doEvaluate(const Context& ctx) const override;
    int review_at_;
    int reject_at_;
    double high_amount_;
};

// 组合模式：对子规则递归虚调用，对业务方仍表现为一个 IRule。
class CompositeRule final : public RuleBase, public IAuditable {
public:
    explicit CompositeRule(std::string id);
    const char* id() const override;
    CompositeRule* clone() const override;
    void accept(RuleVisitor& visitor) const override;
    std::string audit() const override;
    void add(std::unique_ptr<IRule> rule);
    const std::vector<std::unique_ptr<IRule>>& children() const { return children_; }

private:
    Result doEvaluate(const Context& ctx) const override;
    std::string id_;
    std::vector<std::unique_ptr<IRule>> children_;
};

// 插件注册表：保存原型对象，create() 通过 clone() 生产独立实例。
class RuleRegistry {
public:
    static RuleRegistry& instance();

    void add(const std::string& key, std::unique_ptr<IRule> prototype);
    std::unique_ptr<IRule> create(const std::string& key) const;
    std::vector<std::string> keys() const;

private:
    RuleRegistry() = default;
    RuleRegistry(const RuleRegistry&) = delete;
    RuleRegistry& operator=(const RuleRegistry&) = delete;

    std::map<std::string, std::unique_ptr<IRule>> prototypes_;
};

template <typename T, typename... Args>
class Registrar {
public:
    explicit Registrar(const char* key, Args... args) {
        RuleRegistry::instance().add(
            key, std::unique_ptr<IRule>(new T(std::forward<Args>(args)...)));
    }
};

struct Finding {
    std::string rule_id;
    Decision decision;
    int score;
    std::string reason;
};

struct Evaluation {
    Decision decision = Decision::Allow;
    int score = 0;
    std::vector<Finding> findings;
    std::vector<std::string> audits;
};

class RuleEngine {
public:
    void add(std::unique_ptr<IRule> rule);
    Evaluation run(const Context& ctx) const;
    std::size_t size() const { return rules_.size(); }

private:
    std::vector<std::unique_ptr<IRule>> rules_;
};

}  // namespace risk

#endif  // RISK_RISK_HPP
