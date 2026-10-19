// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Linux native ABI fixture. All type creation, extension traversal, registration
// and reader/unloader operations bind the product SO, never a copied algorithm.
#include <bits/stdc++.h>
#include <dlfcn.h>
#include <unistd.h>
// Fixture-only access to compiler metadata and synchronization witnesses.
#define private public
#include "TypeInfoManager.h"
#include "LoaderManager.h"
#include "Loader/CjFileLoader/CjFileLoader.h"
#include "Loader/BinaryFile/CjFile/CjFile.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/ExtensionData.h"
#undef private

namespace MapleRuntime {
extern "C" TypeInfo* CJ_MCC_GetOrCreateTypeInfo(TypeTemplate*, U32, TypeInfo**);
}
extern "C" int LoadCJLibrary(const char*);
using namespace MapleRuntime;
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto limit = std::chrono::seconds(3);
void Check(const char* name, bool pass, int rc = 1)
{
    std::fprintf(stderr, "TYPEINFO_CATALOG_ASSERT %s executed=1 pass=%d\n", name, pass);
    std::fflush(stderr);
    if (!pass) { std::_Exit(rc); }
}
bool Await(const std::atomic<bool>& value)
{
    const auto end = Clock::now() + limit;
    while (!value.load(std::memory_order_acquire)) {
        if (Clock::now() >= end) { return false; }
        std::this_thread::yield();
    }
    return true;
}
template<class T> struct MetadataStorage {
    alignas(T) unsigned char bytes[sizeof(T)] {};
    T* get() { return reinterpret_cast<T*>(bytes); }
};
MetadataStorage<TypeInfo> argumentStorage;
MetadataStorage<TypeTemplate> outerStorage, workerStorage, nestedStorage, controlStorage;
TypeInfo* argument = argumentStorage.get();
TypeTemplate* outer = outerStorage.get();
TypeTemplate* workerType = workerStorage.get();
TypeTemplate* nestedType = nestedStorage.get();
TypeTemplate* controlType = controlStorage.get();
TypeInfo* args[] { argument };
std::atomic<bool> callbackEntered{false}, recordEntered{false}, releaseRecord{false};
std::atomic<bool> workerWitness{false}, recordDelegated{false};
std::atomic<bool> nestedEntered{false}, recordReturned{false}, callbackReturned{false};
std::atomic<bool> visitorDone{false}, workerDone{false};
thread_local bool workerArm = false;
std::atomic<bool> armed{false};
TypeInfo* workerResult = nullptr;
TypeInfo* visitorResult = nullptr;

// This wrapper only schedules entry to the real RecordTypeInfo. It does not
// acquire or replace either product mutex, or implement any catalog operation.
std::atomic<bool> feEntered{false}, releaseFE{false};
std::atomic<bool> loadArmed{false}, fileEntered{false}, releaseFile{false}, fileReturned{false};
class ScheduledFile final : public CJFile {
public:
    ScheduledFile(CString path, Uptr address) : CJFile(path, address) {}
    U32 GetTypeInfoTotalSize() override
    {
        if (loadArmed.load() && fileReturned.load() && !feEntered.load()) {
            auto* first = reinterpret_cast<TypeInfo*>(GetTypeInfoBase());
            auto* ext = LoaderManager::GetInstance()->GetLoader()->GetTypeExt(first);
            Check("fe-preparation-sees-own-typeext", ext && ext->ti == first);
            feEntered.store(true, std::memory_order_release);
            Check("fe-registration-barrier-released", Await(releaseFE));
        }
        return CJFile::GetTypeInfoTotalSize();
    }
    void RegisterFile() override
    {
        CJFile::RegisterFile();
        if (loadArmed.load()) {
            Check("public-load-nonempty-fe-table", GetTypeInfoTotalSize() >= sizeof(TypeInfo));
            fileEntered.store(true, std::memory_order_release);
            Check("file-registration-barrier-released", Await(releaseFile));
            fileReturned.store(true, std::memory_order_release);
        }
    }
};
class ScheduledLoader final : public CJFileLoader {
public:
    BaseFile* CreateFileRefFromAddr(Uptr address) override
    {
        // Retain the product dladdr/path resolution; only decorate its CJFile
        // with a scheduling barrier around the real metadata parser.
        BaseFile* original = CJFileLoader::CreateFileRefFromAddr(address);
        if (original == nullptr) { return nullptr; }
        auto* scheduled = new ScheduledFile(original->GetRealPath(), address);
        delete original;
        return scheduled;
    }
    void RecordTypeInfo(TypeInfo* ti) override
    {
        if (armed.load() && workerArm) {
            recordEntered.store(true, std::memory_order_release);
            Check("worker-record-entry-released", Await(releaseRecord));
            std::fprintf(stderr, "TYPEINFO_CATALOG_REACHED worker AddTypeInfo -> RecordTypeInfo\n");
            recordDelegated.store(true, std::memory_order_release);
            CJFileLoader::RecordTypeInfo(ti);
            recordReturned.store(true, std::memory_order_release);
        } else {
            CJFileLoader::RecordTypeInfo(ti);
        }
    }
};
ScheduledLoader* loader;
bool Available(std::recursive_mutex& mutex)
{
    if (!mutex.try_lock()) { return false; }
    mutex.unlock();
    return true;
}
void* Where(U32, TypeInfo**)
{
    if (!armed.load()) { return reinterpret_cast<void*>(1); }
    callbackEntered.store(true, std::memory_order_release);
    Check("callback-observed-worker-record-entry", Await(recordEntered));
    Check("callback-observed-worker-holds-typeinfo", Await(workerWitness));
    nestedEntered.store(true, std::memory_order_release);
    std::fprintf(stderr, "TYPEINFO_CATALOG_REACHED VisitExtensionData -> AddMTable callback -> GetOrCreateTypeInfo\n");
    releaseRecord.store(true, std::memory_order_release);
    auto* nested = TypeInfoManager::GetTypeInfoManager().GetOrCreateTypeInfo(nestedType, 1, args);
    Check("callback-nested-type-registered", nested && nested->GetUUID() != 0);
    callbackReturned.store(true, std::memory_order_release);
    return reinterpret_cast<void*>(1);
}
struct Image {
    CJFileHeader header {};
    CJGCFlagsTable flags {1, 1, 0};
    I32 extensionRef {};
    MetadataStorage<ExtensionData> extension;
} image;
CJFile* file;

