// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Port of OpenJDK test/hotspot/gtest/gc/z/test_zList.cpp onto ZList<T>.

#include "Heap/z/zList.inline.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

class ZTestEntry {
    friend class MapleRuntime::ZList<ZTestEntry>;

private:
    const int             _id;
    ZListNode<ZTestEntry> _node;

public:
    ZTestEntry(int id)
        : _id(id),
          _node() {}

    int id() const
    {
        return _id;
    }
};

class ZListTest {
public:
    static void assert_sorted(ZList<ZTestEntry>* list)
    {
        // Iterate forward
        {
            int count = list->first()->id();
            ZListIterator<ZTestEntry> iter(list);
            for (ZTestEntry* entry; iter.next(&entry);) {
                GC_EXPECT_EQ(entry->id(), count);
                count++;
            }
        }

        // Iterate backward
        {
            int count = list->last()->id();
            ZListReverseIterator<ZTestEntry> iter(list);
            for (ZTestEntry* entry; iter.next(&entry);) {
                GC_EXPECT_EQ(entry->id(), count);
                count--;
            }
        }
    }
};

} // namespace

GC_TEST(ZListTest, test_insert)
{
    ZList<ZTestEntry> list;
    ZTestEntry e0(0);
    ZTestEntry e1(1);
    ZTestEntry e2(2);
    ZTestEntry e3(3);
    ZTestEntry e4(4);
    ZTestEntry e5(5);

    list.insert_first(&e2);
    list.insert_before(&e2, &e1);
    list.insert_after(&e2, &e3);
    list.insert_last(&e4);
    list.insert_first(&e0);
    list.insert_last(&e5);

    GC_EXPECT_EQ(list.size(), 6u);
    ZListTest::assert_sorted(&list);

    for (int i = 0; i < 6; i++) {
        ZTestEntry* e = list.remove_first();
        GC_EXPECT_EQ(e->id(), i);
    }

    GC_EXPECT_EQ(list.size(), 0u);
}

GC_TEST(ZListTest, test_remove)
{
    // Remove first
    {
        ZList<ZTestEntry> list;
        ZTestEntry e0(0);
        ZTestEntry e1(1);
        ZTestEntry e2(2);
        ZTestEntry e3(3);
        ZTestEntry e4(4);
        ZTestEntry e5(5);

        list.insert_last(&e0);
        list.insert_last(&e1);
        list.insert_last(&e2);
        list.insert_last(&e3);
        list.insert_last(&e4);
        list.insert_last(&e5);

        GC_EXPECT_EQ(list.size(), 6u);

        for (int i = 0; i < 6; i++) {
            ZTestEntry* e = list.remove_first();
            GC_EXPECT_EQ(e->id(), i);
        }

        GC_EXPECT_EQ(list.size(), 0u);
    }

    // Remove last
    {
        ZList<ZTestEntry> list;
        ZTestEntry e0(0);
        ZTestEntry e1(1);
        ZTestEntry e2(2);
        ZTestEntry e3(3);
        ZTestEntry e4(4);
        ZTestEntry e5(5);

        list.insert_last(&e0);
        list.insert_last(&e1);
        list.insert_last(&e2);
        list.insert_last(&e3);
        list.insert_last(&e4);
        list.insert_last(&e5);

        GC_EXPECT_EQ(list.size(), 6u);

        for (int i = 5; i >= 0; i--) {
            ZTestEntry* e = list.remove_last();
            GC_EXPECT_EQ(e->id(), i);
        }

        GC_EXPECT_EQ(list.size(), 0u);
    }
}

// The size/iteration contract behind the spec's cut ③ (ZList::remove must
// unlink): after removing the middle element the list has one element less
// and neither forward nor reverse iteration visits the removed entry. The
// entries are heap objects that this test never frees, so a failing
// expectation reports on its own line instead of tripping ~ZListNode.
GC_TEST(ZListTest, test_remove_middle_unlinks)
{
    ZList<ZTestEntry>* list = new ZList<ZTestEntry>();
    ZTestEntry* e0 = new ZTestEntry(0);
    ZTestEntry* e1 = new ZTestEntry(1);
    ZTestEntry* e2 = new ZTestEntry(2);

    list->insert_last(e0);
    list->insert_last(e1);
    list->insert_last(e2);
    GC_EXPECT_EQ(list->size(), 3u);

    list->remove(e1);
    GC_EXPECT_EQ(list->size(), 2u);

    int visited = 0;
    ZListIterator<ZTestEntry> iter(list);
    for (ZTestEntry* entry; iter.next(&entry);) {
        GC_EXPECT_NE(entry->id(), 1);
        visited++;
    }
    GC_EXPECT_EQ(visited, 2);

    visited = 0;
    ZListReverseIterator<ZTestEntry> reverse(list);
    for (ZTestEntry* entry; reverse.next(&entry);) {
        GC_EXPECT_NE(entry->id(), 1);
        visited++;
    }
    GC_EXPECT_EQ(visited, 2);

    GC_EXPECT_TRUE(list->next(e0) == e2);
    GC_EXPECT_TRUE(list->prev(e2) == e0);
}
