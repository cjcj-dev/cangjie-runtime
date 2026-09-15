// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CjFileLoader.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

#include "ExceptionManager.inline.h"
#include "Common/ScopedObjectAccess.h"
#include "Loader/ElfUnloadQuiescence.h"
#include "LoaderManager.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "ObjectManager.inline.h"
#include "TypeInfoManager.h"
#include "UnwindStack/GcStackInfo.h"
namespace MapleRuntime {

void CJFileLoader::Fini()
{
    ClearLoadedFiles();
}

void CJFileLoader::RegisterLoadFile(Uptr fileMetaAddr)
{
    ScopedEntryTrace trace("CJRT_RegisterLoadFile");
    BaseFile* file = GetBaseFileByMetaAddr(fileMetaAddr);
    if (file == nullptr) {
        return;
    }
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    file->RegisterFile();
#ifndef __arm__
    AddPackageInfos(file);
#endif
    RegisterTypeExt(file);
    RegisterTypeInfoCreatedByFE(file);
    RegisterOuterTypeExtensions(file);
}

BaseFile* CJFileLoader::GetBaseFileByMetaAddr(Uptr fileMetaAddr)
{
    BaseFile* file = nullptr;
    VisitBaseFile([&file, &fileMetaAddr](BaseFile* cJfile) {
        if (cJfile->GetFileMetaAddr() == fileMetaAddr) {
            file = cJfile;
            return true;
        } else {
            return false;
        }
    });
    return file;
}

void CJFileLoader::UnregisterLoadFile(Uptr fileMetaAddr)
{
    BaseFile* file = GetBaseFileByMetaAddr(fileMetaAddr);
    if (file != nullptr) {
        // zGeneration.cpp:1340-1368: unlink -> observer handshake -> purge.
        // RemoveLoadedFiles accepts an image-specific authorization from the
        // public entry, or performs the complete direct-callback protection.
        RemoveLoadedFiles(file);
    }
}
void CJFileLoader::AddLoadedFiles(BaseFile* baseFile)
{
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    ElfUnloadQuiescence::LinkImage(baseFile->GetFileMetaAddr());
    loadedFiles.push_back(baseFile);
}

BaseFile* CJFileLoader::CreateFileRefFromAddr(Uptr fileMetaAddr)
{
    auto getBinaryInfoFromAddressFunc = GetBinaryInfoFromAddressFunc();
    CHECK(getBinaryInfoFromAddressFunc != nullptr);
    Os::Loader::BinaryInfo binInfo;
    int isGetBinInfoSuccess = getBinaryInfoFromAddressFunc(reinterpret_cast<void*>(fileMetaAddr), &binInfo);
    if (isGetBinInfoSuccess == 0) {
        isGetBinInfoSuccess = Os::Loader::GetBinaryInfoFromAddress(reinterpret_cast<void*>(fileMetaAddr), &binInfo);
    }
    CHECK(isGetBinInfoSuccess != 0);
    BaseFile* file = BaseFile::CreateCJFile(FileType::C_FILE, CString(binInfo.filePathName), fileMetaAddr);
    if (file == nullptr) {
        return nullptr;
    }
    return file;
}

void CJFileLoader::AddPackageInfos(BaseFile* baseFile)
{
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    Uptr packageInfoBase = baseFile->GetPackageInfoBase();
    U32 pkgTotalSize = baseFile->GetPackageInfoTotalSize();
    while (pkgTotalSize > 0) {
        PackageInfo* packageInfo = reinterpret_cast<PackageInfo*>(packageInfoBase);
        const char* pkgName = packageInfo->GetPackageName();
        auto pkgIt = packageInfos.find(pkgName);
        if (pkgIt == packageInfos.end()) {
            packageInfos.insert({ pkgName, packageInfo });
            // record the relation between file and the packageInfo,
            // identify whether multiple packages exist in a file.
            auto fileIt = filePackageMap.find(baseFile->GetBaseName().Str());
            if (fileIt == filePackageMap.end()) {
                std::vector<PackageInfo*> pkgs { packageInfo };
                filePackageMap.insert({ baseFile->GetBaseName().Str(), pkgs });
            } else {
                fileIt->second.push_back(packageInfo);
            }
        }

        size_t packageInfoSize = packageInfo->GetPackageSize();
        if (pkgTotalSize >= packageInfoSize) {
            pkgTotalSize -= packageInfoSize;
        } else {
            break;
        }
        packageInfoBase += packageInfoSize;
    }
}

bool CJFileLoader::FileHasLoaded(const char* path)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    CString baseName = Os::Path::GetBaseName(path);
    auto fileIt = filePackageMap.find(baseName.Str());
    if (fileIt != filePackageMap.end()) {
        return true;
    }
    return false;
}

