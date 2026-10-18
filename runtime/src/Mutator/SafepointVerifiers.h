#pragma once
#include "Mutator/Mutator.h"

namespace MapleRuntime {
// HotSpot safepointVerifiers.cpp:33-50. Native malloc and metadata vectors
// are allowed; a managed allocation or a safepoint-capable transition is not.
class NoSafepointVerifier {
public:
    NoSafepointVerifier() : mutator(Mutator::GetMutator())
    {
        if (mutator != nullptr) { mutator->IncNoSafepointCount(); }
    }
    ~NoSafepointVerifier()
    {
        if (mutator != nullptr) { mutator->DecNoSafepointCount(); }
    }
    NoSafepointVerifier(const NoSafepointVerifier&) = delete;
    NoSafepointVerifier& operator=(const NoSafepointVerifier&) = delete;
private:
    Mutator* const mutator;
};
} // namespace MapleRuntime
