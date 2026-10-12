// SPDX-License-Identifier: MIT
// Version-bound GNU libstdc++/x86-64 callback-lifetime guard. No Qt headers.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <utility>

#if !defined(__GLIBCXX__) || !defined(__x86_64__)
#error "The audited layout guard requires GNU libstdc++ on x86-64"
#endif

namespace {
using Callback = std::function<void(double, double)>;
// Match GNU's actual invoker type, not just an ABI-compatible pointer shape.
// Do not call the public operator(): that is the symbol being interposed.
using Invoker = void (*)(const std::_Any_data &, double &&, double &&);
struct Layout {
    std::_Any_data storage;
    void *manager;
    Invoker invoker;
};
static_assert(sizeof(Callback) == sizeof(Layout), "unsupported std::function ABI");
static_assert(sizeof(Layout) == 32 && offsetof(Layout, invoker) == 24,
              "only the audited x86-64 ABI is supported");
std::atomic<unsigned long> snapshots{0};

bool enabled() {
    const char *value = std::getenv("WEMEET_TOOLTIP_FIX");
    return value && std::strcmp(value, "1") == 0;
}

bool target_caller(void *caller) {
    Dl_info info{};
    if (!dladdr(caller, &info) || !info.dli_fbase || !info.dli_fname)
        return false;
    // Only this version's Placeable::PlaceAt PLT call is protected. Other
    // std::function<void(double,double)> users retain direct invocation.
    return std::strcmp(info.dli_fname, "/usr/lib/wemeet/libwemeet_framework.so") == 0 &&
           reinterpret_cast<std::uintptr_t>(caller) -
               reinterpret_cast<std::uintptr_t>(info.dli_fbase) == 0x917f45;
}

void invoke(const Callback *callback, double x, double y) {
    if (!*callback)
        throw std::bad_function_call();
    Invoker invoker;
    std::memcpy(&invoker, reinterpret_cast<const char *>(callback) +
                              offsetof(Layout, invoker), sizeof(invoker));
    const auto &storage = *reinterpret_cast<const std::_Any_data *>(callback);
    invoker(storage, std::move(x), std::move(y));
}

void snapshot_invoke(const Callback *source, double x, double y) {
    Callback snapshot(*source);
    if (snapshots.fetch_add(1, std::memory_order_relaxed) == 0)
        std::fprintf(stderr, "[wemeet-layout-guard] Placeable::PlaceAt snapshot active\n");
    invoke(&snapshot, x, y);
}

__attribute__((destructor)) void report() {
    if (enabled())
        std::fprintf(stderr, "[wemeet-layout-guard] callback snapshots=%lu\n",
                     snapshots.load(std::memory_order_relaxed));
}
} // namespace

extern "C" __attribute__((visibility("default")))
void guarded_callback(const Callback *, double, double)
    asm("_ZNKSt8functionIFvddEEclEdd");

extern "C" void guarded_callback(const Callback *source, double x, double y) {
    void *caller = __builtin_extract_return_addr(__builtin_return_address(0));
    if (!enabled() || !target_caller(caller)) {
        invoke(source, x, y);
        return;
    }
    // Real copy construction invokes the original functor's manager, retaining
    // owned captures as well as isolating SBO bytes from self-assignment. RAII
    // also releases the copy if the callback throws; exceptions are not caught.
    snapshot_invoke(source, x, y);
}