bool CJFileLoader::FileHasMultiPackage(const char* path)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    CString baseName = Os::Path::GetBaseName(path);
    auto fileIt = filePackageMap.find(baseName.Str());
    if (fileIt != filePackageMap.end() && fileIt->second.size() > 1) {
        return true;
    }
    return false;
}

void CJFileLoader::GetSubPackages(PackageInfo* packageInfo, std::vector<PackageInfo*> &subPackages)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    CString prefix = CString(packageInfo->GetPackageName()) + ".";
    for (auto &pkgInfoPair : packageInfos) {
        PackageInfo* pkgInfo = pkgInfoPair.second;
        if (CString(pkgInfo->GetPackageName()).StartWith(prefix)) {
            subPackages.emplace_back(pkgInfo);
        }
    }
}

// Traverse outer extension data grouped by BaseFile
void CJFileLoader::VisitExtensionData(
    TypeInfo* ti, const std::function<bool(ExtensionData* ed)>& f, TypeTemplate* tt) const
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    ti->TryInitMTable();
    CHECK(loadedFiles.size() >= extensionDatas.size());
    for (auto baseFile : loadedFiles) {
        auto it1 = extensionDatas.find(baseFile);
        if (it1 == extensionDatas.end()) {
            continue;
        }
        auto& extensions = it1->second;
        auto range = extensions.equal_range(tt);
        if (range.first == range.second) {
            continue;
        }
        for (auto it2 = range.first; it2 != range.second; ++it2) {
            f(it2->second);
        }
    }
}

void CJFileLoader::ParseEnumCtor(TypeInfo* ti)
{
#ifdef __arm__
    return;
#endif
    TypeInfoManager& typeInfoMgr = TypeInfoManager::GetTypeInfoManager();
    if (ti->IsGenericTypeInfo()) {
        return typeInfoMgr.ParseEnumInfo(
            ti->GetSourceGeneric(), ti->GetTypeArgNum(), ti->GetTypeArgs(), ti);
    }
    EnumInfo* ei = ti->GetEnumInfo();
    if (ei == nullptr || ei->GetNumOfEnumCtor() == 0 || ei->IsParsed()) {
        return;
    }
    U32 enumCtorNum = ei->GetNumOfEnumCtor();
    for (U32 idx = 0; idx < enumCtorNum; ++idx) {
        EnumCtorInfo* enumCtorInfo = ei->GetEnumCtor(idx);
        void* fn = static_cast<void*>(enumCtorInfo->GetCtorFn());
        if (fn == nullptr) {
            continue;
        }
        TypeInfo* enumTi = reinterpret_cast<TypeInfo*>(
            TypeTemplate::ExecuteGenericFunc(fn, ti->GetTypeArgNum(), ti->GetTypeArgs()));
        enumCtorInfo->SetTypeInfo(enumTi);
    }
    ei->SetParsed();
}

void CJFileLoader::RegisterTypeExt(BaseFile* baseFile)
{
    Uptr typeExtBase = baseFile->GetTypeExtBase();
    Uptr typeExtEnd = typeExtBase + baseFile->GetTypeExtTotalSize();
    while (typeExtBase < typeExtEnd) {
        TypeExt* typeExt = reinterpret_cast<TypeExt*>(typeExtBase);
        constexpr uint32_t typeExtAlign = 16u;
        uint32_t sizeAlign = MRT_ALIGN(typeExt->size, typeExtAlign);
        typeExtBase += sizeAlign;
        typeExts.emplace(reinterpret_cast<void*>(typeExt->ti), typeExt);
    }
}

