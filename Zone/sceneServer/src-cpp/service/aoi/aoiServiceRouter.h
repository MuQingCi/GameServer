#ifndef CLEARMOON_SCENE_AOI_SERVICE_ROUTER_H
#define CLEARMOON_SCENE_AOI_SERVICE_ROUTER_H

#include "service/aoi/aoiService.h"
#include "service/aoi/aoiWire.h"

namespace AoiServiceRouter {
enum class Result { kHandled, kRejected, kSnapshotRequested };

// 仅由内部 Service 分支调用；网络消息不能通过此路由成为远程 AOI 事件。
inline Result Dispatch(AoiService& service, workerId self, size_t workerCount, const Msg& msg)
{
    const auto sender = msg.head.serviceSenderWorkerId;
    if(msg.head.msgType != MSGTYPE_SERVICE || msg.head.Module != Module::AOI ||
       sender >= workerCount || sender == self)
        return Result::kRejected;

    // 进程内投递可能走共享载荷（不拷贝 body），两者取其一
    const std::string& body = msg.sharedBody ? *msg.sharedBody : msg.body;

    switch(msg.head.Method) 
    {
    case Method::AOI_ENTRY:
    case Method::AOI_MOVE:
    case Method::AOI_LEAVE: 
    {
        AoiEvent event;
        if(!AoiWire::DecodeEvent(body, event) || event.ownerThreadId != sender ||
           AoiWire::EventMethod(event.msgKind) != msg.head.Method)
            return Result::kRejected;
        return service.onRemoteEntityEvent(sender, event) == AoiService::RemoteResult::kRejected
            ? Result::kRejected : Result::kHandled;
    }
    case Method::AOI_EVENT_BATCH:
    {
        std::vector<AoiEvent> events;
        if(!AoiWire::DecodeBatch(body, events))
            return Result::kRejected;
        bool rejected = false;
        for(const auto& event : events)
        {
            // 信封一致性：任一事件声明错误则整帧拒绝，不做部分应用
            if(event.ownerThreadId != sender || AoiWire::EventMethod(event.msgKind) == 0)
                return Result::kRejected;
            if(service.onRemoteEntityEvent(sender, event) == AoiService::RemoteResult::kRejected)
                rejected = true;
        }
        return rejected ? Result::kRejected : Result::kHandled;
    }
    case Method::AOI_OWNER_SNAPSHOT: 
    {
        AoiOwnerSnapshot snapshot{};
        if(!AoiWire::DecodeSnapshot(body, snapshot) || snapshot.ownerWokerId != sender)
            return Result::kRejected;
        const auto result = service.ApplyOwnerSnapshot(sender, snapshot);
        return result == AoiService::ApplyResult::kApplied || result == AoiService::ApplyResult::kIgnored
            ? Result::kHandled : Result::kRejected;
    }
    case Method::AOI_OWNER_SNAPSHOT_REQ:
        return AoiWire::ValidSnapshotRequest(body) ? Result::kSnapshotRequested : Result::kRejected;
    default:
        return Result::kRejected;
    }
}

inline AoiServiceHooks PublishHooks(workerId self, size_t workerCount)
{
    AoiServiceHooks hooks;
    for(size_t id = 0; id < workerCount; ++id)
        if(id != self) 
            hooks.publishTargets.push_back(static_cast<workerId>(id));

    hooks.encodeEvent = [](const AoiEvent& event, AoiWorkerPublishRequest& request)
    {
        request.msgType = MSGTYPE_SERVICE;
        request.module = Module::AOI;
        request.method = AoiWire::EventMethod(event.msgKind);
        return request.method != 0 && AoiWire::EncodeEvent(event, request.body);
    };
    return hooks;
}
}
#endif