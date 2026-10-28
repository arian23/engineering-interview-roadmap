// RAII: Resource Acquisition Is Initialization (C++23)
//
// Idea: tie every resource (memory, file, lock, socket, transaction...) to the lifetime of an
// object. The constructor ACQUIRES the resource; the destructor RELEASES it. Because C++ always
// runs destructors when an object leaves scope (normal return, early return, break, or an
// exception), the release can never be forgotten. Theory: ../README.md#raii
//
// Sections:
//   1. A hand-written owning wrapper (File) that follows the "rule of five": move-only, never closes twice
//   2. Cleanup order: objects are destroyed in REVERSE order of construction, also during an exception
//   3. A generic scope guard (ScopeExit / ScopeFail) for one-off cleanup without writing a class
//   4. A transaction that rolls back automatically unless commit() is called
//   5. The standard library already gives you RAII types: unique_ptr with a custom deleter, lock_guard

#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ===================================================================== 1. an owning wrapper

class File {
public:
    File(const std::filesystem::path& path, const char* mode) : handle_(std::fopen(path.string().c_str(), mode)), path_(path) {
        if (handle_ == nullptr) throw std::runtime_error("cannot open " + path.string());  // no object = no resource
        std::println("     open   {}", path_.filename().string());
    }
    ~File() { close(); }

    // Rule of five: an owner of a raw resource must decide what copy and move mean.
    File(const File&) = delete;             // copying would make two owners -> double fclose
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)), path_(std::move(other.path_)) {}
    File& operator=(File&& other) noexcept {
        if (this != &other) {
            close();  // release what we own before taking the other one's resource
            handle_ = std::exchange(other.handle_, nullptr);
            path_ = std::move(other.path_);
        }
        return *this;
    }

    void write(std::string_view text) {
        if (std::fwrite(text.data(), 1, text.size(), handle_) != text.size()) throw std::runtime_error("write failed");
    }

private:
    void close() noexcept {  // destructors must not throw
        if (handle_ != nullptr) {
            std::fclose(std::exchange(handle_, nullptr));
            std::println("     close  {}", path_.filename().string());
        }
    }

    std::FILE* handle_;
    std::filesystem::path path_;
};

// ===================================================================== 2. cleanup order

struct Noisy {
    explicit Noisy(std::string n) : name(std::move(n)) { std::println("     acquire {}", name); }
    ~Noisy() { std::println("     release {}", name); }
    Noisy(const Noisy&) = delete;
    Noisy& operator=(const Noisy&) = delete;
    std::string name;
};

void step_that_fails() {
    Noisy connection("database connection");
    Noisy buffer("4 MB buffer");
    Noisy lock("table lock");
    std::println("     ... working, then something throws");
    throw std::runtime_error("disk full");
    // no explicit cleanup anywhere, and none is needed
}

// ===================================================================== 3. scope guards

// Runs a callable when the scope ends, however it ends. (C++ library proposal std::scope_exit.)
template <typename F>
class [[nodiscard]] ScopeExit {
public:
    explicit ScopeExit(F f) : f_(std::move(f)) {}
    ~ScopeExit() {
        if (active_) f_();
    }
    void release() noexcept { active_ = false; }  // cancel the action
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F f_;
    bool active_ = true;
};

// Runs only if the scope is left BECAUSE OF an exception (std::uncaught_exceptions grew since construction).
template <typename F>
class [[nodiscard]] ScopeFail {
public:
    explicit ScopeFail(F f) : f_(std::move(f)) {}
    ~ScopeFail() {
        if (std::uncaught_exceptions() > exceptions_at_start_) f_();
    }
    ScopeFail(const ScopeFail&) = delete;
    ScopeFail& operator=(const ScopeFail&) = delete;

private:
    F f_;
    int exceptions_at_start_ = std::uncaught_exceptions();
};

bool save_report(const std::filesystem::path& path, bool fail_midway) {
    // Declared BEFORE the file, so it is destroyed AFTER it: the file is closed first, then deleted.
    // (Deleting an open file works on POSIX but fails on Windows.)
    ScopeFail remove_partial_file([&] {
        std::println("     scope failed -> deleting the half-written {}", path.filename().string());
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    });
    File out(path, "w");
    out.write("header\n");
    if (fail_midway) throw std::runtime_error("lost connection while writing");
    out.write("rows...\n");
    return true;
}

// ===================================================================== 4. transaction with automatic rollback

class Ledger {
public:
    class Transaction {
    public:
        explicit Transaction(Ledger& ledger) : ledger_(ledger), snapshot_(ledger.balances_) {}
        ~Transaction() {
            if (!committed_) {
                ledger_.balances_ = std::move(snapshot_);  // undo everything since the transaction began
                std::println("     transaction not committed -> rolled back");
            }
        }
        void commit() noexcept { committed_ = true; }
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

    private:
        Ledger& ledger_;
        std::vector<int> snapshot_;
        bool committed_ = false;
    };

    void transfer(std::size_t from, std::size_t to, int amount, bool fail = false) {
        Transaction tx(*this);
        balances_.at(from) -= amount;
        if (fail) throw std::runtime_error("network error after the debit");  // credit never happens
        balances_.at(to) += amount;
        tx.commit();
    }
    [[nodiscard]] std::string show() const { return std::format("A={} B={}", balances_[0], balances_[1]); }

private:
    std::vector<int> balances_{100, 100};
};

// ===================================================================== demo

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "raii-demo";
    std::filesystem::create_directories(dir);

    std::println("1) File: opened in the constructor, closed in the destructor, ownership moves but is never shared");
    {
        File log(dir / "app.log", "w");
        log.write("started\n");
        File moved = std::move(log);  // ownership moves; `log` now owns nothing
        moved.write("still writing through the new owner\n");
        std::println("     end of scope:");
    }

    std::println("\n2) An exception in the middle: everything is released, in reverse order");
    try {
        step_that_fails();
    } catch (const std::exception& e) {
        std::println("     caught: {}", e.what());
    }

    std::println("\n3) Scope guards");
    {
        ScopeExit timer([] { std::println("     ScopeExit: always runs at the end of the block"); });
        std::println("     inside the block");
    }
    save_report(dir / "ok.csv", false);
    std::println("     ok.csv kept: {}", std::filesystem::exists(dir / "ok.csv"));
    try {
        save_report(dir / "broken.csv", true);
    } catch (const std::exception& e) {
        std::println("     caught: {}", e.what());
    }
    std::println("     broken.csv exists: {}", std::filesystem::exists(dir / "broken.csv"));

    std::println("\n4) Transaction: commit or roll back, with no manual cleanup code");
    Ledger ledger;
    ledger.transfer(0, 1, 30);
    std::println("     after a good transfer:   {}", ledger.show());
    try {
        ledger.transfer(0, 1, 50, /*fail=*/true);
    } catch (const std::exception& e) {
        std::println("     caught: {}", e.what());
    }
    std::println("     after a failed transfer: {} (the debit was undone)", ledger.show());

    std::println("\n5) Standard RAII types");
    {
        std::unique_ptr<std::FILE, int (*)(std::FILE*)> c_file(std::fopen((dir / "c.txt").string().c_str(), "w"), &std::fclose);
        std::fputs("closed by unique_ptr's deleter\n", c_file.get());
        std::mutex m;
        std::lock_guard lock(m);  // unlocked automatically at the end of the scope
        std::println("     unique_ptr<FILE, fclose> and lock_guard: both released at the closing brace");
    }

    std::filesystem::remove_all(dir);
}
