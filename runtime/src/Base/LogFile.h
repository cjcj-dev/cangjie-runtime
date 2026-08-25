// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_LOG_FILE_H
#define MRT_LOG_FILE_H

#include <cstring>
#include <mutex>

#include "Base/GcLog.h"
#include "Base/Log.h"
#include "Base/Macros.h"
#include "Base/TimeUtils.h"
#include "Base/ZStat.h"
#include "Cangjie.h"
#include "Interpreter/Options.h"

namespace MapleRuntime {
enum LogType {
    // for overall brief report
    REPORT = 0,

    // for debug purpose
    DEBUG,

    // for allocator
    ALLOC,
    REGION,
    FRAGMENT,

    // for barriers
    BARRIER,  // idle phase
    EBARRIER, // enum phase
    TBARRIER, // trace phase
    PBARRIER, // preforward phase
    FBARRIER, // forward phase

    // for gc
    GCPHASE,
    ENUM,
    TRACE,
    PREFORWARD,
    FORWARD,
    FIX,
    FINALIZE,

    UNWIND,
    EXCEPTION,
    SIGNAL,

    CJTHREAD,

    INTERPRETER,

#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
    SANITIZER,
#endif

    LOG_TYPE_NUMBER
};

#ifndef DEFAULT_MRT_REPORT
#define DEFAULT_MRT_REPORT 0
#endif // DEFAULT_MRT_REPORT

#ifndef DEFAULT_MRT_LOG_ALLOC
#define DEFAULT_MRT_LOG_ALLOC 0
#endif // DEFAULT_MRT_LOG_ALLOC

#ifndef DEFAULT_MRT_LOG_REGION
#define DEFAULT_MRT_LOG_REGION 0
#endif // DEFAULT_MRT_LOG_REGION

#ifndef DEFAULT_MRT_LOG_FRAGMENT
#define DEFAULT_MRT_LOG_FRAGMENT 0
#endif // DEFAULT_MRT_LOG_FRAGMENT

#ifndef DEFAULT_MRT_LOG_DEBUG
#define DEFAULT_MRT_LOG_DEBUG 0
#endif // DEFAULT_MRT_LOG_DEBUG

#ifndef DEFAULT_MRT_LOG_BARRIER
#define DEFAULT_MRT_LOG_BARRIER 0
#endif // DEFAULT_MRT_LOG_BARRIER

#ifndef DEFAULT_MRT_LOG_EBARRIER
#define DEFAULT_MRT_LOG_EBARRIER 0
#endif // DEFAULT_MRT_LOG_EBARRIER

#ifndef DEFAULT_MRT_LOG_TBARRIER
#define DEFAULT_MRT_LOG_TBARRIER 0
#endif // DEFAULT_MRT_LOG_TBARRIER

#ifndef DEFAULT_MRT_LOG_PBARRIER
#define DEFAULT_MRT_LOG_PBARRIER 0
#endif // DEFAULT_MRT_LOG_PBARRIER

#ifndef DEFAULT_MRT_LOG_FBARRIER
#define DEFAULT_MRT_LOG_FBARRIER 0
#endif // DEFAULT_MRT_LOG_FBARRIER

#ifndef DEFAULT_MRT_LOG_GCPHASE
#define DEFAULT_MRT_LOG_GCPHASE 0
#endif // DEFAULT_MRT_LOG_GCPHASE

#ifndef DEFAULT_MRT_LOG_ENUM
#define DEFAULT_MRT_LOG_ENUM 0
#endif // DEFAULT_MRT_LOG_ENUM

#ifndef DEFAULT_MRT_LOG_TRACE
#define DEFAULT_MRT_LOG_TRACE 0
#endif // DEFAULT_MRT_LOG_TRACE

#ifndef DEFAULT_MRT_LOG_PREFORWARD
#define DEFAULT_MRT_LOG_PREFORWARD 0
#endif // DEFAULT_MRT_LOG_PREFORWARD

#ifndef DEFAULT_MRT_LOG_FORWARD
#define DEFAULT_MRT_LOG_FORWARD 0
#endif // DEFAULT_MRT_LOG_FORWARD

#ifndef DEFAULT_MRT_LOG_FIX
#define DEFAULT_MRT_LOG_FIX 0
#endif // DEFAULT_MRT_LOG_FIX

#ifndef DEFAULT_MRT_LOG_FINALIZE
#define DEFAULT_MRT_LOG_FINALIZE 0
#endif // DEFAULT_MRT_LOG_FINALIZE

#ifndef DEFAULT_MRT_LOG_UNWIND
#define DEFAULT_MRT_LOG_UNWIND 0
#endif // DEFAULT_MRT_LOG_UNWIND

#ifndef DEFAULT_MRT_LOG_EXCEPTION
#define DEFAULT_MRT_LOG_EXCEPTION 0
#endif // DEFAULT_MRT_LOG_EXCEPTION

#ifndef DEFAULT_MRT_LOG_SIGNAL
#define DEFAULT_MRT_LOG_SIGNAL 0
#endif // DEFAULT_MRT_LOG_SIGNAL

#ifndef DEFAULT_MRT_LOG_CJTHREAD
#define DEFAULT_MRT_LOG_CJTHREAD 0
#endif // DEFAULT_MRT_LOG_CJTHREAD

#ifndef DEFAULT_MRT_LOG_INTERPRETER
#define DEFAULT_MRT_LOG_INTERPRETER 0
#endif // DEFAULT_MRT_LOG_INTERPRETER

#ifndef DEFAULT_MRT_LOG2STDOUT
#define DEFAULT_MRT_LOG2STDOUT 0
#endif // DEFAULT_MRT_LOG2STDOUT

#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
#ifndef DEFAULT_MRT_LOG_SANITIZER
#define DEFAULT_MRT_LOG_SANITIZER 0
#endif // DEFAULT_MRT_LOG_SANITIZER
#endif

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
long GetEnv(const char* envName, long defaultValue); // do not use directly

// Use this macro to get environment variable for log.
// For example: MRT_ENABLED_LOG(MRT_REPORT)
// Will first check if an environment variable "MRT_REPORT" is present,
// and is a valid integer, and use its value. If not present or not a valid
// integer, it will fall back to the default value of the MRT_REPORT
// macro.  This lets the user override configuration at run time, which is useful
// for debugging.
#define MRT_ENABLED_LOG(conf) (MapleRuntime::GetEnv(#conf, DEFAULT_##conf) == 1)
#else
#define MRT_ENABLED_LOG(conf) (0)
#endif

CString Pretty(uint64_t number) noexcept;
CString PrettyOrderInfo(uint64_t number, const char* unit);
CString PrettyOrderMathNano(uint64_t number, const char* unit);
RTLogLevel InitLogLevel();

void WriteLog(bool addPrefix, LogType type, const char* format, ...) noexcept;

#define ENABLE_LOG(type) LogFile::LogIsEnabled(type)

#if defined (__OHOS__)
#define VLOG(type, format...) \
    if (type == LogType::REPORT) { \
        LOG(RTLOG_INFO, format); \
    } else if (LogFile::LogIsEnabled(type)) { \
        WriteLog(true, type, format); \
    }
#else
#define VLOG(type, format...) \
    if (LogFile::LogIsEnabled(type)) { \
        WriteLog(true, type, format); \
    }
#endif

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
#define DLOG(type, format...) VLOG(type, format)
#else
#define DLOG(type, format...) (void)(0)
#endif

// Macro for cjthread log and use it after ENABLE_LOG for judgment.
#define VLOG_CJTHREAD(format...) WriteLog(true, CJTHREAD, format)

class LogFile {
public:
    LogFile() = default;
    ~LogFile() = default;
    static void Init();
    static void Fini();

