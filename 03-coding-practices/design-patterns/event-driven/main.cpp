// Event-driven architecture (C++23)
//
// Idea: components never call each other directly. A component that does something PUBLISHES an
// event ("OrderPlaced"); any number of other components SUBSCRIBE to the event types they care
// about and react. The publisher does not know who listens, so components can be added, removed
// or changed independently. Theory: ../README.md#event-driven-architecture
//
// Building blocks here:
//   * Event types: plain structs. The bus is TYPE-SAFE: handlers are keyed by std::type_index, and
//     a handler for OrderPlaced receives an OrderPlaced, never a cast or a string payload.
//   * EventBus with two delivery modes:
//       publish()  synchronous: handlers run immediately, on the caller's thread
//       post()     asynchronous: the event goes into a queue and an event-loop thread dispatches it,
//                  so the publisher never waits for slow handlers
//   * Handler failures are isolated: an exception in one handler is logged and does not stop the
//     others (or the loop).
//   * Chained events: a handler may post a NEW event (PaymentCaptured -> OrderShipped), which is how
//     a multi-step workflow (a "choreography") emerges without any central coordinator.
//
// Example: an online shop. Order service, payment, inventory, shipping, email and analytics are
// independent services wired together only through events.

#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <print>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

// ===================================================================== events

struct OrderPlaced { int order_id; std::string customer; double amount; };
struct PaymentCaptured { int order_id; double amount; };
struct PaymentFailed { int order_id; std::string reason; };
struct OrderShipped { int order_id; std::string tracking; };

// ===================================================================== event bus

class EventBus {
public:
    EventBus() : loop_([this](std::stop_token stop) { run(stop); }) {}
    ~EventBus() { shutdown(); }
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    /// Registers `handler` for events of type E. `name` is used in logs.
    template <typename E>
    void subscribe(std::string name, std::function<void(const E&)> handler) {
        std::lock_guard lock(mutex_);
        handlers_[std::type_index(typeid(E))].push_back(
            {std::move(name), [h = std::move(handler)](const std::any& event) { h(std::any_cast<const E&>(event)); }});
    }

    /// Synchronous: runs every handler now, on this thread.
    template <typename E>
    void publish(const E& event) {
        dispatch(std::type_index(typeid(E)), std::any(event));
    }

    /// Asynchronous: queues the event and returns immediately; the event loop delivers it.
    template <typename E>
    void post(E event) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            queue_.push_back({std::type_index(typeid(E)), std::any(std::move(event))});
            ++pending_;
        }
        cv_.notify_one();
    }

    /// Blocks until every posted event (including events posted by handlers) has been handled.
    void wait_idle() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return pending_ == 0; });
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
        }
        cv_.notify_all();
        if (loop_.joinable()) loop_.join();  // the loop drains the queue before exiting
    }

    [[nodiscard]] int handler_failures() const noexcept { return failures_.load(); }

private:
    struct Handler {
        std::string name;
        std::function<void(const std::any&)> call;
    };
    struct Queued {
        std::type_index type;
        std::any event;
    };

    void dispatch(std::type_index type, const std::any& event) {
        std::vector<Handler> snapshot;  // copy, so handlers can subscribe or post while running
        {
            std::lock_guard lock(mutex_);
            if (auto it = handlers_.find(type); it != handlers_.end()) snapshot = it->second;
        }
        for (const Handler& h : snapshot) {
            try {
                h.call(event);
            } catch (const std::exception& e) {  // one failing handler must not affect the others
                ++failures_;
                std::println("      !! handler '{}' failed: {}", h.name, e.what());
            }
        }
    }

    void run(std::stop_token) {
        for (;;) {
            Queued item{typeid(void), {}};
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;  // stopping and fully drained
                item = std::move(queue_.front());
                queue_.pop_front();
            }
            dispatch(item.type, item.event);
            {
                std::lock_guard lock(mutex_);
                if (--pending_ == 0) idle_.notify_all();
            }
        }
    }

    std::mutex mutex_;  // guards handlers_, queue_, pending_, stopping_
    std::condition_variable cv_, idle_;
    std::unordered_map<std::type_index, std::vector<Handler>> handlers_;
    std::deque<Queued> queue_;
    int pending_ = 0;
    bool stopping_ = false;
    std::atomic<int> failures_{0};
    std::jthread loop_;  // last member: started after everything above is initialised
};

