#include <memory>
#include <stdexcept>
#include <string>

#include "spl/object_graph.h"
#include "test_framework.h"

SPL_TEST(element_tree_uses_weak_parent_and_strong_children) {
    spl::Tracked<spl::ElementTag>::reset_counters();
    std::weak_ptr<spl::Element> grandchild_observer;
    {
        std::shared_ptr<spl::Element> root = spl::make_element("root");
        std::shared_ptr<spl::Element> child = spl::make_element("child");
        std::shared_ptr<spl::Element> grandchild = spl::make_element("grandchild");

        root->add_child(child);
        child->add_child(grandchild);
        grandchild_observer = grandchild;

        SPL_CHECK_EQ(root->children().size(), 1);
        SPL_CHECK(child->parent().get() == root.get());
        SPL_CHECK(grandchild->parent().get() == child.get());
        SPL_CHECK_EQ(grandchild->depth(), 2);
        SPL_CHECK(!root->parent());
    }
    SPL_CHECK(grandchild_observer.expired());
    SPL_CHECK_EQ(spl::Tracked<spl::ElementTag>::alive(), 0);
}

SPL_TEST(element_rejects_cycles_and_multiple_parents) {
    std::shared_ptr<spl::Element> root = spl::make_element("root");
    std::shared_ptr<spl::Element> child = spl::make_element("child");
    std::shared_ptr<spl::Element> leaf = spl::make_element("leaf");

    root->add_child(child);
    child->add_child(leaf);
    SPL_CHECK_THROW(root->add_child(root), std::logic_error);
    SPL_CHECK_THROW(leaf->add_child(root), std::logic_error);
    SPL_CHECK_THROW(root->add_child(child), std::logic_error);

    std::shared_ptr<spl::Element> other_parent = spl::make_element("other");
    SPL_CHECK_THROW(other_parent->add_child(leaf), std::logic_error);
}

SPL_TEST(element_requires_shared_ownership) {
    spl::Element stack_element("stack");
    std::shared_ptr<spl::Element> child = spl::make_element("child");
    SPL_CHECK_THROW(stack_element.add_child(child), std::bad_weak_ptr);
}

SPL_TEST(shared_ptr_cycle_leaks_until_broken) {
    spl::Tracked<spl::CyclicNodeTag>::reset_counters();
    std::weak_ptr<spl::CyclicNode> first_observer;
    std::weak_ptr<spl::CyclicNode> second_observer;
    {
        std::shared_ptr<spl::CyclicNode> first = std::make_shared<spl::CyclicNode>();
        std::shared_ptr<spl::CyclicNode> second = std::make_shared<spl::CyclicNode>();
        first_observer = first;
        second_observer = second;
        first->link_to(second);
        second->link_to(first);

        first.reset();
        second.reset();
        SPL_CHECK(!first_observer.expired());
        SPL_CHECK(!second_observer.expired());
        SPL_CHECK_EQ(spl::Tracked<spl::CyclicNodeTag>::alive(), 2);

        first_observer.lock()->break_link();
    }
    SPL_CHECK(first_observer.expired());
    SPL_CHECK(second_observer.expired());
    SPL_CHECK_EQ(spl::Tracked<spl::CyclicNodeTag>::alive(), 0);
}
