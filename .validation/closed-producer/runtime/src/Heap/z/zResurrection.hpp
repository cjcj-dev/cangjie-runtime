#ifndef MRT_Z_RESURRECTION_HPP
#define MRT_Z_RESURRECTION_HPP

#include <atomic>

#include "Heap/z/zBarrier.hpp"

namespace MapleRuntime {

class ZResurrection : public AllStatic {
private:
    static std::atomic<bool> blocked;

public:
    static bool is_blocked();
    static void block();
    static void unblock();
};

} // namespace MapleRuntime
#endif
