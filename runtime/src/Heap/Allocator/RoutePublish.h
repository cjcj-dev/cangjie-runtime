#ifndef MRT_ROUTE_PUBLISH_H
#define MRT_ROUTE_PUBLISH_H

namespace MapleRuntime {
class BaseObject;
class ScopedStopTheWorld;
struct CopierRouteMint;

struct RoutePlan {
    BaseObject* dest = nullptr;
};

struct PublishedRoute {
    BaseObject* dest = nullptr;
};

class CopierRouteToken {
    explicit CopierRouteToken(bool alreadyHeld) : alreadyHeld(alreadyHeld) {}
    bool alreadyHeld = false;
    friend struct CopierRouteMint;
    friend class RegionManager;
};

class StwRouteToken {
    StwRouteToken() = default;
    friend class ScopedStopTheWorld;
};
} // namespace MapleRuntime

#endif