// ===================================================================== independent services

std::mutex log_mutex;
template <typename... A>
void log(std::format_string<A...> f, A&&... a) {
    std::lock_guard lock(log_mutex);
    std::println(f, std::forward<A>(a)...);
}

class PaymentService {
public:
    explicit PaymentService(EventBus& bus) : bus_(bus) {
        bus.subscribe<OrderPlaced>("payment", [this](const OrderPlaced& e) { charge(e); });
    }

private:
    void charge(const OrderPlaced& e) {
        std::this_thread::sleep_for(20ms);  // talking to a payment provider
        if (e.amount > 1000) {
            log("   [payment]   order {} declined", e.order_id);
            bus_.post(PaymentFailed{e.order_id, "amount over card limit"});
        } else {
            log("   [payment]   order {} charged ${:.2f}", e.order_id, e.amount);
            bus_.post(PaymentCaptured{e.order_id, e.amount});
        }
    }
    EventBus& bus_;
};

class InventoryService {
public:
    explicit InventoryService(EventBus& bus) {
        bus.subscribe<OrderPlaced>("inventory", [](const OrderPlaced& e) { log("   [inventory] reserved items for order {}", e.order_id); });
        bus.subscribe<PaymentFailed>("inventory", [](const PaymentFailed& e) { log("   [inventory] released items for order {}", e.order_id); });
    }
};

class ShippingService {
public:
    explicit ShippingService(EventBus& bus) : bus_(bus) {
        bus.subscribe<PaymentCaptured>("shipping", [this](const PaymentCaptured& e) {
            const std::string tracking = std::format("TRK-{:05}", e.order_id * 37);
            log("   [shipping]  order {} shipped, tracking {}", e.order_id, tracking);
            bus_.post(OrderShipped{e.order_id, tracking});
        });
    }

private:
    EventBus& bus_;
};

class EmailService {
public:
    explicit EmailService(EventBus& bus) {
        bus.subscribe<OrderShipped>("email", [](const OrderShipped& e) { log("   [email]     'your order {} is on its way ({})'", e.order_id, e.tracking); });
        bus.subscribe<PaymentFailed>("email", [](const PaymentFailed& e) {
            log("   [email]     'payment for order {} failed: {}'", e.order_id, e.reason);
        });
    }
};

class Analytics {  // added later: no other service had to change
public:
    explicit Analytics(EventBus& bus) {
        bus.subscribe<OrderPlaced>("analytics", [this](const OrderPlaced& e) {
            std::lock_guard lock(m_);
            revenue_by_customer_[e.customer] += e.amount;
        });
        bus.subscribe<OrderPlaced>("fraud-check", [](const OrderPlaced& e) {
            if (e.customer == "mallory") throw std::runtime_error("fraud scoring service unavailable");
        });
    }
    void print() {
        std::lock_guard lock(m_);
        for (const auto& [customer, total] : revenue_by_customer_) std::println("   [analytics] {:<8} ${:>8.2f}", customer, total);
    }

private:
    std::mutex m_;
    std::map<std::string, double> revenue_by_customer_;
};

// ===================================================================== demo

int main() {
    EventBus bus;
    PaymentService payment(bus);
    InventoryService inventory(bus);
    ShippingService shipping(bus);
    EmailService email(bus);
    Analytics analytics(bus);

    std::println("1) The order service only posts OrderPlaced; everything else follows from events");
    const auto start = std::chrono::steady_clock::now();
    bus.post(OrderPlaced{1, "alice", 59.90});
    bus.post(OrderPlaced{2, "bob", 1450.00});   // will be declined
    bus.post(OrderPlaced{3, "mallory", 12.00}); // the fraud-check handler throws, the rest still runs
    const auto posted = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);
    log("   (order service: 3 orders posted in {:.0f} µs, without waiting for any handler)", posted.count());

    bus.wait_idle();
    std::println("\n2) Results");
    analytics.print();
    std::println("   handler failures isolated: {}", bus.handler_failures());

    std::println("\n3) Synchronous publish(): handlers run on the caller's thread before publish() returns");
    bus.publish(PaymentFailed{4, "card expired"});
    std::println("   publish() returned");
    bus.shutdown();
}
