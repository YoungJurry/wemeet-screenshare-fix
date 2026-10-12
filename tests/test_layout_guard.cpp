// SPDX-License-Identifier: MIT
// Include the implementation so private helpers can be tested without adding
// test bypasses, symbols, or caller-whitelist switches to the production DSO.
#include "../src/layout_guard.cpp"
#include <cassert>
#include <memory>
#include <stdexcept>

namespace {
struct Node { int placed = 0; };
struct Child { void (*place)(Node *, double, double); };
Callback *reentered_source;
Node *expected_node;
Child *expected_child;

void replace_callback(Node *node, double x, double y) {
    assert(node == expected_node && x == -14.25 && y == 3.5);
    *reentered_source = [holder = std::make_shared<int>(32)](double, double) {
        assert(*holder == 32);
    };
}
struct TwoPointers {
    Node *node;
    Child *child;
    void operator()(double x, double y) const {
        child->place(node, x, y);
        // This read must use the isolated SBO, not the replaced source storage.
        assert(node == expected_node && child == expected_child);
        node->placed++;
    }
};

struct MutableCounter {
    int count = 0;
    void operator()(double, double) { ++count; }
};
}

int main() {
    unsetenv("WEMEET_TOOLTIP_FIX");
    assert(!enabled());
    setenv("WEMEET_TOOLTIP_FIX", "1", 1);
    assert(enabled());
    assert(!target_caller(nullptr));
    assert(!target_caller(reinterpret_cast<void *>(&main)));

    // Both disabled and unrelated callers must retain direct/mutable behavior.
    Callback direct = MutableCounter{};
    guarded_callback(&direct, 1, 2);
    assert(direct.target<MutableCounter>()->count == 1);
    unsetenv("WEMEET_TOOLTIP_FIX");
    guarded_callback(&direct, 1, 2);
    assert(direct.target<MutableCounter>()->count == 2);

    // Exact audited SBO shape: two ordinary pointers, no ownership to infer.
    Node node;
    Child child{replace_callback};
    expected_node = &node;
    expected_child = &child;
    Callback source;
    reentered_source = &source;
    for (int n = 0; n < 2000; ++n) {
        source = TwoPointers{&node, &child};
        snapshot_invoke(&source, -14.25, 3.5);
        assert(source.target<TwoPointers>() == nullptr);
    }
    assert(node.placed == 2000);

    // Heap-held captures must also outlive replacement, then release normally.
    for (int n = 0; n < 2000; ++n) {
        auto owner = std::make_shared<int>(n);
        std::weak_ptr<int> weak = owner;
        bool observed_live = false;
        source = [owner, &weak, &source, &observed_live](double x, double y) {
            // Save references before self-replacement destroys the original.
            auto local_weak = weak;
            Callback *local_source = &source;
            bool *out = &observed_live;
            assert(x == -14.25 && y == 3.5);
            *local_source = {};
            *out = !local_weak.expired();
        };
        owner.reset();
        snapshot_invoke(&source, -14.25, 3.5);
        assert(observed_live && weak.expired() && !source);
    }

    // Original direct invocation demonstrates the capture lifetime difference.
    auto owner = std::make_shared<int>(7);
    std::weak_ptr<int> weak = owner;
    bool expired_inside = false;
    source = [owner, &weak, &source, &expired_inside](double, double) {
        auto local_weak = weak;
        Callback *local_source = &source;
        bool *out = &expired_inside;
        *local_source = {};
        *out = local_weak.expired();
    };
    owner.reset();
    invoke(&source, 0, 0);
    assert(expired_inside && weak.expired());

    // Exceptions propagate, and copied captures release during stack unwinding.
    owner = std::make_shared<int>(8);
    weak = owner;
    source = [owner, &source](double, double) {
        Callback *local_source = &source;
        *local_source = {};
        throw std::runtime_error("expected");
    };
    owner.reset();
    try {
        snapshot_invoke(&source, 0, 0);
        assert(false);
    } catch (const std::runtime_error &) {
        assert(weak.expired());
    }
    try {
        snapshot_invoke(&source, 0, 0);
        assert(false);
    } catch (const std::bad_function_call &) {}
    std::puts("layout guard: 2000 SBO + 2000 ownership replacements, passthrough, exceptions OK");
}
