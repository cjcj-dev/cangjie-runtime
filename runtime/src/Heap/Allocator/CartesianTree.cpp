// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CartesianTree.h"

#include <cstdlib>

#include "Allocator/RegionInfo.h"

namespace MapleRuntime {
int MappedCache::SizeClass(Count count)
{
    if (count < 2) { return -1; }
    int shift = 0;
    while ((count >>= 1) > 1) { ++shift; }
    return shift;
}

void MappedCache::AddEntry(Index index, Count count)
{
    const int klass = SizeClass(count);
    Entry entry{ count, {} };
    if (klass >= 0) {
        sizeClasses[klass].push_front(index);
        entry.sizeClassPosition = sizeClasses[klass].begin();
    }
    CHECK(entries.emplace(index, entry).second);
    if (refresh) { refresh({index, count}); }
}

void MappedCache::EraseEntry(std::map<Index, Entry>::iterator entry)
{
    const int klass = SizeClass(entry->second.count);
    if (klass >= 0) { sizeClasses[klass].erase(entry->second.sizeClassPosition); }
    entries.erase(entry);
}

// zMappedCache.cpp:560: coalesce both neighbours in the same authoritative cache.
void MappedCache::Insert(Extent extent)
{
    CHECK(extent.count != 0);
    Index start = extent.index;
    Count count = extent.count;
    auto right = entries.lower_bound(start);
    CHECK(right == entries.end() || static_cast<uint64_t>(start) + count <= right->first);
    if (right != entries.begin()) {
        auto left = std::prev(right);
        CHECK(static_cast<uint64_t>(left->first) + left->second.count <= start);
        if (left->first + left->second.count == start) {
            start = left->first;
            count += left->second.count;
            EraseEntry(left);
        }
    }
    if (right != entries.end() && start + count == right->first) {
        count += right->second.count;
        EraseEntry(right);
    }
    AddEntry(start, count);
    size += extent.count;
    lastUsedNs = TimeUtil::NanoSeconds();
}

MappedCache::Extent MappedCache::Remove(Index index, Count count, bool high)
{
    auto entry = entries.find(index);
    CHECK(entry != entries.end() && count <= entry->second.count && count != 0);
    const Count remaining = entry->second.count - count;
    EraseEntry(entry);
    Extent result{ high ? index + remaining : index, count };
    if (remaining != 0) { AddEntry(high ? index : index + count, remaining); }
    size -= count;
    minSizeWatermark = std::min(size, minSizeWatermark);
    return result;
}

MappedCache::Index MappedCache::Select(Count minimum, Count maximum) const
{
    // zMappedCache.cpp:408: guaranteed size class, then descending approximate
    // best fit; only the sub-size-class tail needs an address scan.
    int guaranteed = SizeClass(maximum);
    if (maximum <= 2) { guaranteed = 0; }
    else if ((maximum & (maximum - 1)) != 0) { ++guaranteed; }
    for (int klass = guaranteed; klass < NUM_SIZE_CLASSES; ++klass) {
        if (!sizeClasses[klass].empty()) { return sizeClasses[klass].front(); }
    }
    for (int klass = SizeClass(maximum); klass >= std::max(SizeClass(minimum), 0); --klass) {
        for (Index index : sizeClasses[klass]) {
            if (entries.at(index).count >= minimum) { return index; }
        }
    }
    if (SizeClass(minimum) < 0) {
        for (const auto& entry : entries) {
            if (entry.second.count >= minimum) { return entry.first; }
        }
    }
    return UINT32_MAX;
}

MappedCache::Extent MappedCache::RemoveContiguous(Count count)
{
    CHECK(count != 0);
    if (size < count) { return {}; }
    // zMappedCache.cpp:642: small pages are selected from the lowest address.
    const Index index = count == 1 ? entries.begin()->first : Select(count, count);
    return index == UINT32_MAX ? Extent{} : Remove(index, count);
}

MappedCache::Count MappedCache::RemoveDiscontiguous(Count count, std::vector<Extent>& out)
{
    Count remaining = count;
    while (remaining != 0 && size != 0) {
        const Index index = Select(1, std::min(remaining, size));
        const Extent extent = Remove(index, std::min(remaining, entries.at(index).count));
        out.push_back(extent);
        remaining -= extent.count;
    }
    return count - remaining;
}

MappedCache::Count MappedCache::RemoveForUncommit(Count count, std::vector<Extent>& out)
{
    Count remaining = count;
    while (remaining != 0 && !entries.empty()) {
        auto last = std::prev(entries.end());
        const Extent extent = Remove(last->first, std::min(remaining, last->second.count), true);
        out.push_back(extent);
        remaining -= extent.count;
    }
    return count - remaining;
}

MappedCache::Count MappedCache::MaxExtent() const
{
    Count maximum = 0;
    for (const auto& entry : entries) { maximum = std::max(maximum, entry.second.count); }
    return maximum;
}

bool CartesianTree::MergeInsertInternal(Index idx, Count num, bool refreshRegionInfo)
{
    //     +-------------+       +--------------+
    //     | parent node |  n--> | current node |
    //     |             |       |              |
    // pn ---> Node* l; -------> |              |
    //     |   Node* r;  |       |              |
    //     +-------------+       +--------------+
    // suppose current node is parent node's left child, then
    // n points to the current node,
    // pn points to the 'l' field in the parent node
    Node* n = root;    // root is current node
    Node** pn = &root; // pointer to the 'root' field in this tree
    // stack of pn recording how to go from root to the current node
    LocalDeque<Node**> pnStack(sud); // this uses another deque as container
    Index m = idx + num;
    // this loop insert the new node (idx, num) at the proper place
    do {
        if (n == nullptr) {
            n = new (nodeAllocator.Allocate()) Node(idx, num, refreshRegionInfo);
            CTREE_ASSERT(n != nullptr, "failed to allocate a new node");
            *pn = n;
            IncTotalCount(num);
            break;
        }
        MergeResult res = MergeAt(*n, idx, num, refreshRegionInfo);
        if (res == MergeResult::MERGE_SUCCESS) {
            break;
        } else if (UNLIKELY(res == MergeResult::MERGE_ERROR)) {
            return false;
        }
        // MergeResult::MERGE_MISS: (idx, num) cannot be connected to n
        if (m < n->GetIndex()) {
            // should insert into left subtree
            pnStack.Push(pn);
            pn = &(n->l);
            n = n->l;
        } else if (idx > n->GetIndex() + n->GetCount()) {
            // should insert into right subtree
            pnStack.Push(pn);
            pn = &(n->r);
            n = n->r;
        } else {
            // something clashes
            CTREE_ASSERT(false, "merge insertion failed");
            return false;
        }
    } while (true);

    // this loop bubbles the inserted node up the tree to satisfy heap property
    while (!pnStack.Empty()) {
        pn = pnStack.Top();
        pnStack.Pop();
        n = *pn;
        CTREE_ASSERT(n, "merge insertion bubbling failed case 1");
        if (m < n->GetIndex()) {
            // (idx, num) was inserted into n's left subtree, do rotate l, if needed
            if (n->GetCount() < n->l->GetCount()) {
                *pn = RotateLeftChild(*n);
                CTREE_CHECK_PARENT_AND_RCHILD(*pn);
            } else {
                break;
            }
        } else if (idx > n->GetIndex() + n->GetCount()) {
            // (idx, num) was inserted into n's right subtree, do rotate r, if needed
            if (n->GetCount() < n->r->GetCount()) {
                *pn = RotateRightChild(*n);
                CTREE_CHECK_PARENT_AND_LCHILD(*pn);
            } else {
                break;
            }
        } else {
            CTREE_ASSERT(false, "merge insertion bubbling failed case 2");
            return false;
        }
    }
    return true;
}

#ifdef DEBUG_CARTESIAN_TREE
void CartesianTree::DumpTree(const char* msg) const
{
    if (Empty()) {
        return;
    }

    VLOG(REPORT, "dump %s %p in graphviz .dot:", msg, this);
    VLOG(REPORT, "digraph tree%p {", this);
    CartesianTree::Iterator it(*const_cast<CartesianTree*>(this));
    auto node = it.Next();
    while (node != nullptr) {
        VLOG(REPORT, "c-tree %p N%p [label=\"%p:%u+%u=%u\"]", this, node, node, node->GetIndex(),
             node->GetCount(), node->GetIndex() + node->GetCount());

        if (node->l != nullptr) {
            VLOG(REPORT, "c-tree %p N%p -> N%p", this, node, node->l);
        }

        VLOG(REPORT, "c-tree %p N%p -> D%p [style=invis]", this, node, node);
        VLOG(REPORT, "c-tree %p D%p [width=0, style=invis]", this, node);

        if (node->r != nullptr) {
            VLOG(REPORT, "c-tree %p N%p -> N%p", this, node, node->r);
        }

        node = it.Next();
    }
    VLOG(REPORT, "}");
}
#endif

void CartesianTree::Node::RefreshFreeRegionInfo()
{
    Index idx = GetIndex();
    Count cnt = GetCount();
    RegionInfo::InitFreeRegion(idx, cnt);
}

size_t CartesianTree::GetNodeCount() const
{
    size_t nodeCount = 0;
    Iterator it(*const_cast<CartesianTree*>(this));
    while (it.Next() != nullptr) {
        ++nodeCount;
    }
    return nodeCount;
}

bool CartesianTree::TakeIdleUnits(uint64_t idleBeforeNs, Count maxCount, Index& idx, Count& num)
{
    if (root == nullptr || maxCount == 0 || lastUsedNs > idleBeforeNs) {
        return false;
    }
    Count want = maxCount;
    if (root->GetCount() < want) {
        want = root->GetCount();
    }
#if defined(MRT_GC_UNIT_TESTS)
    const char* cut = std::getenv("MRT_UNCOMMIT_CUT_OWNERSHIP");
    if (cut != nullptr && cut[0] != '\0' && !(cut[0] == '0' && cut[1] == '\0')) {
        idx = root->GetIndex();
        num = want;
        return true;
    }
#endif
    if (!TakeUnitsImpl(want, idx, false)) {
        return false;
    }
    num = want;
    return true;
}

bool CartesianTree::TakeUnitsImpl(Count num, Index& idx, bool refershRegionInfo)
{
    ForwardIterator it(*this);
    Node** nodePtr = it.Next(); // pointer to root node
    if (UNLIKELY(nodePtr == nullptr)) {
        return false;
    }
    Node* node = *nodePtr;
    if (node != nullptr && node->GetCount() < num) {
        DLOG(REGION, "c-tree %p fail to take %u free units", this, num);
        return false;
    }
    Node** nextNodePtr = nullptr;
    while ((nextNodePtr = it.Next()) != nullptr) {
        Node* nextNode = *nextNodePtr;
        if (nextNode != nullptr && nextNode->GetCount() < num) {
            break;
        }

        nodePtr = nextNodePtr;
    }

    node = *nodePtr;
    idx = node->GetIndex();
    auto count = node->GetCount();
    lastUsedNs = TimeUtil::NanoSeconds();

    node->UpdateNode(idx + num, count - num, refershRegionInfo);
    DecTotalCount(num);

    if (node->GetCount() == 0) {
        RemoveZeroNode(*nodePtr);
    } else {
        LowerNonZeroNode(*nodePtr);
    }

    CTREE_CHECK_PARENT_AND_LCHILD(*nodePtr);
    CTREE_CHECK_PARENT_AND_RCHILD(*nodePtr);

    return true;
}

// Best-fit with lowest-address tiebreaker (iterative).
CartesianTree::Node** CartesianTree::FindBestFitLowAddrPtr(Node** nodePtr, Count num, Node** best)
{
    LocalDeque<Node**> stack(sud);
    if (nodePtr != nullptr && *nodePtr != nullptr && (*nodePtr)->GetCount() >= num) {
        stack.Push(nodePtr);
    }

    while (!stack.Empty()) {
        Node** current = stack.Top();
        stack.Pop();

        if (current == nullptr || *current == nullptr || (*current)->GetCount() < num) {
            continue;
        }

        // Current node is a candidate (count >= num).
        // Prefer: (1) smallest count, (2) lowest index as tiebreaker.
        if (best == nullptr ||
            (*current)->GetCount() < (*best)->GetCount() ||
            ((*current)->GetCount() == (*best)->GetCount() &&
             (*current)->GetIndex() < (*best)->GetIndex())) {
            best = current;
        }

        // Exact fit found — only left subtree could have same count with lower address.
        if ((*best)->GetCount() == num) {
            Node** leftChild = &((*current)->l);
            if (*leftChild != nullptr && (*leftChild)->GetCount() >= num) {
                stack.Push(leftChild);
            }
        } else {
            // Search both subtrees for a tighter fit.
            Node** rightChild = &((*current)->r);
            if (*rightChild != nullptr && (*rightChild)->GetCount() >= num) {
                stack.Push(rightChild);
            }
            Node** leftChild = &((*current)->l);
            if (*leftChild != nullptr && (*leftChild)->GetCount() >= num) {
                stack.Push(leftChild);
            }
        }
    }

    return best;
}

// Best-fit allocation with lowest-address tiebreaker.
// Picks the node whose count is closest to num to avoid unnecessary splitting,
// breaking ties by lowest address to pack live data toward the low end.
bool CartesianTree::TakeUnitsLowAddrImpl(Count num, Index& idx, bool refreshRegionInfo)
{
    Node** nodePtr = FindBestFitLowAddrPtr(&root, num, nullptr);
    if (nodePtr == nullptr) {
        DLOG(REGION, "c-tree %p fail to take %u free units (best-fit-low-addr)", this, num);
        return false;
    }

    Node* node = *nodePtr;
    idx = node->GetIndex();
    auto count = node->GetCount();

    node->UpdateNode(idx + num, count - num, refreshRegionInfo);
    DecTotalCount(num);

    if (node->GetCount() == 0) {
        RemoveZeroNode(*nodePtr);
    } else {
        LowerNonZeroNode(*nodePtr);
    }

    return true;
}

bool CartesianTree::AllocateLowestAddressFromNode(Node*& node, Count count, Index& index)
{
    Count nodeCount = node->GetCount();
    if (nodeCount < count) {
        return false;
    }

    index = node->GetIndex();
    DLOG(REGION, "c-tree %p v-alloc %u units from [%u+%u, %u)", this, count, index, nodeCount, index + nodeCount);

    node->UpdateNode(index + count, nodeCount - count, false);
    DecTotalCount(count);
    if (node->GetCount() == 0) {
        RemoveZeroNode(node);
    } else {
        LowerNonZeroNode(node);
    }
    return true;
}
} // namespace MapleRuntime
