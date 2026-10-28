// Factory patterns (C++23)
//
// Idea: separate the code that CREATES objects from the code that USES them. Callers ask for
// "something that does X" and get back an object behind an interface, without naming (or
// #including) the concrete class. Theory: ../README.md#factory
//
// Four variants, from simplest to most flexible:
//   1. Simple factory     - one function with a switch: kind -> concrete object
//   2. Factory method     - a base class defines the algorithm; subclasses decide which object to create
//   3. Abstract factory   - one factory creates a whole FAMILY of related objects that must match
//   4. Registry factory   - a name -> creator map; new types register themselves, so adding a type
//                           does not touch the factory at all (open/closed principle)

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// ===================================================================== product interface (used by 1 and 4)

class Exporter {
public:
    virtual ~Exporter() = default;
    [[nodiscard]] virtual std::string format(const std::vector<std::pair<std::string, int>>& rows) const = 0;
};

class CsvExporter final : public Exporter {
public:
    std::string format(const std::vector<std::pair<std::string, int>>& rows) const override {
        std::string out = "name,count\n";
        for (const auto& [k, v] : rows) out += std::format("{},{}\n", k, v);
        return out;
    }
};

class JsonExporter final : public Exporter {
public:
    std::string format(const std::vector<std::pair<std::string, int>>& rows) const override {
        std::string out = "[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            out += std::format("{}{{\"name\":\"{}\",\"count\":{}}}", i ? "," : "", rows[i].first, rows[i].second);
        }
        return out + "]\n";
    }
};

// ===================================================================== 1. simple factory

enum class Format { Csv, Json };

[[nodiscard]] std::unique_ptr<Exporter> make_exporter(Format f) {
    switch (f) {
        case Format::Csv: return std::make_unique<CsvExporter>();
        case Format::Json: return std::make_unique<JsonExporter>();
    }
    return nullptr;  // unreachable when every enum value is handled (-Wswitch warns about missing ones)
}

// ===================================================================== 2. factory method

class Transport {
public:
    virtual ~Transport() = default;
    [[nodiscard]] virtual std::string deliver(std::string_view cargo) const = 0;
};
class Truck final : public Transport {
public:
    std::string deliver(std::string_view cargo) const override { return std::format("truck drives {} by road", cargo); }
};
class Ship final : public Transport {
public:
    std::string deliver(std::string_view cargo) const override { return std::format("ship carries {} by sea", cargo); }
};

class Logistics {
public:
    virtual ~Logistics() = default;
    // The shared algorithm. It does not know which Transport it will get.
    void plan(std::string_view cargo) const {
        const auto transport = create_transport();  // <- the factory method
        std::println("   {:<16} {}", name(), transport->deliver(cargo));
    }

protected:
    [[nodiscard]] virtual std::unique_ptr<Transport> create_transport() const = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
};
class RoadLogistics final : public Logistics {
    std::unique_ptr<Transport> create_transport() const override { return std::make_unique<Truck>(); }
    std::string_view name() const override { return "RoadLogistics"; }
};
class SeaLogistics final : public Logistics {
    std::unique_ptr<Transport> create_transport() const override { return std::make_unique<Ship>(); }
    std::string_view name() const override { return "SeaLogistics"; }
};

// ===================================================================== 3. abstract factory

class Button {
public:
    virtual ~Button() = default;
    [[nodiscard]] virtual std::string render(std::string_view label) const = 0;
};
class Checkbox {
public:
    virtual ~Checkbox() = default;
    [[nodiscard]] virtual std::string render(std::string_view label, bool checked) const = 0;
};

// One factory per family. Using one factory guarantees the widgets always match each other.
class WidgetFactory {
public:
    virtual ~WidgetFactory() = default;
    [[nodiscard]] virtual std::unique_ptr<Button> create_button() const = 0;
    [[nodiscard]] virtual std::unique_ptr<Checkbox> create_checkbox() const = 0;
};

class LightButton final : public Button {
    std::string render(std::string_view l) const override { return std::format("[ {} ]", l); }
};
class LightCheckbox final : public Checkbox {
    std::string render(std::string_view l, bool c) const override { return std::format("[{}] {}", c ? "x" : " ", l); }
};
class DarkButton final : public Button {
    std::string render(std::string_view l) const override { return std::format("<# {} #>", l); }
};
class DarkCheckbox final : public Checkbox {
    std::string render(std::string_view l, bool c) const override { return std::format("({}) {}", c ? "*" : " ", l); }
};