void CJFileLoader::RegisterTypeInfoCreatedByFE(BaseFile* baseFile)
{
    TypeInfoManager& typeInfoMgr = TypeInfoManager::GetTypeInfoManager();
    Uptr typeInfoBase = baseFile->GetTypeInfoBase();
    U32 typeInfoTotalSize = baseFile->GetTypeInfoTotalSize();
    typeInfoMgr.NoteTypeInfoImage(typeInfoBase, typeInfoTotalSize);
    Uptr typeInfoEnd = typeInfoBase + typeInfoTotalSize;
    while (typeInfoBase < typeInfoEnd) {
        TypeInfo* ti = reinterpret_cast<TypeInfo*>(typeInfoBase);
        constexpr uint32_t typeInfoAlign = 16u;
        constexpr uint32_t sizeAlign = MRT_ALIGN(sizeof(TypeInfo), typeInfoAlign);
        typeInfoBase += sizeAlign;
        auto tt = ti->GetSourceGeneric();
        if (tt != nullptr) {
            ti->SetvExtensionDataStart(tt->GetvExtensionDataStart());
        }
        typeInfoMgr.AddTypeInfo(ti);
        if (ti->IsEnum() || ti->IsTempEnum()) {
            ParseEnumCtor(ti);
        }
    }
    typeInfoMgr.InitAnyAndObjectType();

    Uptr staticGIBase = baseFile->GetStaticGIBase();
    Uptr staticGIEnd = staticGIBase + baseFile->GetStaticGISize();
    staticGIs.clear();
    while (staticGIBase < staticGIEnd) {
        I32 offset = *reinterpret_cast<I32*>(staticGIBase);
#if defined(__APPLE__)
        TypeInfo* ti = reinterpret_cast<TypeInfo*>(staticGIBase - offset);
#else
        TypeInfo* ti = reinterpret_cast<TypeInfo*>(staticGIBase + offset);
#endif
        staticGIBase += sizeof(I32);
        staticGIs.push_back(ti);
        if (ti->IsEnum() || ti->IsTempEnum()) {
            continue;
        } else if (ti->IsGenericTypeInfo() && ti->ReflectInfoIsNull() && !ti->GetSourceGeneric()->ReflectInfoIsNull()) {
            typeInfoMgr.FillReflectInfo(ti->GetSourceGeneric(), ti);
        }
    }
}

void CJFileLoader::RegisterOuterTypeExtensions(BaseFile* baseFile)
{
    TypeInfoManager& typeInfoMgr = TypeInfoManager::GetTypeInfoManager();
    Uptr extensionDataRefBase = baseFile->GetOuterTypeExtensionsBase();
    Uptr extensionDataRefEnd = extensionDataRefBase + baseFile->GetOuterTypeExtensionsSize();
    while (extensionDataRefBase < extensionDataRefEnd) {
        I32 offset = *reinterpret_cast<I32*>(extensionDataRefBase);
#ifdef __APPLE__
        ExtensionData* extensionData = reinterpret_cast<ExtensionData*>(extensionDataRefBase - offset);
#else
        ExtensionData* extensionData = reinterpret_cast<ExtensionData*>(extensionDataRefBase + offset);
#endif
        extensionDataRefBase += sizeof(I32);
        // for the extension of which target is a TypeInfo, since it cannot be used
        // in subsequent processes, we add MTable for it in advance so that it won't
        // need to be collected in `extensionDatas`.
        if (extensionData->TargetIsTypeInfo()) {
            TypeInfo* itf = extensionData->GetInterfaceTypeInfo();
            typeInfoMgr.AddTypeInfo(itf);
            TypeInfo* ti = reinterpret_cast<TypeInfo*>(extensionData->GetTargetType());
            typeInfoMgr.AddTypeInfo(ti);
            ti->AddMTable(itf, extensionData);
            continue;
        }
        TypeTemplate* tt = reinterpret_cast<TypeTemplate*>(extensionData->GetTargetType());
        extensionDatas[baseFile].emplace(tt, extensionData);
    }
}

bool CJFileLoader::VisitPackageInfoByPath(
    const char* path, const std::function<void(PackageInfo*)>& visitor)
{
    ElfUnloadQuiescence::ReadScope reader;
    PackageInfo* packageInfo = nullptr;
    {
        std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
        CString baseName = Os::Path::GetBaseName(path);
        auto fileIt = filePackageMap.find(baseName.Str());
        if (fileIt == filePackageMap.end()) {
            return false;
        }
        packageInfo = fileIt->second[0];
    }
    visitor(packageInfo);
    return true;
}

