// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_ELF_UNLOAD_QUIESCENCE_H
#define MRT_ELF_UNLOAD_QUIESCENCE_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

#include "Base/Types.h"

namespace MapleRuntime {

// ZGC zUnload.cpp:126-173 / zGeneration.cpp:1330-1388:
// unlink first, rendezvous every observer, then purge the backing storage.
//
// A ReadScope covers the complete use of metadata obtained from an image. An
// UnloadScope closes admission before product registries are unlinked and its
// Synchronize() waits for every reader admitted before that cut. New readers
// wait for the unload to finish and therefore can only observe the post-unlink
// registries.
class ElfUnloadQuiescence final {
public:
    enum class ReaderKind : U8 {
        GENERIC,
        GC_STACK_ENTRY,
    };

    class ReadScope final {
    public:
        explicit ReadScope(ReaderKind kind = ReaderKind::GENERIC);
        ~ReadScope();

        ReadScope(const ReadScope&) = delete;
        ReadScope& operator=(const ReadScope&) = delete;

    private:
        bool active { false };
    };

    class UnloadScope final {
    public:
        explicit UnloadScope(Uptr imageAddress);
        ~UnloadScope();

        UnloadScope(const UnloadScope&) = delete;
        UnloadScope& operator=(const UnloadScope&) = delete;

        void Synchronize();
        void OpenAdmission();

    private:
        std::unique_lock<std::mutex> writerLock;
        Uptr imageIdentity { 0 };
        bool synchronized { false };
        bool admissionOpen { false };
    };

    // A task entry found through FindCJSymbol remains a code reference from
    // queue insertion through the return from ExecuteCangjieStub. PendingTask
    // records that complete interval; CompletionScope closes its final removal
    // against an unload preflight.
    class TaskAdmissionScope;

    class PendingTask final {
    public:
        explicit PendingTask(Uptr entryAddress);
        ~PendingTask();

        PendingTask(const PendingTask&) = delete;
        PendingTask& operator=(const PendingTask&) = delete;

        class CompletionScope final {
        public:
            explicit CompletionScope(PendingTask& task);
            ~CompletionScope() = default;

            CompletionScope(const CompletionScope&) = delete;
            CompletionScope& operator=(const CompletionScope&) = delete;
        };

    private:
        friend class TaskAdmissionScope;
        void MarkCompleted();

        Uptr entry { 0 };
        bool pending { false };
    };

    // Serializes the pending-task check and active-frame preflight with task
    // submission/start. Hold this scope through the platform unload.
    class TaskAdmissionScope final {
    public:
        TaskAdmissionScope();
        ~TaskAdmissionScope() = default;

        TaskAdmissionScope(const TaskAdmissionScope&) = delete;
        TaskAdmissionScope& operator=(const TaskAdmissionScope&) = delete;

        bool HasPendingForImage(Uptr imageAddress) const;
        void WaitUntilNoPendingForImage(Uptr imageAddress) const;

    private:
        std::unique_lock<std::shared_timed_mutex> admissionLock;
    };

    // A purge authorization is image-specific and thread-bound. The public
    // unload entry creates it while holding its own pending/active preflight
    // and STW scopes; a direct platform callback must create the same proof
    // only after performing its own rendezvous.
    class PurgeAuthorizationScope final {
    public:
        explicit PurgeAuthorizationScope(Uptr imageAddress);
        PurgeAuthorizationScope(Uptr imageAddress, const TaskAdmissionScope& admission);
        ~PurgeAuthorizationScope();

        PurgeAuthorizationScope(const PurgeAuthorizationScope&) = delete;
        PurgeAuthorizationScope& operator=(const PurgeAuthorizationScope&) = delete;

    private:
        Uptr imageIdentity { 0 };
        Uptr previousImageIdentity { 0 };
        const TaskAdmissionScope* previousAdmission { nullptr };
    };

    static void LinkImage(Uptr imageAddress);
    static void UnlinkImage(Uptr imageAddress);
    static bool IsLinkedAddress(Uptr address);
    static bool IsAddressInImage(Uptr address, Uptr imageAddress);
    static bool IsPurgeAuthorized(Uptr imageAddress);
    static bool HasCallerPurgeProtection();
    static bool CallerProtectionHasPendingForImage(Uptr imageAddress);
    static void AssertReaderActive();
#ifdef MRT_TESTABLE_INTERNALS
    static Uptr ImageIdentityForTesting(Uptr address);
    static bool IsImageIdentityLinkedForTesting(Uptr imageIdentity);
    static bool IsUnloadPendingForTesting();
    static U64 GcEntryCountForTesting();
    static void ResetDirectOrderForTesting();
    static void NoteDirectPreflightForTesting();
    static bool DirectPreflightEnteredForTesting();
    static void NoteDirectUnlinkForTesting();
    static void NoteDirectHandshakeForTesting();
    static void NoteDirectPurgeForTesting();
    static bool DirectOrderValidForTesting();
    static void EnableGcReaderPauseForTesting();
    static bool GcReaderPausedForTesting();
    static void ReleaseGcReaderPauseForTesting();
    static void PauseGcReaderForTesting();
    static void EnablePackageReaderPauseForTesting();
    static bool PackageReaderPausedForTesting();
    static void ReleasePackageReaderPauseForTesting();
    static void PausePackageReaderForTesting();
#endif

private:
    static constexpr U64 WRITER_BIT = U64 { 1 } << 63;
    static constexpr U64 READER_MASK = ~WRITER_BIT;

    static std::atomic<U64>& State();
    static std::mutex& WriterMutex();
    static std::mutex& DrainMutex();
    static std::condition_variable& DrainCondition();
    static std::shared_timed_mutex& TaskAdmissionMutex();
    static std::mutex& PendingTaskMutex();
    static std::condition_variable& PendingTaskCondition();
    static std::unordered_set<PendingTask*>& PendingTasks();
    static Uptr ResolveImageIdentity(Uptr address);
};

} // namespace MapleRuntime

#endif // MRT_ELF_UNLOAD_QUIESCENCE_H