void Setup()
{
    auto& manager = TypeInfoManager::GetTypeInfoManager();
    manager.NewMMap(1024 * 1024);
    argument->SetName("catalog:Int64");
    argument->SetType(TypeKind::TYPE_KIND_INT64);
    argument->SetAlign(8);
    argument->SetInstanceSize(8);
    manager.AddTypeInfo(argument);
    auto init = [](TypeTemplate* tt, const char* name, I8 kind) {
        tt->name = name; tt->type = kind; tt->typeArgsNum = 1;
    };
    init(outer, "catalog:Outer", TypeKind::TYPE_KIND_STRUCT);
    init(workerType, "catalog:WorkerArray", TypeKind::TYPE_KIND_RAWARRAY);
    init(nestedType, "catalog:NestedArray", TypeKind::TYPE_KIND_RAWARRAY);
    init(controlType, "catalog:ControlArray", TypeKind::TYPE_KIND_RAWARRAY);
    // Use the actual singleton dispatch used by AddMTable and AddTypeInfo.
    // The original singleton loader remains untouched and the fixture lives
    // until this isolated native process exits.
    loader = new ScheduledLoader;
    LoaderManager::GetInstance()->loader = loader;
    auto* ed = image.extension.get();
    ed->argNum = 1; ed->tt = outer;
    ed->isInterfaceTypeInfo = 1; ed->interfaceTypeInfo = argument;
    ed->whereCondFn = reinterpret_cast<FuncPtr>(&Where);
    image.extensionRef = static_cast<I32>(reinterpret_cast<char*>(ed) - reinterpret_cast<char*>(&image.extensionRef));
    image.header.cJFileSize = sizeof(image);
    image.header.tables[GC_FLAGS_TABLE] = {offsetof(Image, flags), sizeof(image.flags)};
    image.header.tables[OUTER_TYPE_EXTENSIONS_TABLE] = {offsetof(Image, extensionRef), sizeof(image.extensionRef)};
    file = new CJFile(CString("typeinfo-catalog-fixture"), reinterpret_cast<Uptr>(&image));
    loader->AddLoadedFiles(file);
    loader->RegisterLoadFile(reinterpret_cast<Uptr>(&image));
    Check("real-loader-registered-extension", loader->extensionDatas.at(file).count(outer) == 1);
    Dl_info info {};
    Check("product-symbol-source", dladdr(reinterpret_cast<void*>(&CJ_MCC_GetOrCreateTypeInfo), &info) != 0);
    std::fprintf(stderr, "TYPEINFO_CATALOG_PRODUCT %s\n", info.dli_fname);
}
void Controls()
{
    auto& manager = TypeInfoManager::GetTypeInfoManager();
    auto* ti = manager.GetOrCreateTypeInfo(controlType, 1, args);
    Check("independent-registration", ti && manager.ContainsTypeInfo(ti) && ti->GetUUID() != 0);
    Check("independent-directory", loader->FindTypeInfoFromLoadedFiles(ti->GetName()) == ti);
    TypeInfo* same = nullptr;
    std::thread duplicate([&] { same = manager.GetOrCreateTypeInfo(controlType, 1, args); });
    duplicate.join();
    Check("unique-published-type", same == ti);
}
void Reentrant()
{
    auto& manager = TypeInfoManager::GetTypeInfoManager();
    // No product mutex is manually acquired. The two real production paths
    // acquire them; the observer probes while their entry barriers are held.
    armed.store(true);
    std::thread visitor([&] {
        visitorResult = manager.GetOrCreateTypeInfo(outer, 1, args);
        visitorDone.store(true, std::memory_order_release);
    });
    Check("extension-callback-arrived", Await(callbackEntered));
    const bool callbackOwnsCatalog = !Available(loader->catalogMutex);
    std::thread worker([&] {
        workerArm = true;
        workerResult = manager.GetOrCreateTypeInfo(workerType, 1, args);
        workerDone.store(true, std::memory_order_release);
    });
    Check("registration-record-arrived", Await(recordEntered));
    Check("worker-holds-typeinfo-before-record", !Available(manager.tiMutex));
    workerWitness.store(true, std::memory_order_release);
    // Both entry barriers are now witnessed; only actual product mutexes
    // can prevent progress after the callback releases the recording worker.
    Check("nested-request-arrived", Await(nestedEntered));
    const bool completed = Await(visitorDone) && Await(workerDone);
    if (!completed) {
        const bool exactCycle = callbackOwnsCatalog && !Available(manager.tiMutex) &&
            !Available(loader->catalogMutex) && recordEntered.load() && nestedEntered.load() &&
            recordDelegated.load() && !recordReturned.load() && !callbackReturned.load();
        std::fprintf(stderr, "TYPEINFO_CATALOG_WAIT_GRAPH callback_owns_catalog=%d worker_in_record=%d nested_request=%d ti_unavailable=%d\n",
                     callbackOwnsCatalog, recordEntered.load(), nestedEntered.load(), !Available(manager.tiMutex));
        Check("no-catalog-typeinfo-cross-wait", !exactCycle, 42);
        Check("fixture-completion-without-proven-cycle", false, 2);
    }
    visitor.join(); worker.join(); armed.store(false);
    Check("no-catalog-typeinfo-cross-wait", true);
    Check("worker-published", loader->FindTypeInfoFromLoadedFiles(workerResult->GetName()) == workerResult);
    Check("outer-published", loader->FindTypeInfoFromLoadedFiles(visitorResult->GetName()) == visitorResult);
    Check("recursive-extension-installed", visitorResult->GetMTableDesc()->mTable.count(argument->GetUUID()) == 1);
}
void PublicLoad(const char* path, bool concurrent, bool enumCallback)
{
    auto& manager = TypeInfoManager::GetTypeInfoManager();
    LoaderManager::GetInstance()->Init();
    Check("runtime-registration-branch", LoaderManager::GetInstance()->GetInitStatus());
    loadArmed.store(concurrent); armed.store(concurrent);
    std::atomic<bool> loadDone{false};
    int loadRc = -1;
    std::thread loading([&] { loadRc = LoadCJLibrary(path); loadDone.store(true, std::memory_order_release); });
    std::thread worker;
    bool ownsCatalog = false;
    if (concurrent) {
        Check("public-dlopen-mrt-register-parser-arrived", Await(fileEntered));
        ownsCatalog = !Available(loader->catalogMutex);
        Check("real-file-parser-holds-catalog", ownsCatalog);
        Check("file-parser-does-not-hold-typeinfo", Available(manager.tiMutex));
        worker = std::thread([&] {
            workerArm = true;
            workerResult = manager.GetOrCreateTypeInfo(workerType, 1, args);
            workerDone.store(true, std::memory_order_release);
        });
        Check("parallel-loaded-type-reached-record", Await(recordEntered));
        Check("parallel-worker-holds-typeinfo", !Available(manager.tiMutex));
        // Both production locks are now owned by their respective paths.
        // Let RegisterFile return towards FE registration, then delegate the
        // worker to the real catalog recording operation.
        releaseFile.store(true, std::memory_order_release);
        Check("file-parser-returned-to-fe-registration", Await(fileReturned));
        Check("real-fe-registration-arrived", Await(feEntered));
        const bool feOwnsCatalog = !Available(loader->catalogMutex);
        std::fprintf(stderr, "TYPEINFO_LOAD_REACHED FE-registration catalog_owned=%d worker_holds_ti=1\n", feOwnsCatalog);
        if (!feOwnsCatalog) {
            Check("incomplete-package-not-public", loader->GetPackageInfo("catalog.load") == nullptr);
        }
        releaseFE.store(true, std::memory_order_release);
        releaseRecord.store(true, std::memory_order_release);
    }
    const bool done = Await(loadDone) && (!concurrent || Await(workerDone));
    if (!done) {
        const bool cycle = concurrent && ownsCatalog && fileReturned.load() && feEntered.load() &&
            recordDelegated.load() && !recordReturned.load() && !loadDone.load() &&
            !Available(manager.tiMutex) && !Available(loader->catalogMutex);
        std::fprintf(stderr, "TYPEINFO_LOAD_WAIT_GRAPH parser_returned=%d worker_delegated=%d catalog_unavailable=%d ti_unavailable=%d\n",
                     fileReturned.load(), recordDelegated.load(), !Available(loader->catalogMutex), !Available(manager.tiMutex));
        Check("no-public-load-typeinfo-cross-wait", !cycle, 43);
        Check("public-load-completed-without-cycle", false, 2);
    }
    loading.join(); if (concurrent) { worker.join(); }
    armed.store(false); loadArmed.store(false);
    Check("public-load-result", loadRc == 0);
    auto* pkg = loader->GetPackageInfo("catalog.load");
    Check("complete-package-published", pkg && pkg->GetNumOfTypeInfos() == 2);
    for (const char* name : {"catalog.load:First", "catalog.load:Last"}) {
        auto* ti = loader->FindTypeInfoFromLoadedFiles(name);
        Check("fe-type-registered-before-directory-published", ti && ti->GetUUID() != 0 && manager.ContainsTypeInfo(ti));
    }
    if (concurrent) {
        Check("parallel-type-published", loader->FindTypeInfoFromLoadedFiles(workerResult->GetName()) == workerResult);
    }
    if (enumCallback) {
        void* handle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
        Check("enum-dso-already-loaded", handle != nullptr);
        auto getNested = reinterpret_cast<TypeInfo*(*)()>(dlsym(handle, "CatalogNestedType"));
        Check("enum-callback-symbol", getNested != nullptr);
        auto* nested = getNested();
        auto* enumeration = loader->FindTypeInfoFromLoadedFiles("catalog.load:Last");
        Check("fe-enum-callback-created-generic", nested && manager.ContainsTypeInfo(nested));
        Check("fe-enum-published-parsed", enumeration->GetEnumInfo()->IsParsed() &&
              enumeration->GetEnumInfo()->GetCtorTypeInfo(0) == nested);
        Check("nested-registration-directory-committed", loader->FindTypeInfoFromLoadedFiles(nested->GetName()) == nested);
        dlclose(handle); // Drop only the RTLD_NOLOAD reference; public load stays owned.
    }
    Check("no-public-load-typeinfo-cross-wait", true);
}
void Unload()
{
    auto& manager = TypeInfoManager::GetTypeInfoManager();
    auto* ti = manager.GetOrCreateTypeInfo(controlType, 1, args);
    std::atomic<bool> entered{false}, release{false}, writerEntered{false}, drained{false};
    std::thread reader([&] {
        loader->VisitExtensionData(ti, [&](ExtensionData* ed) {
            entered.store(true, std::memory_order_release);
            Check("reader-release", Await(release));
            Check("metadata-live-during-callback", ed == image.extension.get() && ed->GetTargetType() == outer);
            return false;
        }, outer);
    });
    Check("unload-callback-arrived", Await(entered));
    std::thread writer([&] {
        ElfUnloadQuiescence::UnloadScope unload(reinterpret_cast<Uptr>(&image));
        // The real unlink/drain operations used by RemoveLoadedFiles, with no
        // mutator/GC simulation. Existing public unload tests cover its entry.
        loader->UnlinkLoadedFile(file);
        writerEntered.store(true, std::memory_order_release);
        unload.Synchronize();
        drained.store(true, std::memory_order_release);
        unload.OpenAdmission();
    });
    Check("unload-unlinked-while-callback-active", Await(writerEntered));
    Check("unload-drain-waits-for-callback", !drained.load());
    release.store(true, std::memory_order_release);
    reader.join(); writer.join();
    Check("unload-drain-after-callback", drained.load());
    unsigned visits = 0;
    loader->VisitExtensionData(ti, [&](ExtensionData*) { ++visits; return false; }, outer);
    Check("unlinked-extension-not-discovered", visits == 0);
}
}
int main(int argc, char** argv)
{
    Check("fixture-argument", argc == 2 || argc == 3);
    Setup(); Controls();
    const std::string mode(argv[1]);
    if (mode == "load" || mode == "load-control" || mode == "load-enum") { Check("public-load-path", argc == 3); PublicLoad(argv[2], mode != "load-control", mode == "load-enum"); }
    else if (mode == "reentrant") { Reentrant(); }
    else if (mode == "unload") { Unload(); }
    else { Check("registration-mode", mode == "registration"); }
    std::fprintf(stderr, "TYPEINFO_CATALOG_PASS %s\n", mode.c_str());
    return 0;
}
