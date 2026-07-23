// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_TYPE_INFO_MANAGER_H
#define MRT_TYPE_INFO_MANAGER_H

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ObjectModel/MClass.h"
#include "Base/SysCall.h"
#include "Base/RwLock.h"
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
#include <sys/mman.h>
#endif
#include "Base/HashUtils.h"
#include "Base/ImmortalWrapper.h"

namespace MapleRuntime {
class TypeGCInfo {
public:
    CString GetGCTibStr(TypeInfo* ti);
private:
    void FillTypeGCInfo(TypeInfo* ti, CString &gcTibStr, U32 &curSize);
    void FillArrayTypeGCInfo(TypeInfo* ti, CString &gcTibStr, U32 &curSize);
};
class TypeInfoManager {
    friend class TypeInfo;
    friend class CJFileLoader;
public:
    // 1024: number of buckets for the hash map.
    explicit TypeInfoManager() : genericTypeInfoDescMap(1024), genericTypeInfoFastMap(4096) {}
    ~TypeInfoManager() = default;

    void Init();
    void Fini();
    void NewMMap(size_t size);
    void FreeMMap(uintptr_t address, size_t size);
    static TypeInfoManager& GetTypeInfoManager();

    TypeInfo* GetOrCreateTypeInfo(TypeTemplate* tt, U32 argSize, TypeInfo* args[]);
    void InvalidateGenericTypeInfoFastMap();
    void AddTypeInfo(TypeInfo* ti);
    void ProbeRecordFETypeInfo(TypeInfo* ti);
    static U32 GetTypeSize(TypeInfo* ti);
    void ParseEnumInfo(TypeTemplate* tt, U32 argSize, TypeInfo* args[], TypeInfo* ti);
    void RecordMTableDesc(U32 uuid, MTableDesc* mTableDesc) { mTableList.emplace(uuid, mTableDesc); }
    MTableDesc* GetMTableDesc(U32 uuid)
    {
        auto it = mTableList.find(uuid);
        if (it != mTableList.end()) {
            return it->second;
        }
        return nullptr;
    }
    U16 GetTypeTemplateUUID(TypeTemplate* tt);
    void FillReflectInfo(TypeTemplate* tt, TypeInfo* ti);
    void InitAnyAndObjectType();
    TypeInfo* GetAnyTypeInfo() { return anyTi; }
    TypeInfo* GetObjectTypeInfo() { return objectTi; }
    void FillOffsets(TypeInfo* newTypeInfo, TypeTemplate* tt, U32 argSize, TypeInfo* args[]);
    void CalculateGCTib(TypeInfo* typeInfo);
private:
    uintptr_t Allocate(size_t size);
    CString GetGCTibStr(TypeInfo* typeInfo);
    void AddMTable(TypeTemplate* tt, TypeInfo* newTypeInfo, U32 argSize, TypeInfo* args[]);
    // Helper methods for copying method and parameter information
    void CopyParameterInfos(MethodInfo* ttMethodInfo, MethodInfo* tiMethodInfo);
    void CopyMethodInfo(MethodInfo* ttMethodInfo, MethodInfo* tiMethodInfo, TypeInfo* ti);

    bool IsEnumInfoReady(TypeTemplate* tt, TypeInfo* ti);
    EnumInfo* AllocateEnumInfo(EnumInfo* ttEnumInfo);
    EnumDebugInfo* AllocateEnumDebugInfo(EnumInfo* ttEnumInfo);
    void SetEnumConstructors(EnumDebugInfo* enumDebugInfo, EnumInfo* ttEnumInfo, U32 argSize, TypeInfo* args[]);
    void SetMethodInfos(EnumInfo* enumInfo, EnumInfo* ttEnumInfo, TypeInfo* ti);