void CJFileLoader::RemovePackageInfo(const char* path)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    CString baseName = Os::Path::GetBaseName(path);
    auto fileIt = filePackageMap.find(baseName.Str());
    if (fileIt != filePackageMap.end()) {
        for (auto pkgInfo : fileIt->second) {
            packageInfos.erase(pkgInfo->GetPackageName());
        }
        filePackageMap.erase(baseName.Str());
    }
}

void CJFileLoader::RemovePackageInfo(BaseFile* baseFile)
{
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    CString baseName = baseFile->GetBaseName();
    auto fileIt = filePackageMap.find(baseName.Str());
    if (fileIt == filePackageMap.end()) {
        return;
    }

    std::unordered_set<PackageInfo*> removedPackages(fileIt->second.begin(), fileIt->second.end());
    for (PackageInfo* pkgInfo : fileIt->second) {
        auto pkgIt = packageInfos.find(pkgInfo->GetPackageName());
        if (pkgIt != packageInfos.end() && pkgIt->second == pkgInfo) {
            packageInfos.erase(pkgIt);
        }
    }
    filePackageMap.erase(fileIt);

    for (auto it = subPackageMap.begin(); it != subPackageMap.end();) {
        if (removedPackages.count(it->first) != 0) {
            it = subPackageMap.erase(it);
            continue;
        }
        auto& children = it->second;
        children.erase(std::remove_if(children.begin(), children.end(), [&removedPackages](PackageInfo* child) {
            return removedPackages.count(child) != 0;
        }), children.end());
        ++it;
    }
}

PackageInfo* CJFileLoader::GetPackageInfo(const char* pkgName) const
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    PackageInfo* pkgInfo = nullptr;
    auto it = packageInfos.find(pkgName);
    if (it != packageInfos.end()) {
        pkgInfo = it->second;
        if (!pkgInfo->IsVaild()) {
            return nullptr;
        }
        return pkgInfo;
    }
    return nullptr;
}

