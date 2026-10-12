// SPDX-License-Identifier: MIT
// Exercise the exported DSO hook via dlsym/LD_PRELOAD. Only this test executable
// replaces dladdr to model the exact audited caller, never the production DSO.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <memory>
#include <stdexcept>

namespace {
using Callback = std::function<void(double, double)>;
using Hook = void (*)(const Callback *, double, double);
int caller_mode;
struct Counter {
    int n = 0;
    void operator()(double, double) { ++n; }
};
}

extern "C" __attribute__((visibility("default")))
int dladdr(const void *address, Dl_info *info) noexcept {
    *info = {};
    info->dli_fname = caller_mode == 2 ? "/tmp/not-the-audited-framework.so" :
                                       "/usr/lib/wemeet/libwemeet_framework.so";
    info->dli_fbase = reinterpret_cast<void *>(
        reinterpret_cast<std::uintptr_t>(address) -
        (caller_mode == 3 ? 0x917f44 : 0x917f45));
    return caller_mode != 0;
}

int main() {
    auto hook = reinterpret_cast<Hook>(
        dlsym(RTLD_DEFAULT, "_ZNKSt8functionIFvddEEclEdd"));
    assert(hook);
    setenv("WEMEET_TOOLTIP_FIX", "1", 1);
    Callback source = Counter{};
    // Unknown module, wrong module, and off-by-one caller all pass through.
    for (int mode : {0, 2, 3}) {
        caller_mode = mode;
        hook(&source, 0, 0);
    }
    assert(source.target<Counter>()->n == 3);
    caller_mode = 1;
    unsetenv("WEMEET_TOOLTIP_FIX");
    hook(&source, 0, 0);
    assert(source.target<Counter>()->n == 4);

    setenv("WEMEET_TOOLTIP_FIX", "1", 1);
    bool copied_capture_survived = false;
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> weak = owner;
    source = [owner, &source, &weak, &copied_capture_survived](double x, double y) {
        Callback *local_source = &source;
        auto local_weak = weak;
        bool *out = &copied_capture_survived;
        assert(x == -14.25 && y == 3.5);
        *local_source = {};
        *out = !local_weak.expired();
    };
    owner.reset();
    hook(&source, -14.25, 3.5);
    assert(copied_capture_survived && weak.expired() && !source);

    owner = std::make_shared<int>(43);
    weak = owner;
    source = [owner, &source](double, double) {
        Callback *local_source = &source;
        *local_source = {};
        throw std::runtime_error("expected");
    };
    owner.reset();
    try {
        hook(&source, 0, 0);
        assert(false);
    } catch (const std::runtime_error &) {
        assert(weak.expired());
    }
    try {
        hook(&source, 0, 0);
        assert(false);
    } catch (const std::bad_function_call &) {}
    std::puts("layout preload: caller gates, captures and exception unwinding OK");
}
