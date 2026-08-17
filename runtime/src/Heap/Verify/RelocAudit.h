#ifndef MRT_RELOC_AUDIT_H
#define MRT_RELOC_AUDIT_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;

namespace RelocAudit {

static constexpr bool kEnabled = false;

void Note(BaseObject* from, BaseObject* to, size_t size);
void CompareAtEvacFinish(const char* site);

} // namespace RelocAudit
} // namespace MapleRuntime

#endif