void CJFileLoader::UnlinkLoadedFile(BaseFile* baseFile)
{
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    extensionDatas.erase(baseFile);
    loadedFiles.remove(baseFile);

    for (auto it = typeInfoCache.begin(); it != typeInfoCache.end();) {
        if (baseFile->IsAddrInCJFile(reinterpret_cast<Uptr>(it->second))) {
            it = typeInfoCache.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = typeTemplateCache.begin(); it != typeTemplateCache.end();) {
        if (baseFile->IsAddrInCJFile(reinterpret_cast<Uptr>(it->second))) {
            it = typeTemplateCache.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = typeExts.begin(); it != typeExts.end();) {
        if (baseFile->IsAddrInCJFile(reinterpret_cast<Uptr>(it->second)) ||
            baseFile->IsAddrInCJFile(reinterpret_cast<Uptr>(it->first))) {
            it = typeExts.erase(it);
        } else {
            ++it;
        }
    }
    staticGIs.erase(std::remove_if(staticGIs.begin(), staticGIs.end(), [baseFile](TypeInfo* ti) {
        return baseFile->IsAddrInCJFile(reinterpret_cast<Uptr>(ti));
    }), staticGIs.end());
    RemovePackageInfo(baseFile);

    baseFile->UnregisterFile();
}

void CJFileLoader::PurgeLoadedFile(BaseFile* baseFile)
{
    Uptr imageAddress = baseFile->GetFileMetaAddr();
    CHECK_DETAIL(ElfUnloadQuiescence::IsPurgeAuthorized(imageAddress),
                 "ELF metadata purge requires pending/active authorization");
    TypeInfoManager::GetTypeInfoManager().RemoveTypeInfosInRange(
        baseFile->GetTypeInfoBase(), baseFile->GetTypeInfoTotalSize());
    ElfUnloadQuiescence::UnlinkImage(imageAddress);
    delete baseFile;
}

void CJFileLoader::RemoveLoadedFiles(BaseFile* baseFile)
{
    Uptr imageAddress = baseFile->GetFileMetaAddr();

    // A public unload callback runs on the same thread and inside the exact
    // pending/active/STW scopes that authorized this image. A direct callback
    // has no authorization and must establish all three protections itself.
    bool authorizedByCaller = ElfUnloadQuiescence::IsPurgeAuthorized(imageAddress);
    std::unique_ptr<ElfUnloadQuiescence::PurgeAuthorizationScope> callbackAuthorization;
    if (!authorizedByCaller && ElfUnloadQuiescence::HasCallerPurgeProtection()) {
        CHECK_DETAIL(!ElfUnloadQuiescence::CallerProtectionHasPendingForImage(imageAddress),
                     "ELF dependent image has a pending task during public unload");
        CHECK_DETAIL(!HasActiveImageFrames(baseFile),
                     "ELF dependent image has an active frame during public unload");
        callbackAuthorization =
            std::make_unique<ElfUnloadQuiescence::PurgeAuthorizationScope>(imageAddress);
        authorizedByCaller = true;
    }
    std::unique_ptr<ElfUnloadQuiescence::TaskAdmissionScope> directAdmission;
    if (!authorizedByCaller) {
#ifdef MRT_TESTABLE_INTERNALS
        ElfUnloadQuiescence::NoteDirectPreflightForTesting();
#endif
        directAdmission = std::make_unique<ElfUnloadQuiescence::TaskAdmissionScope>();
        directAdmission->WaitUntilNoPendingForImage(imageAddress);

        // The platform close cannot be rejected after its fini callback starts.
        // Wait outside the retirement cut until every active image frame leaves.
        for (;;) {
            bool active = false;
            {
                ScopedEnterSaferegion enterSaferegion(false);
                ScopedStopTheWorld preflight("direct ELF unload active-image preflight", false);
                active = HasActiveImageFrames(baseFile);
            }
            if (!active) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    ElfUnloadQuiescence::UnloadScope unload(imageAddress);

    // Unlink every discovery surface while pre-cut readers retain their use
    // right. StaticRootTable::UnregisterRoots also waits for an in-flight GC
    // root visitor because both operations hold gcRootsLock.
    UnlinkLoadedFile(baseFile);
#ifdef MRT_TESTABLE_INTERNALS
    if (!authorizedByCaller) {
        ElfUnloadQuiescence::NoteDirectUnlinkForTesting();
    }
#endif

    // GC code lookup readers are not mutators, so drain them explicitly before
    // the mutator rendezvous. Admission remains closed through purge.
    unload.Synchronize();

    if (!authorizedByCaller) {
        ScopedEnterSaferegion enterSaferegion(false);
        ScopedStopTheWorld handshake("ELF unload quiescence", false);
#ifdef MRT_TESTABLE_INTERNALS
        ElfUnloadQuiescence::NoteDirectHandshakeForTesting();
#endif
        CHECK_DETAIL(!HasActiveImageFrames(baseFile),
                     "ELF image became active after direct unload preflight");
        ElfUnloadQuiescence::PurgeAuthorizationScope authorization(imageAddress);
        PurgeLoadedFile(baseFile);
#ifdef MRT_TESTABLE_INTERNALS
        ElfUnloadQuiescence::NoteDirectPurgeForTesting();
#endif
        unload.OpenAdmission();
        return;
    }
    PurgeLoadedFile(baseFile);
    unload.OpenAdmission();
}

void CJFileLoader::VisitBaseFile(const std::function<bool(BaseFile*)>& f) const
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    for (auto file : loadedFiles) {
        if (f(file)) {
            return;
        }
    }
}

TypeInfo* CJFileLoader::FindTypeInfoFromLoadedFiles(const char* typeInfoName)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    auto it = typeInfoCache.find(typeInfoName);
    if (it != typeInfoCache.end()) {
        return it->second;
    }
    CString pkgName;
    CString typeInfoNameStr = CString(typeInfoName);
    int idx = typeInfoNameStr.RFind(":");
    if (idx < 0) {
        pkgName = "std.core";
    } else {
        pkgName = typeInfoNameStr.SubStr(0, idx);
    }
    auto pkgIt = packageInfos.find(pkgName.Str());
    if (pkgIt != packageInfos.end()) {
        PackageInfo* pkgInfo = pkgIt->second;
        TypeInfo* ti = pkgInfo->GetTypeInfo(typeInfoName);
        if (ti == nullptr) {
            return nullptr;
        }
        typeInfoCache.insert({ ti->GetName(), ti });
        return ti;
    }
    return nullptr;
}

TypeTemplate* CJFileLoader::FindTypeTemplateFromLoadedFiles(const char* typeTemplateName)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    auto it = typeTemplateCache.find(typeTemplateName);
    if (it != typeTemplateCache.end()) {
        return it->second;
    }
    CString pkgName;
    CString typeTemplateNameStr = CString(typeTemplateName);
    int idx = typeTemplateNameStr.RFind(":");
    if (idx < 0) {
        pkgName = "std.core";
    } else {
        pkgName = typeTemplateNameStr.SubStr(0, idx);
    }
    auto pkgIt = packageInfos.find(pkgName.Str());
    if (pkgIt != packageInfos.end()) {
        PackageInfo* pkgInfo = pkgIt->second;
        TypeTemplate* tt = pkgInfo->GetTypeTemplate(typeTemplateName);
        if (tt == nullptr) {
            return nullptr;
        }
        typeTemplateCache.insert({ tt->GetName(), tt });
        return tt;
    }
    return nullptr;
}

void CJFileLoader::RecordTypeInfo(TypeInfo* ti)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    typeInfoCache.insert({ ti->GetName(), ti });
}

