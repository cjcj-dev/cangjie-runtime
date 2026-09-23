// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zDriverPort.hpp"
#include "Inspector/FileStream.h"
#include "Inspector/CjAllocData.h"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Inspector/ProfilerAgentImpl.h"
namespace MapleRuntime {
int EnableAllocRecord(bool enable)
{
    MapleRuntime::CjAllocData::GetCjAllocData()->SetRecording(enable);
    if (enable) {
        MapleRuntime::CjAllocData::SetCjAllocData();
    } else {
        MapleRuntime::CjAllocData::GetCjAllocData()->DeleteCjAllocData();
    }
    return 0;
}
std::string ParseDomain(const std::string &message)
{
    std::string key = "\"id\":";
    size_t startPos = message.find(key);
    if (startPos == std::string::npos) {
        return "";
    }
    startPos += key.length();

    while (startPos < message.length() && std::isspace(message[startPos])) {
        ++startPos;
    }

    if (message[startPos] == '"') {
        size_t endPos = message.find('"', startPos + 1);
        if (endPos == std::string::npos) {
            return "";
        }
        return message.substr(startPos + 1, endPos - startPos - 1);
    } else {
        size_t endPos = message.find(',', startPos);
        if (endPos == std::string::npos) {
            endPos = message.find('}', startPos);
        }
        if (endPos == std::string::npos) {
            return "";
        }
        return message.substr(startPos, endPos - startPos);
    }
}

void SetEnd(const std::string &message, MapleRuntime::MsgType type)
{
    MapleRuntime::HeapProfilerStream* stream = &MapleRuntime::HeapProfilerStream::GetInstance();
    stream->SetContext(type);
    MapleRuntime::StreamWriter* writer = new MapleRuntime::StreamWriter(stream);
    writer->WriteString("{\"id\":");
    writer->WriteString(MapleRuntime::HeapProfilerStream::GetInstance().GetMessageID());
    writer->WriteString(",\"result\":{}");
    writer->End();
    delete writer;
}

void DumpHeapSnapshot(SendMsgCB sendMsg)
{
    MapleRuntime::Heap::GetHeap().DumpHeap(
        MapleRuntime::HeapDumpKind::IDE);
}

void StartTrackingHeapObjects(const std::string &message, SendMsgCB sendMsg)
{
    SetEnd(message, MapleRuntime::MsgType::END);
    EnableAllocRecord(true);
}

void StopTrackingHeapObjects(const std::string &message, SendMsgCB sendMsg)
{
    EnableAllocRecord(false);
    SetEnd(message, MapleRuntime::MsgType::END);
}

void CollectGarbage(const std::string &message, SendMsgCB sendMsg)
{
    MapleRuntime::Heap::GetHeap().RequestGC(MapleRuntime::GC_REASON_DCMD_GC_RUN, false);
    SetEnd(message, MapleRuntime::MsgType::END);
}

void DisableCollect(const std::string &message, SendMsgCB sendMsg)
{
    SetEnd(message, MapleRuntime::MsgType::DISABLE);
}

void GetHeapUsage(const std::string &message, SendMsgCB sendMsg)
{
    // get HeapStats stream and process requests concurrently.
    MapleRuntime::HeapProfilerStream* stream = &MapleRuntime::HeapProfilerStream::GetHeapStatsInstance();
    stream->SetHandler(sendMsg);
    stream->SetMessageID(message);
    stream->SetContext(MapleRuntime::MsgType::HEAPUSAGE);
    MapleRuntime::StreamWriter* writer = new MapleRuntime::StreamWriter(stream);
    writer->WriteString("{\"id\":");
    writer->WriteString(stream->GetMessageID());
    const auto usage = Heap::GetHeap().GetMemoryUsage();
    const auto writeSize = [writer](size_t value) {
        writer->WriteString(CString(static_cast<uint64_t>(value)));
    };
    const auto writePool = [writer, &writeSize](const ZMemoryUsage& pool) {
        writer->WriteString("{\"usedSize\":");
        writeSize(pool.used);
        writer->WriteString(",\"currentSize\":");
        writeSize(pool.current);
        writer->WriteString(",\"maxSize\":");
        writeSize(pool.max);
        writer->WriteString("}");
    };
    writer->WriteString(",\"result\":{\"usedSize\":");
    writeSize(usage.young.used + usage.old.used);
    writer->WriteString(",\"currentSize\":");
    writeSize(usage.young.current + usage.old.current);
    writer->WriteString(",\"maxSize\":");
    writeSize(usage.young.max);
    writer->WriteString(",\"young\":");
    writePool(usage.young);
    writer->WriteString(",\"old\":");
    writePool(usage.old);
    // HeapProfilerStream appends the profiler field and closes the envelope.
    writer->WriteString("}");
    writer->End();
    delete writer;
}

void ProfilerAgentImpl(const std::string &message, SendMsgCB sendMsg)
{
    if (message.find("getHeapUsage", 0) != std::string::npos) {
        GetHeapUsage(message, sendMsg);
        return;
    }
    MapleRuntime::HeapProfilerStream::GetInstance().SetHandler(sendMsg);
    MapleRuntime::HeapProfilerStream::GetInstance().SetMessageID(message);
    if (message.find("takeHeapSnapshot", 0) != std::string::npos) {
        DumpHeapSnapshot(sendMsg);
    } else if (message.find("startTrackingHeapObjects", 0) != std::string::npos) {
        StartTrackingHeapObjects(message, sendMsg);
    } else if (message.find("stopTrackingHeapObjects", 0) != std::string::npos) {
        StopTrackingHeapObjects(message, sendMsg);
    } else if (message.find("disable", 0) != std::string::npos) {
        DisableCollect(message, sendMsg);
    } else if (message.find("collectGarbage", 0) != std::string::npos) {
        CollectGarbage(message, sendMsg);
    } else {
        LOG(RTLOG_ERROR, "invaild request\n");
    }
}
}
