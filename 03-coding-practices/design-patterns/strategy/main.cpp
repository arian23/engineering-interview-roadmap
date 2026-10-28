// Strategy pattern (C++23)
//
// Idea: put each variant of an algorithm behind one interface, and let the caller choose
// which one to use. The code that USES the algorithm (the "context") never changes when a
// new variant is added. Theory: ../README.md#strategy
//
// Example: a checkout that calculates shipping cost. Three ways to express a strategy in C++:
//   1. Classic: an abstract interface + std::unique_ptr  (choose and swap at run time)
//   2. Lightweight: std::function                        (any lambda / function works)
//   3. Compile time: a template parameter + a concept    (zero overhead, fixed at compile time)

#include <concepts>
#include <functional>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct Order {
    double subtotal;     // dollars
    double weight_kg;
    std::string country;
};

// -------------------------------------------------------------------- 1. classic strategy

class ShippingStrategy {
public:
    virtual ~ShippingStrategy() = default;
    [[nodiscard]] virtual double cost(const Order& order) const = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
};

class FlatRate final : public ShippingStrategy {
public:
    explicit FlatRate(double price) : price_(price) {}
    double cost(const Order&) const override { return price_; }
    std::string_view name() const override { return "flat rate"; }

private:
    double price_;
};

class ByWeight final : public ShippingStrategy {
public:
    double cost(const Order& order) const override { return 4.0 + 2.5 * order.weight_kg; }
    std::string_view name() const override { return "by weight"; }
};

class FreeOverThreshold final : public ShippingStrategy {
public:
    // Decorates another strategy: free above a threshold, otherwise delegate.
    FreeOverThreshold(double threshold, std::unique_ptr<ShippingStrategy> fallback)
        : threshold_(threshold), fallback_(std::move(fallback)) {}
    double cost(const Order& order) const override {
        return order.subtotal >= threshold_ ? 0.0 : fallback_->cost(order);
    }
    std::string_view name() const override { return "free over threshold"; }

private:
    double threshold_;
    std::unique_ptr<ShippingStrategy> fallback_;
};

// The context: depends only on the interface, not on any concrete strategy.
class Checkout {
public:
    explicit Checkout(std::unique_ptr<ShippingStrategy> strategy) : strategy_(std::move(strategy)) {}
    void set_strategy(std::unique_ptr<ShippingStrategy> strategy) { strategy_ = std::move(strategy); }

    [[nodiscard]] double total(const Order& order) const { return order.subtotal + strategy_->cost(order); }
    [[nodiscard]] std::string_view strategy_name() const { return strategy_->name(); }

private:
    std::unique_ptr<ShippingStrategy> strategy_;
};

// -------------------------------------------------------------------- 2. std::function strategy

using ShippingRule = std::function<double(const Order&)>;

double total_with(const Order& order, const ShippingRule& rule) { return order.subtotal + rule(order); }

// -------------------------------------------------------------------- 3. compile-time strategy (policy)

template <typename P>
concept ShippingPolicy = requires(const P& policy, const Order& order) {
    { policy.cost(order) } -> std::convertible_to<double>;
};

struct DomesticPolicy {
    double cost(const Order& o) const { return o.weight_kg <= 2.0 ? 5.0 : 9.0; }
};
struct InternationalPolicy {
    double cost(const Order& o) const { return 15.0 + 4.0 * o.weight_kg; }
};

// The strategy is a template parameter: the call is resolved at compile time and can be inlined.
template <ShippingPolicy Policy>
class StaticCheckout {
public:
    [[nodiscard]] double total(const Order& order) const { return order.subtotal + policy_.cost(order); }

private:
    [[no_unique_address]] Policy policy_;  // stateless policy: takes no space
};

// -------------------------------------------------------------------- demo

int main() {
    const std::vector<Order> orders{{30.0, 1.0, "CA"}, {120.0, 3.5, "CA"}, {45.0, 6.0, "FR"}};

    std::println("1) Classic Strategy: the Checkout code stays the same, only the strategy changes");
    Checkout checkout(std::make_unique<FlatRate>(7.99));
    for (auto make : std::vector<std::function<std::unique_ptr<ShippingStrategy>()>>{
             [] { return std::make_unique<FlatRate>(7.99); },
             [] { return std::make_unique<ByWeight>(); },
             [] { return std::make_unique<FreeOverThreshold>(100.0, std::make_unique<ByWeight>()); }}) {
        checkout.set_strategy(make());  // swapped at run time
        std::print("   {:<20}", checkout.strategy_name());
        for (const Order& o : orders) std::print("  ${:>7.2f}", checkout.total(o));
        std::println("");
    }

    std::println("\n2) std::function: a strategy is just any callable");
    const double tax_rate = 0.13;
    const ShippingRule express = [](const Order& o) { return 20.0 + 3.0 * o.weight_kg; };
    const ShippingRule pickup = [](const Order&) { return 0.0; };
    const ShippingRule taxed_express = [&](const Order& o) { return express(o) * (1.0 + tax_rate); };  // compose
    for (auto [name, rule] : {std::pair{"express", express}, {"store pickup", pickup}, {"express + 13% tax", taxed_express}}) {
        std::print("   {:<20}", name);
        for (const Order& o : orders) std::print("  ${:>7.2f}", total_with(o, rule));
        std::println("");
    }

    std::println("\n3) Compile-time policy: chosen by type, no virtual call");
    StaticCheckout<DomesticPolicy> domestic;
    StaticCheckout<InternationalPolicy> international;
    for (const Order& o : orders) {
        const double t = o.country == "CA" ? domestic.total(o) : international.total(o);
        std::println("   order to {} ({:.1f} kg): ${:.2f}", o.country, o.weight_kg, t);
    }
    std::println("   sizeof(StaticCheckout<DomesticPolicy>) = {} byte (empty policy, [[no_unique_address]])",
                 sizeof(StaticCheckout<DomesticPolicy>));
}
