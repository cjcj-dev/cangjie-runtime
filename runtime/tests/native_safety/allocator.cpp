// Uses the normal product NativeAllocator -> ThreadCache -> CentralCache chain.
#include "Common/NativeAllocator.h"
#include "Common/PageCache.h"
#include "Mutator/ThreadLocal.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_set>
using namespace MapleRuntime;
static void Require(bool condition, const char* what)
{
    if (!condition) { std::fprintf(stderr, "FAIL %s\n", what); std::exit(43); }
}
static void Run(size_t requested)
{
    const size_t aligned = SizeManager::RoundUp(requested);
    const size_t spanBytes = SizeManager::CalculatePageNum(aligned) << PAGE_SHIFT;
    const size_t perSpan = spanBytes / aligned;
    const size_t count = 2 * perSpan + 3;
    std::vector<void*> objects;
    std::unordered_set<void*> unique;
    std::unordered_set<pageID> spans;
    for (size_t i = 0; i < count; ++i) {
        void* object = NativeAllocator::NativeAlloc(requested);
        Require(object != nullptr, "allocation");
        Span* span = PageCache::GetInstance()->MapObjectToSpan(object);
        Require(span != nullptr && span->isUse, "live span");
        const size_t offset = reinterpret_cast<uintptr_t>(object) - (span->pageId << PAGE_SHIFT);
        const size_t bytes = span->pageNum << PAGE_SHIFT;
        if (offset > bytes || aligned > bytes - offset) {
            std::fprintf(stderr, "FAIL FULL_BLOCK requested=%zu aligned=%zu allocation=%zu offset=%zu spanBytes=%zu remaining=%zu\n",
                requested, aligned, i, offset, bytes, offset <= bytes ? bytes - offset : 0);
            std::exit(42);
        }
        Require(offset % aligned == 0, "block alignment within span");
        Require(unique.insert(object).second, "unique live allocation");
        spans.insert(span->pageId);
        std::memset(object, static_cast<unsigned char>(i % 251 + 1), requested);
        objects.push_back(object);
    }
    Require(spans.size() >= 3, "exhausted spans");
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto* bytes = static_cast<const unsigned char*>(objects[i]);
        for (size_t j = 0; j < requested; ++j) Require(bytes[j] == i % 251 + 1, "filled allocation preserved");
    }
    for (void* object : objects) NativeAllocator::NativeFree(object, requested);
    static_cast<ThreadCache*>(ThreadLocal::GetThreadCache())->Flush();
    for (void* object : objects) {
        Span* span = PageCache::GetInstance()->MapObjectToSpan(object);
        Require(!span->isUse && span->useCount == 0 && span->freeBlocks == nullptr, "free chain drained to PageCache");
    }
    std::printf("PASS requested=%zu aligned=%zu spanBytes=%zu perSpan=%zu allocations=%zu spans=%zu full_block_checks=%zu fill/free/drain\n",
        requested, aligned, spanBytes, perSpan, count, spans.size(), count);
}
int main(int argc, char** argv)
{
    Require(argc == 2, "one size argument");
    const size_t requested = std::strtoull(argv[1], nullptr, 10);
    Require(requested > 0 && requested <= MAX_BYTES, "small allocation size");
    Require(NativeAllocator::NativeAlloc(0) == nullptr, "zero allocation");
    NativeAllocator::NativeFree(nullptr, 0);
    Run(requested);
    Run(requested); // consumed/released spans, cached PageCache reuse
}
