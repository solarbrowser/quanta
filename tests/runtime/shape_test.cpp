/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Standalone unit tests for Shape (make shape-test).
 */

#include "quanta/core/runtime/Shape.h"
#include <cstdio>

using namespace Quanta;

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void test_root_is_empty() {
    Shape* root = Shape::root();
    CHECK(root->slot_count() == 0);
    CHECK(root->find_slot("x") == -1);
    CHECK(root->keys_in_order().empty());
}

static void test_linear_transition_assigns_slots_in_order() {
    Shape* root = Shape::root();
    Shape* s1 = root->transition("a");
    Shape* s2 = s1->transition("b");
    Shape* s3 = s2->transition("c");

    CHECK(s1->slot_count() == 1);
    CHECK(s2->slot_count() == 2);
    CHECK(s3->slot_count() == 3);

    CHECK(s3->find_slot("a") == 0);
    CHECK(s3->find_slot("b") == 1);
    CHECK(s3->find_slot("c") == 2);
    CHECK(s3->find_slot("d") == -1);

    auto keys = s3->keys_in_order();
    CHECK(keys.size() == 3);
    CHECK(keys[0] == "a" && keys[1] == "b" && keys[2] == "c");
}

static void test_same_key_sequence_shares_shape() {
    Shape* root = Shape::root();
    Shape* p1 = root->transition("x")->transition("y");
    Shape* p2 = root->transition("x")->transition("y");
    CHECK(p1 == p2);
}

static void test_different_order_diverges() {
    Shape* root = Shape::root();
    Shape* ab = root->transition("a")->transition("b");
    Shape* ba = root->transition("b")->transition("a");
    CHECK(ab != ba);
    CHECK(ab->find_slot("a") == 0 && ab->find_slot("b") == 1);
    CHECK(ba->find_slot("b") == 0 && ba->find_slot("a") == 1);
}

static void test_transition_cap_forces_dictionary_fallback() {
    Shape* base = Shape::root()->transition("__cap_test_base__");
    for (int i = 0; i < 128; i++) {
        Shape* child = base->transition("k" + std::to_string(i));
        CHECK(child != nullptr);
    }
    // 129th distinct key from the same shape must be refused.
    CHECK(base->transition("k128") == nullptr);
    // Re-requesting an already-memoized transition still works past the cap.
    CHECK(base->transition("k0") != nullptr);
}

static void test_slot_depth_cap_forces_dictionary_fallback() {
    Shape* s = Shape::root()->transition("__depth_test_base__");
    for (int i = 1; i < 128; i++) {
        s = s->transition("d" + std::to_string(i));
        CHECK(s != nullptr);
    }
    CHECK(s->slot_count() == 128);
    // 129th property on one object must be refused (chain depth cap),
    // even though this node has no sibling transitions at all.
    CHECK(s->transition("d128") == nullptr);
}

static void test_independent_branches_do_not_interfere() {
    Shape* root = Shape::root();
    Shape* left = root->transition("__branch_left__")->transition("p")->transition("q");
    Shape* right = root->transition("__branch_right__")->transition("m");
    CHECK(left->find_slot("m") == -1);
    CHECK(right->find_slot("p") == -1 && right->find_slot("q") == -1);
    CHECK(left->slot_count() == 3);
    CHECK(right->slot_count() == 2);
}

int main() {
    test_root_is_empty();
    test_linear_transition_assigns_slots_in_order();
    test_same_key_sequence_shares_shape();
    test_different_order_diverges();
    test_transition_cap_forces_dictionary_fallback();
    test_slot_depth_cap_forces_dictionary_fallback();
    test_independent_branches_do_not_interfere();

    if (failures == 0) {
        std::printf("shape-test: ALL PASS\n");
        return 0;
    }
    std::printf("shape-test: %d FAILURE(S)\n", failures);
    return 1;
}