void CJFileLoader::ClearLoadedFiles()
{
    // CangjieRuntime clears Runtime::runtime before module finalization, so a
    // shutdown cleanup cannot create ScopedStopTheWorld or query
    // MutatorManager::Instance(). Both runtime exits retire their entry mutator
    // and stop the scheduler before FiniAndDelete, so no active image frame can
    // be created or remain here. Close task admission for the complete cleanup
    // and prove the remaining pending side explicitly for every image.
    CHECK_DETAIL(Runtime::CurrentRef() == nullptr,
                 "ELF shutdown cleanup requires a stopped runtime");
    ElfUnloadQuiescence::TaskAdmissionScope shutdownAdmission;
    for (;;) {
        BaseFile* file = nullptr;
        {
            std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
            if (loadedFiles.empty()) {
                return;
            }
            file = loadedFiles.front();
        }
        Uptr imageAddress = file->GetFileMetaAddr();
        shutdownAdmission.WaitUntilNoPendingForImage(imageAddress);
        ElfUnloadQuiescence::PurgeAuthorizationScope authorization(
            imageAddress, shutdownAdmission);
        RemoveLoadedFiles(file);
    }
}

bool CJFileLoader::LibInit(const char* libName)
{
    ElfUnloadQuiescence::ReadScope reader;
    BaseFile* baseFile = GetBaseFile(libName);
    if (baseFile == nullptr) {
        return false;
    }
    return DoInitImage(baseFile);
}

#ifdef __OHOS__
void CJFileLoader::RegisterLoadFunc(void* loadFunc, void* loadLibraryFunc)
{
    binLoadApi.binLoad = (void*(*)(const char*))(loadFunc);
    binLoadApi.binLoadLib = (void*(*)(LibraryKind, const char*))(loadLibraryFunc);
}
#endif

void* CJFileLoader::LoadCJLibrary(const char* libName)
{
    void* handler = binLoadApi.binLoad(libName);
    if (handler != nullptr) {
        std::lock_guard<std::mutex> lock(libCjsoHandlersMutex);
        CString baseName = Os::Path::GetBaseName(libName);
        auto handlerIt =
            std::find_if(cjLibHandlers.begin(), cjLibHandlers.end(), [&baseName](const LibNameToHandler& info) {
                return baseName == Os::Path::GetBaseName(info.baseName.Str());
            });
        if (handlerIt == cjLibHandlers.end()) {
            cjLibHandlers.push_back({ baseName, handler });
        }
    }
    return handler;
}

#ifdef INTERPRETER_ENABLED
void* CJFileLoader::LoadInterpreter(const char* libName)
{
    if (binLoadApi.binLoadLib == nullptr) {
        return nullptr;
    }
    
    return binLoadApi.binLoadLib(LibraryKind::APP, libName);
}
#endif

