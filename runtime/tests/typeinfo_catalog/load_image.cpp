// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <bits/stdc++.h>
#define private public
#include "ObjectModel/MClass.inline.h"
#include "TypeInfoManager.h"
#include "ObjectModel/PackageInfo.h"
#include "Loader/BinaryFile/CjFile/CjFileMeta.h"
#include "Loader/CjSemanticVersion/CjSemanticVersion.h"
#undef private
using namespace MapleRuntime;
namespace {
constexpr size_t stride = (sizeof(TypeInfo) + 15) & ~size_t(15);
struct alignas(16) Package { PackageInfo info {}; TypeInfo* types[2] {}; };
struct Image {
    CJFileHeader header {};
    const char* version {};
    CJGCFlagsTable flags {1, 1, 0};
    alignas(16) unsigned char types[2 * stride] {};
    Package package {};
    alignas(16) unsigned char typeExt[32] {};
#ifdef TYPEINFO_LOAD_ENUM
    alignas(TypeTemplate) unsigned char generic[sizeof(TypeTemplate)] {};
    EnumInfo enumInfo {};
    EnumCtorInfo ctor {};
#endif
} image;
}
extern "C" void MRT_LibraryOnLoad(uint64_t, bool);
extern "C" TypeInfo* CatalogLoadType(unsigned i) { return reinterpret_cast<TypeInfo*>(image.types + i * stride); }
#ifdef TYPEINFO_LOAD_ENUM
namespace {
TypeInfo* nestedType = nullptr;
void* EnumCtor(U32, TypeInfo**)
{
    auto* tt = reinterpret_cast<TypeTemplate*>(image.generic);
    TypeInfo* args[] {CatalogLoadType(0)};
    nestedType = TypeInfoManager::GetTypeInfoManager().GetOrCreateTypeInfo(tt, 1, args);
    return nestedType;
}
}
extern "C" TypeInfo* CatalogNestedType() { return nestedType; }
#endif
__attribute__((constructor)) static void LoadImage()
{
    const char* names[] = {"catalog.load:First", "catalog.load:Last"};
    for (unsigned i = 0; i < 2; ++i) {
        auto* ti = CatalogLoadType(i);
        ti->SetName(names[i]); ti->SetType(TypeKind::TYPE_KIND_INT64);
        ti->SetAlign(8); ti->SetInstanceSize(8);
        image.package.types[i] = ti;
    }
#ifdef TYPEINFO_LOAD_ENUM
    auto* tt = reinterpret_cast<TypeTemplate*>(image.generic);
    tt->name = "catalog.load:EnumArray"; tt->type = TypeKind::TYPE_KIND_RAWARRAY; tt->typeArgsNum = 1;
    image.header.tables[TYPE_TEMPLATE_TABLE] = {offsetof(Image, generic), sizeof(image.generic)};
    auto* enumeration = CatalogLoadType(1);
    enumeration->SetType(TypeKind::TYPE_KIND_ENUM);
    enumeration->SetEnumInfo(&image.enumInfo);
    image.enumInfo.SetCtorInfoNum(1);
    image.enumInfo.enumDebugInfo.enumCtorInfos.refOffset = reinterpret_cast<intptr_t>(&image.ctor) -
        reinterpret_cast<intptr_t>(&image.enumInfo.enumDebugInfo.enumCtorInfos);
    image.ctor.enumCtorFn = reinterpret_cast<FuncPtr*>(&EnumCtor);
#endif
    auto& pkg = image.package.info;
    pkg.typeInfoCnt = 2; pkg.isVaild = 1;
    pkg.packageName.refOffset = reinterpret_cast<intptr_t>("catalog.load") - reinterpret_cast<intptr_t>(&pkg.packageName);
    auto* ext = reinterpret_cast<TypeExt*>(image.typeExt);
    ext->ti = CatalogLoadType(0); ext->size = sizeof(image.typeExt);
    image.header.tables[TYPE_EXT_TABLE] = {offsetof(Image, typeExt), sizeof(image.typeExt)};
    image.version = CjSemanticVersion().GetRuntimeSDKVersion();
    image.header.cJFileSize = sizeof(image);
    image.header.cJSDKVersionOffset = offsetof(Image, version);
    image.header.tables[GC_FLAGS_TABLE] = {offsetof(Image, flags), sizeof(image.flags)};
    image.header.tables[TYPE_INFO_TABLE] = {offsetof(Image, types), sizeof(image.types)};
    image.header.tables[PACKINFO_TABLE] = {offsetof(Image, package), sizeof(image.package)};
    MRT_LibraryOnLoad(reinterpret_cast<uint64_t>(&image), true);
}
