#include "Heap/Verify/PlainCensus.h"
#include "ObjectModel/RefField.h"
namespace MapleRuntime {
namespace {
[[maybe_unused]] HealSite kH[]={HealSite::PlainCensusInject,HealSite::PlainCensusRestore};
}
const char* PlainWriterSiteName(PlainWriterSite site) { return nullptr; }
ScopedPlainWriter::ScopedPlainWriter(PlainWriterSite site) {}
ScopedPlainWriter::~ScopedPlainWriter() {}
void NotePlainHeapWrite(const void* slot, uintptr_t newVal) {  }
void RunPlainCensus(const char* point, bool force ) {  }
bool InjectPlainHeapWriteOnce() { return false; }
void DumpPlainWriteCounters(const char* point) {  }
PlainWriteColumn ColumnOf(PlainWriterSite site) { return {}; }
} // namespace MapleRuntime
