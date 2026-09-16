#ifndef MRT_Z_WEAK_ROOTS_PROCESSOR_HPP
#define MRT_Z_WEAK_ROOTS_PROCESSOR_HPP

namespace MapleRuntime {
class ZWorkers;

class ZWeakRootsProcessor {
private:
    ZWorkers* workers;
public:
    explicit ZWeakRootsProcessor(ZWorkers* workers);
    void process_weak_roots();
};
} // namespace MapleRuntime
#endif