int CJFileLoader::UnloadLibrary(const char* libName)
{
    if (libName == nullptr) {
        return -1;
    }
    CString baseName = Os::Path::GetBaseName(libName);
    std::lock_guard<std::mutex> lock(libCjsoHandlersMutex);
    auto handlerIt =
        std::find_if(cjLibHandlers.begin(), cjLibHandlers.end(), [&baseName](const LibNameToHandler& info) {
            return baseName == Os::Path::GetBaseName(info.baseName.Str());
        });
    if (handlerIt == cjLibHandlers.end()) {
        return -1;
    }

    int ret = -1;
    BaseFile* baseFile = GetBaseFile(baseName);
    if (LoaderManager::GetInstance()->GetInitStatus()) {
        if (baseFile == nullptr) {
            return -1;
        }
        // A FutureImpl retains the image entry before its cjthread becomes an
        // active mutator. Close that transition first, then reject queued image
        // entries before using the active-frame preflight for started tasks.
        ElfUnloadQuiescence::TaskAdmissionScope taskAdmission;
        if (taskAdmission.HasPendingForImage(baseFile->GetFileMetaAddr())) {
            LOG(RTLOG_WARNING, "refuse to unload queued Cangjie image %s", baseName.Str());
            return -1;
        }
        // Keep every managed entry stopped from the active-frame decision
        // through the platform unmap. A mutator blocked in C2N is already in a
        // saferegion, but its managed caller is still an active image frame and
        // therefore makes this unload ineligible.
        ScopedEnterSaferegion enterSaferegion(false);
        ScopedStopTheWorld stw("ELF unload active-image preflight", false);
        if (HasActiveImageFrames(baseFile)) {
            LOG(RTLOG_WARNING, "refuse to unload active Cangjie image %s", baseName.Str());
            return -1;
        }
        ElfUnloadQuiescence::PurgeAuthorizationScope authorization(
            baseFile->GetFileMetaAddr(), taskAdmission);
        ret = binLoadApi.binUnload(handlerIt->handler);
    } else {
        ret = binLoadApi.binUnload(handlerIt->handler);
    }
    if (ret == 0) {
        cjLibHandlers.erase(handlerIt);
    }
    return ret;
}

bool CJFileLoader::HasActiveImageFrames(BaseFile* baseFile) const
{
    ElfUnloadQuiescence::ReadScope metadataReader;
    bool active = false;
    const Uptr imageAddress = baseFile->GetFileMetaAddr();
    MutatorManager::Instance().VisitAllMutatorsExceptFinalizer([&](Mutator& mutator) {
        if (active || !mutator.IsManagedContext()) {
            return;
        }
        GCStackInfo stackInfo(&mutator.GetUnwindContext());
        stackInfo.FillInStackTrace();
        for (const FrameInfo& frame : stackInfo.GetStack()) {
            Uptr startPC = reinterpret_cast<Uptr>(frame.GetFuncStartPC());
            Uptr framePC = reinterpret_cast<Uptr>(frame.mFrame.GetIP());
            if (ElfUnloadQuiescence::IsAddressInImage(startPC, imageAddress) ||
                ElfUnloadQuiescence::IsAddressInImage(framePC, imageAddress)) {
                active = true;
                return;
            }
        }
    });
    return active;
}

Uptr CJFileLoader::FindSymbol(const CString libName, const CString symName) const
{
    CString baseName = Os::Path::GetBaseName(libName.Str());
    std::lock_guard<std::mutex> lock(libCjsoHandlersMutex);
    auto handlerIt =
        std::find_if(cjLibHandlers.begin(), cjLibHandlers.end(), [&baseName](const LibNameToHandler& info) {
            return baseName == Os::Path::GetBaseName(info.baseName.Str());
        });
    if (handlerIt == cjLibHandlers.end()) {
        return 0;
    }
    return reinterpret_cast<Uptr>(binLoadApi.findSymbol(handlerIt->handler, symName.Str()));
}

#ifdef MRT_TESTABLE_INTERNALS
void* CJFileLoader::GetLibraryHandleForTesting(const char* libName) const
{
    CString baseName = Os::Path::GetBaseName(libName);
    std::lock_guard<std::mutex> lock(libCjsoHandlersMutex);
    auto handlerIt =
        std::find_if(cjLibHandlers.begin(), cjLibHandlers.end(), [&baseName](const LibNameToHandler& info) {
            return baseName == Os::Path::GetBaseName(info.baseName.Str());
        });
    return handlerIt == cjLibHandlers.end() ? nullptr : handlerIt->handler;
}
#endif

