// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_SAMPLES_RECORD_H
#define MRT_SAMPLES_RECORD_H

#include <vector>
#include <map>
#include <set>
#include <atomic>
#include <list>
#include <mutex>
#include "Common/StackType.h"

namespace MapleRuntime {
constexpr int MAX_NODE_COUNT = 20000;   // 20000:the maximum size of the array
constexpr int UNKNOWN_NODE_ID = 0;      // 0: the (root) node parent id
constexpr int ROOT_NODE_ID = 1;         // 1: the (root) node id
constexpr int PROGRAM_NODE_ID = 2;      // 2: the (program) node id
constexpr int IDLE_NODE_ID = 3;         // 3: the (idel) node id

struct CodeInfo {
    TraceFunctionIdentity functionIdentity;
    uint64_t scriptId = 0;
    uint32_t lineNumber = 0;
    FrameType frameType = FrameType::UNKNOWN;
    CString functionName = "";
    CString url = "";

    bool operator < (const CodeInfo& codeInfo) const
    {
        return std::tie(frameType, functionIdentity, functionName, lineNumber) <
               std::tie(codeInfo.frameType, codeInfo.functionIdentity, codeInfo.functionName, codeInfo.lineNumber);
    }
};

struct CpuProfileNode {
    uint64_t id = 0;
    uint64_t parentId = 0;
    uint64_t hitCount = 0;
    struct CodeInfo codeEntry;
    std::vector<uint64_t> children;

    bool operator < (const CpuProfileNode& node) const
    {
        return parentId < node.parentId || (parentId == node.parentId && codeEntry < node.codeEntry);
    }
};

struct ProfileInfo {
    uint64_t mutatorId = 0;
    uint64_t startTime = 0;
    uint64_t stopTime = 0;
    uint64_t nodeCount = 0;

    CpuProfileNode nodes[MAX_NODE_COUNT];
    std::vector<int> samples;
    std::vector<int> timeDeltas;

    std::set<CpuProfileNode> nodeSet;
    uint64_t previousTimeStamp = 0;
    uint64_t previousId = 0;
    FrameType previousState = FrameType::UNKNOWN;

    // state time statistic
    uint64_t managedTime = 0;
    uint64_t unknownTime = 0;
};

class SampleTask {
public:
    explicit SampleTask(uint64_t time, uint64_t id, const std::vector<RawTraceFrame>& captured)
        : timeStamp(time), mutatorId(id), frames(captured) {}

    uint64_t timeStamp;
    uint64_t mutatorId;
    std::vector<RawTraceFrame> frames;
    bool finishParsed {false};
    int checkPoint {0};
};

class SamplesRecord {
public:
    SamplesRecord() {}
    ~SamplesRecord();
    void InitProfileInfo();
    void ReleaseProfileInfo();
    void SetIsStart(bool started) { isStart.store(started); }
    bool GetIsStart() const { return isStart.load(); }
    void SetThreadStartTime(uint64_t threadStartTime) { profileInfo->startTime = threadStartTime; }
    void SetSampleStopTime(uint64_t threadStopTime) { profileInfo->stopTime = threadStopTime; }
    void AddSample(SampleTask& task);
    void AddEmptySample(SampleTask& task);
    void StringifySampleData(ProfileInfo* info);
    void DumpProfileInfo();
    void RunTaskLoop();
    void DoSingleTask(uint64_t previousTimeStemp);
    void ParseSampleData(uint64_t previousTimeStemp);
    void Post(uint64_t mutatorId, const std::vector<RawTraceFrame>& frames);
    std::vector<CodeInfo> BuildCodeInfos(SampleTask* task);
    int GetSamplingInterval() { return interval; }
    bool OpenFile(int fd);
    static uint64_t GetMicrosecondsTimeStamp();

private:
    void StatisticStateTime(ProfileInfo* info, int timeDelta);
    void NodeInit();
    void StringifyStateTimeStatistic(ProfileInfo* info);
    void StringifyNodes(ProfileInfo* info);
    void StringifySamples(ProfileInfo* info);
    uint64_t UpdateScriptIdMap(CString& url);
    bool UpdateNodeSet(ProfileInfo* info, CpuProfileNode& methodNode);
    void AddNodes(ProfileInfo* info, CpuProfileNode& methodNode);
    int GetSampleNodeId(uint64_t previousId, uint64_t topFrameNodeId);
    uint64_t GetPreviousTimeStamp(uint64_t previousTimeStamp, uint64_t startTime);
    void DeleteAbnormalSample(ProfileInfo* info, int timeDelta);
    void SetPreviousState(ProfileInfo* info, FrameType frameType);
    void IncreaseNodeHitCount(ProfileInfo* info, int sampleNodeId);
    void AddSampleNodeId(ProfileInfo* info, int sampleNodeId);
    void AddTimeDelta(ProfileInfo* info, int timeDelta);
    void SetPreviousTimeStamp(ProfileInfo* info, uint64_t timeStamp);
    std::vector<CodeInfo> GetCodeInfos(SampleTask& task);
    CString GetUrl(const RawTraceFrame& frame);
    CString ParseUrl(const RawTraceFrame& frame);
    CString GetDemangleName(const RawTraceFrame& frame);
    CString ParseDemangleName(const RawTraceFrame& frame);
    void WriteFile();
    bool IsTimeout(uint64_t previousTimeStemp);
    ProfileInfo* GetProfileInfo(uint64_t mutatorId);

    int fileDesc{-1};
    uint32_t timeDeltaThreshold {2000}; // 2000 : default timeDeltaThreshold 2000us
    std::atomic_bool isStart {false};
    CString sampleData {""};
    std::mutex sampleMonitor;
    std::list<SampleTask> taskQueue;
    ProfileInfo* profileInfo {nullptr};
    std::map<CString, uint64_t> scriptIdMap {{"", 0}}; // {filePath, id}: each filePath has a unique id.
    int interval {500}; // 500 : default interval 500us
    std::map<TraceFunctionIdentity, CString> identifierFuncnameMap;
    std::map<TraceFunctionIdentity, CString> identifierUrlMap;
};
} // namespace MapleRuntime
#endif // MRT_SAMPLES_RECORD_H