    enum TypeInfoStatus : uint8_t {
        TYPEINFO_NOT_CREATED = 0,
        TYPEINFO_INITING,
        TYPEINFO_INITED,
    };
    class GenericTiDesc {
    public:
        GenericTiDesc(TypeTemplate* pTypeTemplate, U32 pArgSize, TypeInfo* pArgs[])
            : tt(pTypeTemplate), argSize(pArgSize), args(pArgs) {
            hash = computeHash();
        }

        GenericTiDesc(GenericTiDesc &desc)
        {
            tt = desc.tt;
            argSize = desc.argSize;
            hash = desc.hash;
            argsVector.assign(desc.args, desc.args + desc.argSize);
            tid.store(GetTid());
        }

#ifdef MRT_TYPEINFO_FAST_MAP_TEST
        GenericTiDesc() : tt(nullptr), argSize(0), args(nullptr), hash(0) {}
#endif

        GenericTiDesc& operator=(const GenericTiDesc& other) = delete;
        bool operator==(const GenericTiDesc &other) const;

        U32 GetHash() const { return hash; }
        TypeInfo* GetArg(U32 idx) const { return argsVector[idx]; }

        TypeInfo* typeInfo { nullptr };
        bool IsNotCreated() { return typeInfo == nullptr; }
        bool IsIniting() { return status.load(std::memory_order_acquire) == TypeInfoStatus::TYPEINFO_INITING; }
        bool IsInited() { return status.load(std::memory_order_acquire) == TypeInfoStatus::TYPEINFO_INITED; }
        void SetTypeInfoStatus(TypeInfoStatus tiStatus) { status.store(tiStatus, std::memory_order_release); }
        TypeInfoStatus GetTypeInfoStatus() { return status.load(std::memory_order_acquire); }
        std::atomic<U32> tid = { 0 };
        U32 computeHash();
        TypeTemplate* tt;
        U32 argSize;
        TypeInfo** args;
        U32 hash { 0 };
        U64 probeHashNs { 0 };
        U64 probeLockNs { 0 };
        U64 probeLookupNs { 0 };
        U64 probeScanNs { 0 };
        U32 probeTemplateUUIDEnsures { 0 };
        U32 probeArgumentUUIDEnsures { 0 };
        U32 probeScanLength { 0 };
    private:
        std::vector<TypeInfo*> argsVector = { };
        std::atomic<TypeInfoStatus> status { TypeInfoStatus::TYPEINFO_NOT_CREATED };
    };
    class GenericTiDescHashMap {
    public:
        explicit GenericTiDescHashMap(size_t numBuckets)
            : buckets(numBuckets) {}
        ~GenericTiDescHashMap()
        {
            for (auto &bucket : buckets) {
                for (auto &vec : bucket.maps) {
                    for (auto desc : vec.second) {
                        delete desc;
                        desc = nullptr;
                    }
                }
            }
            buckets.clear();
        }
        GenericTiDesc* InsertGenericTiDesc(GenericTiDesc& desc);
        GenericTiDesc* GetGenericTiDesc(GenericTiDesc& desc);
    private:
        struct Bucket {
            std::unordered_map<U32, std::vector<GenericTiDesc*>> maps;
            RwLock rwLock;
        };

        std::vector<Bucket> buckets;
    };
    class GenericTiDescFastMap {
    public:
        explicit GenericTiDescFastMap(size_t initialCapacity);
        ~GenericTiDescFastMap();
        GenericTiDesc* Get(TypeTemplate* tt, U32 argSize, TypeInfo* args[], U64& observedGeneration) const;
        void Insert(TypeTemplate* tt, U32 argSize, TypeInfo* args[], GenericTiDesc* desc, U64 expectedGeneration);
        void Invalidate();
    private:
        static constexpr U32 MAX_INLINE_ARGS = 3;
        struct Entry {
            Entry(U64 keyHash, TypeTemplate* keyTT, U32 keyArgSize, TypeInfo* keyArgs[], GenericTiDesc* value);
            bool Matches(U64 keyHash, TypeTemplate* keyTT, U32 keyArgSize, TypeInfo* keyArgs[]) const;