    struct LogFileItem {
        bool enableLog = false;
        std::mutex fileMutex;
        FILE* file = nullptr;
        size_t maxFileSize = DEFAULT_MAX_FILE_SIZE;
        size_t curPosLocation = 0;
    };

    static FILE* GetFile(LogType type) { return logFile[type].file; }

    static void LogFileLock(LogType type) { logFile[type].fileMutex.lock(); }

    static void LogFileUnLock(LogType type) { logFile[type].fileMutex.unlock(); }

    static bool LogIsEnabled(LogType type) noexcept
    {
#if (defined(__OHOS__) && (__OHOS__ == 1))
        if (type == REPORT) {
            return true;
        }
#endif
        return logFile[type].enableLog;
    }

    static void EnableLog(LogType type, bool key) { logFile[type].enableLog = key; }

    static size_t GetMaxFileSize(LogType type) { return logFile[type].maxFileSize; }

    static size_t GetCurPosLocation(LogType type) { return logFile[type].curPosLocation; }

    static void SetCurPosLocation(LogType type, size_t curPos)
    {
        logFile[type].curPosLocation = curPos;
    }

    static RTLogLevel GetLogLevel() { return logLevel; }

private:
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    static void OpenLogFiles();
#endif
    static void CloseLogFiles();

    static void SetFlagWithEnv(const char* env, LogType type);

    static void SetFlags();
    static LogFileItem logFile[LOG_TYPE_NUMBER];

