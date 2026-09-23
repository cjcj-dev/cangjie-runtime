#include "Common/WeakHandle.inline.h"
#include "Base/Log.h"

namespace MapleRuntime {
WeakHandle::WeakHandle(OopStorage* storage, BaseObject* object) : obj(storage->Allocate())
{
    CHECK_DETAIL(object != nullptr, "no need to create weak null oop");
    CHECK_DETAIL(obj != nullptr, "Unable to create new weak oop handle in OopStorage");
    NativeAccess<ON_PHANTOM_OOP_REF>::oop_store(obj, object);
}

void WeakHandle::release(OopStorage* storage)
{
    if (obj != nullptr) {
        NativeAccess<ON_PHANTOM_OOP_REF>::oop_store(obj, nullptr);
        storage->Release(obj);
        obj = nullptr;
    }
}
} // namespace MapleRuntime