            U64 hash;
            TypeTemplate* tt;
            U32 argSize;
            std::array<TypeInfo*, MAX_INLINE_ARGS> args;
            GenericTiDesc* desc;
        };
        struct Table {
            explicit Table(size_t tableCapacity);
            ~Table();

            size_t capacity;
            size_t size { 0 };
            std::atomic<Entry*>* slots;
        };

        static size_t NormalizeCapacity(size_t capacity);
        static U64 ComputeHash(TypeTemplate* tt, U32 argSize, TypeInfo* args[]);
        static Entry* FindInTable(Table* table, U64 hash, TypeTemplate* tt, U32 argSize, TypeInfo* args[]);
        static void InsertIntoTable(Table* table, Entry* entry);
        void PublishResizedTable(size_t capacity);

        std::atomic<Table*> activeTable { nullptr };
        std::atomic<U64> generation { 1 };
        std::mutex writerMutex;
        std::vector<Table*> tables;
        std::vector<Entry*> entries;
    };
#ifdef MRT_TYPEINFO_FAST_MAP_TEST
public:
    using TestGenericTiDesc = GenericTiDesc;
    using TestGenericTiDescFastMap = GenericTiDescFastMap;
private:
#endif
    GenericTiDesc* InsertGenericTiDesc(GenericTiDesc& desc);
    GenericTiDesc* GetGenericTiDesc(GenericTiDesc& desc);
    GenericTiDesc* GetTypeInfo(TypeTemplate* tt, U32 argSize, TypeInfo* args[]);
    void CreatedTypeInfo(GenericTiDesc* &tiDesc, TypeTemplate* tt, U32 argSize, TypeInfo* args[]);
    void CreatedTypeInfoImpl(GenericTiDesc* &tiDesc, TypeTemplate* tt, U32 argSize, TypeInfo* args[]);
    void FillRemainingField(GenericTiDesc* &tiDesc, TypeTemplate* tt, U32 argSize, TypeInfo* args[]);

    size_t mapMemory = 1 * MB; // dynamic scaling, 1mb each time.
    std::atomic<uintptr_t> position;
    std::mutex ttMutex; //  guaranteed typeTemplates insert and find atomic
    std::recursive_mutex tiMutex; //  guaranteed nonGenericTypeInfo insert and find atomic
    std::unordered_map<const char*, TypeInfo*, HashString, EqualString> nonGenericTypeInfos;
    std::unordered_map<const char*, TypeInfo*, HashString, EqualString> genericTypeInfos;
    std::unordered_map<const char*, TypeTemplate*, HashString, EqualString> typeTemplates;
    GenericTiDescHashMap genericTypeInfoDescMap;
    GenericTiDescFastMap genericTypeInfoFastMap;
    uintptr_t startAddress;
    uintptr_t endAddress;
    std::atomic<U32> tiMaxUUID { 1 };
    std::atomic<U16> ttMaxUUID { 1 };
    TypeGCInfo typeGCInfo;
    std::vector<std::pair<uintptr_t, size_t>> mmapList;
    std::unordered_map<U32, MTableDesc*> mTableList;
    // Record two special TypeInfo, Any is the subclass of all types,
    // and Object is the superclass of all classes.
    // Because these two classes have no special tags,
    // record these two classes during initialization.
    TypeInfo* anyTi = nullptr;
    TypeInfo* objectTi = nullptr;
    std::mutex probeMutex;
    std::unordered_set<GenericTiDesc*> probeFEDescs;
    std::unordered_map<GenericTiDesc*, U64> probeQueryCounts;
    U64 probeTotalQueries = 0;
    U64 probeFEHits = 0;
    U64 probeNonFEHits = 0;
    U64 probeMisses = 0;
    U64 probeFastAttempts = 0;
    U64 probeFastHits = 0;
    U64 probeFastHitNs = 0;
    U64 probeFastMissNs = 0;
};
} // namespace MapleRuntime
#endif // MRT_TYPE_INFO_MANAGER_H