    static RTLogLevel logLevel;
};

#define MRT_PHASE_TIMER(...) Timer MRT_pt_##__LINE__(__VA_ARGS__)

// The sole out-of-line Timer → GCLOG bearing point.  Inclusive and structural-leaf records receive
// the same constructor-captured seq, name and nanosecond sample, so neither schema can drift.
// `kind` is -1 unknown, 0 concurrent, 1 pause.
void EmitTimerRecords(uint64_t seq, const char* name, uint64_t startNs, uint64_t ns, bool isLeaf, int kind, uint64_t depth,
                      bool pathOk, const char* path);

class Timer {
public:
    explicit Timer(const CString& pName, LogType type = REPORT) : name(pName), logType(type)
    {
        // Time when either the human VLOG channel or structured GC log needs the duration.
        // ENABLE_LOG alone used to gate phase records; under DEFAULT_MRT_REPORT=0 that
        // silently dropped rec=phase even with MRT_GC_LOG=1.
        zstatActive = ZStat::Enabled();
        gcLogActive = GcLog::Enabled();
        if (ENABLE_LOG(type) || gcLogActive || zstatActive) {
            startTimeNs = TimeUtil::NanoSeconds();
            active = true;
            // FINALIZE timers run on a lifecycle thread that does not participate in the active
            // collection.  Its LogType is the structural ownership marker: even when that thread
            // overlaps a process-wide cycle, its work remains unowned (seq=0).  All GC phase
            // timers use the default REPORT type and retain the active cycle, including nonpillar
            // phases such as young.flush_alloc.
            cycleSeq = logType == FINALIZE ? 0 : GcLog::CurrentSeq();
            // ZStatPhase kind (pause vs concurrent, zStat.hpp:257/270) is sampled at scope
            // entry: a phase that straddles the world-release is booked where its work began.
            if (zstatActive) {
                zstatPauseAtStart = ZStat::WorldStoppedNow();
            }
            if (gcLogActive) {
                parent = CurrentLeafTimer();
                if (parent != nullptr) {
                    parent->hasChild = true;
                }
                CurrentLeafTimer() = this;
            }
        }
    }

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    ~Timer()
    {
        if (!active) {
            return;
        }
        if (gcLogActive) {
            CHECK(CurrentLeafTimer() == this);
            CurrentLeafTimer() = parent;
        }
        uint64_t stopTimeNs = TimeUtil::NanoSeconds();
        uint64_t diffTimeNs = stopTimeNs - startTimeNs;
        if (ENABLE_LOG(logType)) {
            WriteLog(true, logType, "%s time: %sus", name.Str(), Pretty(diffTimeNs / 1000).Str());
        }
        char path[GcLog::MAX_PHASE_PATH + 1] = "_";
        uint64_t depth = 1;
        bool pathOk = true;
        const bool isLeaf = gcLogActive && !hasChild;
        if (isLeaf) {
            BuildLeafPath(path, sizeof(path), depth, pathOk);
        }
        EmitTimerRecords(cycleSeq, name.Str(), startTimeNs, diffTimeNs, isLeaf,
                         zstatActive ? (zstatPauseAtStart ? 1 : 0) : -1, depth, pathOk, path);
        // Keep ZStat as an adjacent independent downstream: cutting GCLOG emission must not cut
        // the positive-control account.
        if (zstatActive) {
            ZStat::NotePhase(name.Str(), zstatPauseAtStart, diffTimeNs);
        }
    }

private:
    static void FoldPathComponent(const char* text, char* out)
    {
        size_t i = 0;
        if (text != nullptr) {
            for (; i < GcLog::MAX_PHASE_NAME && text[i] != '\0'; ++i) {
                char c = text[i];
                bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                out[i] = keep ? c : '_';
            }
        }
        if (i == 0) {
            out[i++] = '_';
        }
        out[i] = '\0';
    }

    void BuildLeafPath(char* out, size_t cap, uint64_t& depth, bool& pathOk) const
    {
        size_t pos = 0;
        depth = 0;
        pathOk = true;
        for (const Timer* timer = this; timer != nullptr; timer = timer->parent) {
            char component[GcLog::MAX_PHASE_NAME + 1];
            FoldPathComponent(timer->name.Str(), component);
            size_t componentLen = std::strlen(component);
            size_t separatorLen = pos == 0 ? 0 : 1;
            if (pos + separatorLen + componentLen >= cap) {
                pathOk = false;
                break;
            }
            if (separatorLen != 0) {
                out[pos++] = '>';
            }
            std::memcpy(out + pos, component, componentLen);
            pos += componentLen;
            ++depth;
        }
        out[pos] = '\0';
    }

    static Timer*& CurrentLeafTimer()
    {
        static thread_local Timer* current = nullptr;
        return current;
    }

    CString name;
    uint64_t startTimeNs = 0;
    uint64_t cycleSeq = 0;
    LogType logType;
    bool active = false;
    bool gcLogActive = false;
    bool zstatActive = false;
    bool zstatPauseAtStart = false;
    bool hasChild = false;
    Timer* parent = nullptr;
};
} // namespace MapleRuntime
#endif // MRT_LOG_FILE_H
