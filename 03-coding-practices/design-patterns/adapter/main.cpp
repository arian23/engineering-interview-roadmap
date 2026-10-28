// Adapter pattern (C++23)
//
// Idea: make an existing class with the "wrong" interface usable where a different interface is
// expected, without changing either side. The adapter translates calls, units, types and error
// conventions. Theory: ../README.md#adapter
//
// Scenario: our monitoring code works with one interface, TemperatureSensor (Celsius, std::expected
// for errors). We must plug in three things we cannot change:
//   1. LegacyFahrenheitProbe  - an old C++ class: tenths of a degree Fahrenheit, -1 means "error"
//      -> ObjectAdapter (composition: the adapter HAS-A probe)            <- preferred form
//   2. VendorThermometer       - a vendor class we may inherit from, reports Kelvin
//      -> ClassAdapter (inheritance: the adapter IS-A vendor thermometer)
//   3. A C library             - handles, char buffers, int return codes
//      -> CApiAdapter, which also owns the C handle (RAII)
// Plus a function adapter: a C callback API (function pointer + void* user data) used with a C++ lambda.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// ===================================================================== the interface our code expects (Target)

class TemperatureSensor {
public:
    virtual ~TemperatureSensor() = default;
    [[nodiscard]] virtual std::expected<double, std::string> read_celsius() = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};

void report(std::vector<std::unique_ptr<TemperatureSensor>>& sensors) {  // client code: knows only the Target
    for (auto& sensor : sensors) {
        if (auto t = sensor->read_celsius()) {
            std::println("   {:<28} {:6.2f} °C", sensor->name(), *t);
        } else {
            std::println("   {:<28} error: {}", sensor->name(), t.error());
        }
    }
}

// ===================================================================== 1. legacy C++ class (Adaptee)

class LegacyFahrenheitProbe {
public:
    explicit LegacyFahrenheitProbe(int fake_tenths) : value_(fake_tenths) {}
    int GetTempTenthsF() const { return value_; }  // -1 on failure: an old-style error code
private:
    int value_;
};

class ProbeAdapter final : public TemperatureSensor {  // OBJECT adapter: composition
public:
    explicit ProbeAdapter(LegacyFahrenheitProbe probe) : probe_(probe) {}
    std::expected<double, std::string> read_celsius() override {
        const int tenths = probe_.GetTempTenthsF();
        if (tenths == -1) return std::unexpected("legacy probe returned -1");  // translate the error convention
        const double fahrenheit = tenths / 10.0;                              // translate units
        return (fahrenheit - 32.0) * 5.0 / 9.0;
    }
    std::string name() const override { return "legacy probe (via adapter)"; }

private:
    LegacyFahrenheitProbe probe_;
};

// ===================================================================== 2. vendor class (Adaptee) + class adapter

class VendorThermometer {
public:
    double kelvin() const { return 296.65; }
    std::string model() const { return "VT-9000"; }
};

class VendorAdapter final : public TemperatureSensor, private VendorThermometer {  // CLASS adapter: inheritance
public:
    std::expected<double, std::string> read_celsius() override { return kelvin() - 273.15; }
    std::string name() const override { return model() + " (class adapter)"; }
};

// ===================================================================== 3. a C library (Adaptee)

extern "C" {
struct thermo_device { int id; int open; };
thermo_device* thermo_open(int id) {
    auto* d = static_cast<thermo_device*>(std::malloc(sizeof(thermo_device)));
    *d = {id, 1};
    std::println("   [C library] thermo_open({})", id);
    return d;
}
int thermo_read(thermo_device* d, char* buffer, std::size_t size) {  // writes "23.5C"; returns 0 or an errno code
    if (d == nullptr || !d->open) return EBADF;
    if (d->id == 13) return EIO;  // device 13 is broken
    std::snprintf(buffer, size, "%d.%dC", 20 + d->id, d->id % 10);
    return 0;
}
void thermo_close(thermo_device* d) {
    std::println("   [C library] thermo_close({})", d->id);
    std::free(d);
}
}

class CApiAdapter final : public TemperatureSensor {
public:
    // Acquire in the constructor. A unique_ptr with a function-pointer deleter needs both arguments.
    explicit CApiAdapter(int device_id) : device_(thermo_open(device_id), &thermo_close) {}
    std::expected<double, std::string> read_celsius() override {
        char buffer[32] = {};
        if (const int rc = thermo_read(device_.get(), buffer, sizeof buffer); rc != 0) {
            return std::unexpected(std::generic_category().message(rc));  // int code -> readable message
        }
        return std::strtod(buffer, nullptr);  // "23.3C" -> 23.3
    }
    std::string name() const override { return std::format("C device #{} (adapter)", device_->id); }

private:
    // The adapter also adapts the RESOURCE convention: open/close -> RAII via a custom deleter.
    // (Holding a unique_ptr also makes the adapter move-only, so the handle can never be closed twice.)
    std::unique_ptr<thermo_device, decltype(&thermo_close)> device_;
};

// ===================================================================== 4. function adapter for a C callback API

extern "C" {
using sample_callback = void (*)(double value, void* user_data);
void sample_n_times(int n, sample_callback cb, void* user_data) {  // a typical C API: no closures
    for (int i = 0; i < n; ++i) cb(20.0 + i * 0.5, user_data);
}
}

// Lets any C++ callable (including capturing lambdas) be used with sample_n_times.
void sample_n_times(int n, const std::function<void(double)>& fn) {
    sample_n_times(n,
                   [](double value, void* user) { (*static_cast<const std::function<void(double)>*>(user))(value); },
                   const_cast<std::function<void(double)>*>(&fn));
}

// ===================================================================== demo

int main() {
    std::println("1) One client loop, four very different devices behind the same interface");
    {
        std::vector<std::unique_ptr<TemperatureSensor>> sensors;
        sensors.push_back(std::make_unique<ProbeAdapter>(LegacyFahrenheitProbe(725)));  // 72.5 °F
        sensors.push_back(std::make_unique<ProbeAdapter>(LegacyFahrenheitProbe(-1)));   // failing probe
        sensors.push_back(std::make_unique<VendorAdapter>());
        sensors.push_back(std::make_unique<CApiAdapter>(3));
        sensors.push_back(std::make_unique<CApiAdapter>(13));  // broken C device
        report(sensors);
        std::println("   (sensors going out of scope: the C handles are closed automatically)");
    }

    std::println("\n2) Function adapter: a capturing lambda passed to a C API that only takes a function pointer");
    std::vector<double> samples;
    double max = 0;
    sample_n_times(5, [&](double v) {
        samples.push_back(v);
        max = std::max(max, v);
    });
    std::println("   collected {} samples, max {:.1f} °C", samples.size(), max);
}
