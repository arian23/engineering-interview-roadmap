// Observer pattern (C++23)
//
// Idea: a subject keeps a list of observers and notifies all of them when something happens.
// The subject does not know what the observers are; observers can join and leave at any time.
// Theory: ../README.md#observer
//
// This implementation is a type-safe "signal" (as in Qt / Boost.Signals2), with the safety
// problems of the naive version solved:
//   * Dangling observers: subscribe() returns a move-only Subscription. Destroying it unsubscribes,
//     so an observer that goes out of scope can never be called again (RAII).
//   * Changing the list during a notification: emit() calls a SNAPSHOT of the observers, so a
//     callback may subscribe or unsubscribe (even itself) without invalidating the loop.
//   * Threads: the list is guarded by a mutex, and callbacks run OUTSIDE the lock, so a slow or
//     re-entrant callback cannot deadlock the subject.
//   * Lifetime of the subject: Subscription holds only a weak_ptr to the subject's state, so it is
//     safe even if the subject is destroyed first.
// A second approach, observe_weak(), ties an observer to a std::shared_ptr object's lifetime.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <utility>
#include <vector>

template <typename... Args>
class Signal {
    using Slot = std::function<void(const Args&...)>;

    struct State {
        std::mutex mutex;  // guards slots and next_id
        std::vector<std::pair<std::uint64_t, std::shared_ptr<Slot>>> slots;
        std::uint64_t next_id = 1;
    };

public:
    /// Unsubscribes when destroyed. Move-only, like std::unique_ptr.
    class Subscription {
    public:
        Subscription() = default;
        Subscription(std::weak_ptr<State> state, std::uint64_t id) : state_(std::move(state)), id_(id) {}
        Subscription(Subscription&& other) noexcept : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {}
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                reset();
                state_ = std::move(other.state_);
                id_ = std::exchange(other.id_, 0);
            }
            return *this;
        }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        ~Subscription() { reset(); }

        void reset() {
            if (id_ == 0) return;
            if (auto state = state_.lock()) {  // the subject may already be gone: then nothing to do
                std::lock_guard lock(state->mutex);
                std::erase_if(state->slots, [&](const auto& slot) { return slot.first == id_; });
            }
            id_ = 0;
        }
        [[nodiscard]] bool active() const { return id_ != 0 && !state_.expired(); }

    private:
        std::weak_ptr<State> state_;
        std::uint64_t id_ = 0;
    };

    [[nodiscard]] Subscription subscribe(Slot slot) {
        std::lock_guard lock(state_->mutex);
        const std::uint64_t id = state_->next_id++;
        state_->slots.emplace_back(id, std::make_shared<Slot>(std::move(slot)));
        return Subscription(state_, id);
    }

    /// Calls `method` on `object` for as long as the object is alive; no Subscription to manage.
    template <typename T>
    void observe_weak(const std::shared_ptr<T>& object, void (T::*method)(const Args&...)) {
        auto keep = std::make_shared<Subscription>();
        *keep = subscribe([weak = std::weak_ptr<T>(object), method, keep_weak = std::weak_ptr<Subscription>(keep)](const Args&... args) {
            if (auto target = weak.lock()) {
                ((*target).*method)(args...);
            } else if (auto self = keep_weak.lock()) {
                self->reset();  // the object died: remove this slot on the next notification
            }
        });
        weak_subscriptions_.push_back(std::move(keep));
    }

    void emit(const Args&... args) const {
        std::vector<std::shared_ptr<Slot>> snapshot;
        {
            std::lock_guard lock(state_->mutex);
            for (const auto& [id, slot] : state_->slots) snapshot.push_back(slot);
        }
        for (const auto& slot : snapshot) (*slot)(args...);  // outside the lock
    }

    [[nodiscard]] std::size_t observer_count() const {
        std::lock_guard lock(state_->mutex);
        return state_->slots.size();
    }

private:
    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::vector<std::shared_ptr<Subscription>> weak_subscriptions_;
};

// ===================================================================== example domain

struct PriceTick {
    std::string symbol;
    double price;
};

class StockTicker {  // the subject
public:
    Signal<PriceTick> on_price;
    void publish(std::string symbol, double price) { on_price.emit(PriceTick{std::move(symbol), price}); }
};

class PriceAlert {  // an observer object
public:
    PriceAlert(std::string symbol, double limit) : symbol_(std::move(symbol)), limit_(limit) {}
    void check(const PriceTick& t) {
        if (t.symbol == symbol_ && t.price > limit_) std::println("     [alert] {} above {:.2f}: {:.2f}", t.symbol, limit_, t.price);
    }

private:
    std::string symbol_;
    double limit_;
};

int main() {
    StockTicker ticker;

    std::println("1) Two observers subscribe; both are notified");
    auto logger = ticker.on_price.subscribe([](const PriceTick& t) { std::println("     [log]   {} = {:.2f}", t.symbol, t.price); });
    double total = 0;
    int count = 0;
    auto averager = ticker.on_price.subscribe([&](const PriceTick& t) {
        total += t.price;
        ++count;
    });
    ticker.publish("ERIC", 7.10);
    ticker.publish("NOK", 4.25);
    std::println("     observers: {}, average so far {:.3f}", ticker.on_price.observer_count(), total / count);

    std::println("\n2) RAII: an observer in an inner scope unsubscribes automatically when it ends");
    {
        auto temporary = ticker.on_price.subscribe([](const PriceTick& t) { std::println("     [temp]  saw {}", t.symbol); });
        std::println("     observers inside the scope: {}", ticker.on_price.observer_count());
        ticker.publish("ERIC", 7.20);
    }
    std::println("     observers after the scope: {}", ticker.on_price.observer_count());
    ticker.publish("ERIC", 7.30);  // [temp] is not called

    std::println("\n3) A one-shot observer that unsubscribes ITSELF during a notification");
    Signal<PriceTick>::Subscription once;
    once = ticker.on_price.subscribe([&](const PriceTick& t) {
        std::println("     [once]  first tick only: {}", t.symbol);
        once.reset();  // safe: emit() iterates over a snapshot
    });
    ticker.publish("NOK", 4.30);
    ticker.publish("NOK", 4.35);

    std::println("\n4) observe_weak: an observer object tied to a shared_ptr's lifetime");
    auto alert = std::make_shared<PriceAlert>("ERIC", 7.50);
    ticker.on_price.observe_weak(alert, &PriceAlert::check);
    ticker.publish("ERIC", 7.60);
    alert.reset();  // the object is destroyed; the signal must not call it
    std::println("     alert object destroyed");
    ticker.publish("ERIC", 7.70);  // no [alert], and its slot is cleaned up
    std::println("     observers now: {} (logger + averager)", ticker.on_price.observer_count());

    std::println("\n5) The subject is destroyed before its subscription: still safe");
    Signal<PriceTick>::Subscription orphan;
    {
        StockTicker short_lived;
        orphan = short_lived.on_price.subscribe([](const PriceTick&) {});
        std::println("     subscription active: {}", orphan.active());
    }
    std::println("     subscription active after subject died: {} (its destructor will do nothing)", orphan.active());
}