bool CJFileLoader::DoInitImage(BaseFile* baseFile) const
{
    ScopedEntryTrace trace((CString("CJRT_INIT_LIBRARY_") + baseFile->GetBaseName()).Str());
    std::vector<Uptr> funcs;
    baseFile->GetGlobalInitFunc(funcs);
    for (Uptr func : funcs) {
        if (reinterpret_cast<void*>(func) != nullptr) {
            using FuncType = void (*)();
            FuncType initAddr = reinterpret_cast<FuncType>(func);
#if defined(__OHOS__) || defined(__IOS__)
            InitCJLibraryStub(reinterpret_cast<void*>(initAddr));
#else
            Mutator* mutator = ThreadLocal::GetMutator();
            if (mutator != nullptr) {
                mutator->SetManagedContext(true);
            }
            uintptr_t threadData = MapleRuntime::MRT_GetThreadLocalData();
            ExecuteCangjieStub(0, 0, 0, reinterpret_cast<void*>(initAddr), reinterpret_cast<void*>(threadData), 0);
            if (mutator != nullptr) {
                mutator->SetManagedContext(false);
            }
            if (ExceptionManager::HasPendingException()) {
                ExceptionRef ex = ExceptionManager::GetPendingException();
                LOG(RTLOG_ERROR, "Init Image fail! exception occurrence when init image, exception:%s ",
                    ex->GetTypeInfo()->GetName());
                ExceptionManager::ClearPendingException();
                return false;
            }
#endif
        }
    }
    return true;
}

BaseFile* CJFileLoader::GetBaseFile(CString fileName) const
{
    BaseFile* baseFile = nullptr;
    CString baseName = Os::Path::GetBaseName(fileName.Str());
    VisitBaseFile([&baseName, &baseFile](BaseFile* file) {
        if (file->GetBaseName() == baseName) {
            baseFile = file;
            return true;
        } else {
            return false;
        }
    });
    return baseFile;
}

bool CJFileLoader::CheckPackageCompatibility(BaseFile* file)
{
    if (file == nullptr) {
        return false;
    }
    CString packageName = file->GetRealPath();
    CString packageVersion = file->GetSDKVersion();
    bool isCompatible = compatibility.CheckPackageCompatibility(packageName, packageVersion);
    file->SetFileCompatibility(isCompatible);
    AddLoadedFiles(file);
    return isCompatible;
}

void CJFileLoader::TryThrowException(Uptr fileMetaAddr)
{
    BaseFile* file = GetBaseFileByMetaAddr(fileMetaAddr);
    if (file == nullptr || file->IsCompatible()) {
        return;
    }
    CString packageName = file->GetRealPath();
    CString packageVersion = file->GetSDKVersion();
    CString msg = "executable cangjie file ";
    msg.Append(packageName);
    msg.Append(CString::FormatString(" version %s is not compatible with deployed cangjie runtime version %s",
        packageVersion.Str(), compatibility.GetRuntimeSDKVersion()));
#ifndef DISABLE_VERSION_CHECK
    ExceptionManager::IncompatiblePackageExpection(msg);
    RemoveLoadedFiles(file);
#else
    LOG(RTLOG_WARNING, "%s", msg.Str());
#endif
}

U32 CJFileLoader::GetNumOfInterface(TypeInfo* ti)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::vector<TypeInfo*> itfs;
    ti->GetInterfaces(itfs);
    return itfs.size();
}

TypeInfo* CJFileLoader::GetInterface(TypeInfo* ti, U32 idx)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::vector<TypeInfo*> itfs;
    ti->GetInterfaces(itfs);
    if (idx >= itfs.size()) {
        return nullptr;
    }
    return itfs[idx];
}

TypeExt* CJFileLoader::GetTypeExt(void* type)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    auto it = typeExts.find(type);
    return it == typeExts.end() ? nullptr : it->second;
}

#ifdef MRT_TESTABLE_INTERNALS
size_t CJFileLoader::GetPackageIndexSizeForTesting() const
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    return filePackageMap.size();
}
#endif
} // namespace MapleRuntime
