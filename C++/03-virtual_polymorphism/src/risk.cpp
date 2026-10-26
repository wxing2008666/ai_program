#include "risk/risk.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace risk {

const char* to_string(Decision value) {
    switch (value) {
        case Decision::Allow:  return "ALLOW";
        case Decision::Review: return "REVIEW";
        case Decision::Reject: return "REJECT";
    }
    return "UNKNOWN";
}

Result RuleBase::evaluate(const Context& ctx) const {
    // 非虚外壳可以放日志、指标、参数校验等公共逻辑；派生类只定制 doEvaluate。
    Result result = doEvaluate(ctx);
    if (result.reason.empty()) {
        result.reason = "ok";
    }
    return result;
}

MaxAmountRule::MaxAmountRule(double limit) : limit_(limit) {}

const char* MaxAmountRule::id() const { return "max_amount"; }

Result MaxAmountRule::doEvaluate(const Context& ctx) const {
    if (ctx.amount > limit_) {
        return Result(Decision::Reject, 60,
                      "amount " + std::to_string(ctx.amount) + " exceeds limit " +
                          std::to_string(limit_));
    }
    return Result(Decision::Allow, 0, "amount within limit");
}

MaxAmountRule* MaxAmountRule::clone() const { return new MaxAmountRule(*this); }

void MaxAmountRule::accept(RuleVisitor& visitor) const { visitor.visit(*this); }

std::string MaxAmountRule::audit() const {
    std::ostringstream oss;
    oss << "max_amount(limit=" << limit_ << ")";
    return oss.str();
}

CountryBlockRule::CountryBlockRule() {
    blocked_.push_back("KP");
    blocked_.push_back("IR");
}

const char* CountryBlockRule::id() const { return "country_block"; }

void CountryBlockRule::block(const std::string& country) {
    blocked_.push_back(country);
}

Result CountryBlockRule::doEvaluate(const Context& ctx) const {
    if (std::find(blocked_.begin(), blocked_.end(), ctx.country) != blocked_.end()) {
        return Result(Decision::Reject, 100, "blocked country: " + ctx.country);
    }
    return Result(Decision::Allow, 0, "country allowed");
}

CountryBlockRule* CountryBlockRule::clone() const {
    return new CountryBlockRule(*this);
}

void CountryBlockRule::accept(RuleVisitor& visitor) const { visitor.visit(*this); }

VelocityRule::VelocityRule(int review_at, int reject_at, double high_amount)
    : review_at_(review_at), reject_at_(reject_at), high_amount_(high_amount) {}

const char* VelocityRule::id() const { return "velocity"; }

Result VelocityRule::doEvaluate(const Context& ctx) const {
    if (ctx.tx_count_1m >= reject_at_ && ctx.amount >= high_amount_) {
        return Result(Decision::Reject, 80,
                      "high velocity " + std::to_string(ctx.tx_count_1m) +
                          " with amount " + std::to_string(ctx.amount));
    }
    if (ctx.tx_count_1m >= review_at_) {
        return Result(Decision::Review, 30,
                      "velocity " + std::to_string(ctx.tx_count_1m) + " needs review");
    }
    return Result(Decision::Allow, 0, "velocity normal");
}

VelocityRule* VelocityRule::clone() const { return new VelocityRule(*this); }

void VelocityRule::accept(RuleVisitor& visitor) const { visitor.visit(*this); }

CompositeRule::CompositeRule(std::string id) : id_(std::move(id)) {}

const char* CompositeRule::id() const { return id_.c_str(); }

void CompositeRule::add(std::unique_ptr<IRule> rule) {
    if (!rule) {
        throw std::invalid_argument("CompositeRule::add: null rule");
    }
    children_.push_back(std::move(rule));
}

Result CompositeRule::doEvaluate(const Context& ctx) const {
    Decision worst = Decision::Allow;
    int total_score = 0;
    std::string reason = "all rules passed";

    for (const auto& child : children_) {
        const Result result = child->evaluate(ctx);
        total_score += result.score;

        if (result.decision == Decision::Reject) {
            worst = Decision::Reject;
            reason = std::string(child->id()) + ": " + result.reason;
        } else if (result.decision == Decision::Review && worst != Decision::Reject) {
            worst = Decision::Review;
            reason = std::string(child->id()) + ": " + result.reason;
        }
    }
    return Result(worst, total_score, reason);
}

CompositeRule* CompositeRule::clone() const {
    std::unique_ptr<CompositeRule> copy(new CompositeRule(id_));
    for (const auto& child : children_) {
        copy->add(clone_rule(*child));
    }
    return copy.release();
}

void CompositeRule::accept(RuleVisitor& visitor) const { visitor.visit(*this); }

std::string CompositeRule::audit() const {
    return "composite(" + id_ + ", children=" +
           std::to_string(children_.size()) + ")";
}

RuleRegistry& RuleRegistry::instance() {
    static RuleRegistry registry;
    return registry;
}

void RuleRegistry::add(const std::string& key, std::unique_ptr<IRule> prototype) {
    if (!prototype) {
        throw std::invalid_argument("RuleRegistry::add: null prototype");
    }
    const auto inserted =
        prototypes_.insert(std::make_pair(key, std::move(prototype)));
    if (!inserted.second) {
        throw std::runtime_error("duplicate rule key: " + key);
    }
}

std::unique_ptr<IRule> RuleRegistry::create(const std::string& key) const {
    const auto it = prototypes_.find(key);
    if (it == prototypes_.end()) {
        throw std::runtime_error("unknown rule key: " + key);
    }
    return std::unique_ptr<IRule>(it->second->clone());
}

std::vector<std::string> RuleRegistry::keys() const {
    std::vector<std::string> result;
    result.reserve(prototypes_.size());
    for (const auto& entry : prototypes_) {
        result.push_back(entry.first);
    }
    return result;
}

void RuleEngine::add(std::unique_ptr<IRule> rule) {
    if (!rule) {
        throw std::invalid_argument("RuleEngine::add: null rule");
    }
    rules_.push_back(std::move(rule));
}

Evaluation RuleEngine::run(const Context& ctx) const {
    Evaluation evaluation;

    for (const auto& rule : rules_) {
        const Result result = rule->evaluate(ctx);
        evaluation.score += result.score;

        Finding finding;
        finding.rule_id = rule->id();
        finding.decision = result.decision;
        finding.score = result.score;
        finding.reason = result.reason;
        evaluation.findings.push_back(finding);

        if (result.decision == Decision::Reject) {
            evaluation.decision = Decision::Reject;
        } else if (result.decision == Decision::Review &&
                   evaluation.decision != Decision::Reject) {
            evaluation.decision = Decision::Review;
        }

        // 关键：从 IRule* 跨继承层次转到同源的 IAuditable*。
        if (const IAuditable* auditable = dynamic_cast<const IAuditable*>(rule.get())) {
            evaluation.audits.push_back(auditable->audit());
        }
    }
    return evaluation;
}

namespace {
// 静态对象在 main 之前完成注册；构造顺序由同一个 TU 内的定义顺序保证。
Registrar<MaxAmountRule, double> g_max_amount("max_amount", 5000.0);
Registrar<CountryBlockRule> g_country_block("country_block");
Registrar<VelocityRule> g_velocity("velocity");
}  // namespace

}  // namespace risk
