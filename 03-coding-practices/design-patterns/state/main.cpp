// State pattern (C++23)
//
// Idea: an object's behaviour depends on its current state. Instead of one big
// `switch (state)` repeated in every method, each state is its own type that knows how to
// handle each event and which state comes next. Theory: ../README.md#state
//
// Two implementations:
//   1. Classic (GoF): an abstract State class; the context delegates to the current state object.
//      Example: a document workflow  Draft -> InReview -> Published.
//   2. Modern C++: states are plain structs in a std::variant, transitions are overloaded functions
//      chosen by std::visit. States carry their own data, and invalid transitions are explicit.
//      Example: a network connection  Disconnected -> Connecting -> Connected.

#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ===================================================================== 1. classic State

class Document;

class DocumentState {
public:
    virtual ~DocumentState() = default;
    virtual void edit(Document& doc, std::string_view text) const;
    virtual void submit(Document& doc) const;
    virtual void approve(Document& doc) const;
    virtual void reject(Document& doc) const;
    [[nodiscard]] virtual std::string_view name() const = 0;

protected:
    void refuse(std::string_view action) const { std::println("     x '{}' is not allowed while {}", action, name()); }
};

class Document {
public:
    Document();
    void edit(std::string_view text) { state_->edit(*this, text); }
    void submit() { state_->submit(*this); }
    void approve() { state_->approve(*this); }
    void reject() { state_->reject(*this); }

    // Called only by states. Transitions happen in exactly one place.
    void transition_to(std::unique_ptr<DocumentState> next) {
        std::println("     -> {} becomes {}", state_->name(), next->name());
        state_ = std::move(next);
    }
    void append(std::string_view text) { content_ += text; }
    [[nodiscard]] const std::string& content() const { return content_; }
    [[nodiscard]] std::string_view state() const { return state_->name(); }

private:
    std::unique_ptr<DocumentState> state_;
    std::string content_;
};

// Default: every action is refused. Each state overrides only what it allows.
void DocumentState::edit(Document&, std::string_view) const { refuse("edit"); }
void DocumentState::submit(Document&) const { refuse("submit"); }
void DocumentState::approve(Document&) const { refuse("approve"); }
void DocumentState::reject(Document&) const { refuse("reject"); }

class Draft final : public DocumentState {
public:
    void edit(Document& doc, std::string_view text) const override { doc.append(text); }
    void submit(Document& doc) const override;
    std::string_view name() const override { return "Draft"; }
};

class InReview final : public DocumentState {
public:
    void approve(Document& doc) const override;
    void reject(Document& doc) const override { doc.transition_to(std::make_unique<Draft>()); }
    std::string_view name() const override { return "InReview"; }
};

class Published final : public DocumentState {
public:
    std::string_view name() const override { return "Published"; }
};

void Draft::submit(Document& doc) const {
    if (doc.content().empty()) return refuse("submit (empty document)");
    doc.transition_to(std::make_unique<InReview>());
}
void InReview::approve(Document& doc) const { doc.transition_to(std::make_unique<Published>()); }

Document::Document() : state_(std::make_unique<Draft>()) {}

// ===================================================================== 2. std::variant state machine

namespace conn {

// States: each one holds only the data that makes sense in that state.
struct Disconnected {};
struct Connecting { int attempt; };
struct Connected { std::string session_id; };
using State = std::variant<Disconnected, Connecting, Connected>;

// Events
struct Connect {};
struct Succeeded { std::string session_id; };
struct Failed {};
struct Disconnect {};
using Event = std::variant<Connect, Succeeded, Failed, Disconnect>;

constexpr int kMaxAttempts = 3;

// The transition table, written as overloads: (current state, event) -> next state.
// The generic fallback at the bottom handles every combination not listed: the event is ignored.
struct Transitions {
    State operator()(Disconnected, Connect) const { return Connecting{1}; }
    State operator()(Connecting c, Failed) const {
        if (c.attempt < kMaxAttempts) return Connecting{c.attempt + 1};  // retry
        return Disconnected{};                                           // give up
    }
    State operator()(Connecting, const Succeeded& e) const { return Connected{e.session_id}; }
    State operator()(const Connected&, Disconnect) const { return Disconnected{}; }
    State operator()(Connecting, Disconnect) const { return Disconnected{}; }

    template <typename S, typename E>
    State operator()(const S& state, const E&) const { return state; }  // not a valid transition: stay
};

std::string describe(const State& s) {
    struct {
        std::string operator()(Disconnected) const { return "Disconnected"; }
        std::string operator()(const Connecting& c) const { return std::format("Connecting(attempt {}/{})", c.attempt, kMaxAttempts); }
        std::string operator()(const Connected& c) const { return std::format("Connected(session {})", c.session_id); }
    } visitor;
    return std::visit(visitor, s);
}

std::string describe(const Event& e) {
    struct {
        std::string operator()(Connect) const { return "Connect"; }
        std::string operator()(const Succeeded& s) const { return "Succeeded(" + s.session_id + ")"; }
        std::string operator()(Failed) const { return "Failed"; }
        std::string operator()(Disconnect) const { return "Disconnect"; }
    } visitor;
    return std::visit(visitor, e);
}

class Connection {
public:
    void handle(const Event& event) {
        State next = std::visit(Transitions{}, state_, event);  // double dispatch on (state, event)
        const bool changed = next.index() != state_.index() || describe(next) != describe(state_);
        std::println("     {:<24} {:<26} -> {}{}", describe(state_), describe(event), describe(next), changed ? "" : "   (ignored)");
        state_ = std::move(next);
    }
    [[nodiscard]] bool is_connected() const { return std::holds_alternative<Connected>(state_); }

private:
    State state_ = Disconnected{};
};

}  // namespace conn

// ===================================================================== demo

int main() {
    std::println("1) Classic State: a document workflow");
    Document doc;
    std::println("   submit an empty draft:");
    doc.submit();
    std::println("   edit, submit, try to edit during review, reject, edit again, submit, approve:");
    doc.edit("Design patterns notes. ");
    doc.submit();
    doc.edit("sneaky change");
    doc.reject();
    doc.edit("Now with examples.");
    doc.submit();
    doc.approve();
    std::println("   publish again:");
    doc.approve();
    std::println("   final state: {}   content: \"{}\"", doc.state(), doc.content());

    std::println("\n2) std::variant state machine: a connection with retries");
    conn::Connection c;
    for (const conn::Event& e : std::vector<conn::Event>{conn::Disconnect{}, conn::Connect{}, conn::Failed{}, conn::Failed{},
                                                         conn::Succeeded{"a91f"}, conn::Connect{}, conn::Disconnect{},
                                                         conn::Connect{}, conn::Failed{}, conn::Failed{}, conn::Failed{}}) {
        c.handle(e);
    }
    std::println("   connected at the end: {}", c.is_connected());
}