class LightTheme final : public WidgetFactory {
    std::unique_ptr<Button> create_button() const override { return std::make_unique<LightButton>(); }
    std::unique_ptr<Checkbox> create_checkbox() const override { return std::make_unique<LightCheckbox>(); }
};
class DarkTheme final : public WidgetFactory {
    std::unique_ptr<Button> create_button() const override { return std::make_unique<DarkButton>(); }
    std::unique_ptr<Checkbox> create_checkbox() const override { return std::make_unique<DarkCheckbox>(); }
};

void draw_settings_dialog(const WidgetFactory& ui) {  // client: knows only the abstract factory
    const auto remember = ui.create_checkbox();
    const auto ok = ui.create_button();
    std::println("     {}   {}", remember->render("remember me", true), ok->render("OK"));
}

// ===================================================================== 4. registry (self-registering) factory

class ExporterRegistry {
public:
    using Creator = std::function<std::unique_ptr<Exporter>()>;

    static ExporterRegistry& instance() {  // function-local static: created on first use, thread-safe
        static ExporterRegistry registry;
        return registry;
    }

    bool add(std::string name, Creator creator) { return creators_.emplace(std::move(name), std::move(creator)).second; }

    [[nodiscard]] std::expected<std::unique_ptr<Exporter>, std::string> create(std::string_view name) const {
        if (auto it = creators_.find(name); it != creators_.end()) return it->second();
        return std::unexpected(std::format("unknown exporter '{}'", name));
    }

    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const auto& [name, creator] : creators_) out.push_back(name);
        return out;
    }

private:
    std::map<std::string, Creator, std::less<>> creators_;  // std::less<> allows lookup by string_view
};

// Each type registers itself in its own file. The registry and the callers never change.
namespace {
const bool csv_registered = ExporterRegistry::instance().add("csv", [] { return std::make_unique<CsvExporter>(); });
const bool json_registered = ExporterRegistry::instance().add("json", [] { return std::make_unique<JsonExporter>(); });

// A brand-new format, added without editing anything above:
class MarkdownExporter final : public Exporter {
public:
    std::string format(const std::vector<std::pair<std::string, int>>& rows) const override {
        std::string out = "| name | count |\n|---|---|\n";
        for (const auto& [k, v] : rows) out += std::format("| {} | {} |\n", k, v);
        return out;
    }
};
const bool md_registered = ExporterRegistry::instance().add("markdown", [] { return std::make_unique<MarkdownExporter>(); });
}  // namespace

// ===================================================================== demo

int main() {
    const std::vector<std::pair<std::string, int>> rows{{"alpha", 3}, {"beta", 7}};

    std::println("1) Simple factory: make_exporter(Format::Json)");
    std::print("   {}", make_exporter(Format::Json)->format(rows));

    std::println("\n2) Factory method: same plan(), subclasses choose the transport");
    const std::vector<std::unique_ptr<Logistics>> companies = [] {
        std::vector<std::unique_ptr<Logistics>> v;
        v.push_back(std::make_unique<RoadLogistics>());
        v.push_back(std::make_unique<SeaLogistics>());
        return v;
    }();
    for (const auto& company : companies) company->plan("20 crates");

    std::println("\n3) Abstract factory: switch the whole widget family with one object");
    LightTheme light;
    DarkTheme dark;
    for (const WidgetFactory* theme : {static_cast<const WidgetFactory*>(&light), static_cast<const WidgetFactory*>(&dark)}) {
        std::println("   {}:", theme == &light ? "light theme" : "dark theme");
        draw_settings_dialog(*theme);
    }

    std::println("\n4) Registry factory: chosen by name (e.g. from a config file or command line)");
    std::string registered;
    for (const auto& name : ExporterRegistry::instance().names()) registered += (registered.empty() ? "" : ", ") + name;
    std::println("   registered: {}", registered);
    for (std::string_view requested : {"markdown", "csv", "xml"}) {
        if (auto exporter = ExporterRegistry::instance().create(requested)) {
            std::println("   '{}':", requested);
            std::print("{}", (*exporter)->format(rows));
        } else {
            std::println("   error: {}", exporter.error());
        }
    }
    (void)csv_registered, (void)json_registered, (void)md_registered;
}
