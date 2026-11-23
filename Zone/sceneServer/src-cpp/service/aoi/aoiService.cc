#include "service/aoi/aoiService.h"
#include "service/aoi/aoiWire.h"
#include "player/player.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
AoiEvent FromEntity(const AoiEntity& ent, AoiMsgKind kind)
{
    AoiEvent ev;
    ev.msgKind = kind;
    ev.entityKind = ent.kind;
    ev.eId = ent.entityId;
    ev.businessId = ent.businessId;
    ev.target_x = ent.x;
    ev.target_y = ent.y;
    ev.target_dir = ent.dir;
    ev.entityGeneration = ent.entityGeneration;
    ev.actorEpoch = ent.actorEpoch;
    ev.spatialVersion = ent.spatialVersion;
    ev.ownerThreadId = ent.ownerWorker;
    ev.ownerSeq = ent.ownerSeq;
    return ev;
}
}

AoiService::AoiService(workerId id, const AoiConfig& cfg, 
                       AoiPushSink& sink,
                       const Map& map, 
                       AoiServiceHooks hooks)
                    :  wId_(id), 
                       aoiConfig_(cfg), 
                       sink_(sink), 
                       map_(map), 
                       hooks_(std::move(hooks)),
                       replica_(id, aoiConfig_, map)
{
    //确保两个配置都合法
    std::string err;
    if(!cfg.Validate(err) || map.getCellSize() != cfg.cellSize || map.getCols() == 0)
        throw std::invalid_argument("invalid AOI configuration or map: " + err);

    //将所有逻辑线程ID排序并将重复的移出数据，然后将本线程id从数组中移出
    auto& targets = hooks_.publishTargets;
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    targets.erase(std::remove(targets.begin(), targets.end(), wId_), targets.end());
}

bool AoiService::IsEntityActive(EntityId id) const
{
    return replica_.findById(id) != nullptr;
}

AoiService::ApplyResult AoiService::OnEntityEnter(Player& p) 
{ 
    return Enter(p, false); 
}

AoiService::ApplyResult AoiService::OnEntityReenter(Player& p) 
{ 
    return Enter(p, true); 
}


AoiService::ApplyResult AoiService::OnEntityLeave(EntityId id)
{
    //1.校验
    //校验实例是否存在
    const auto* ent = replica_.findById(id);
    if(!ent) 
        return ApplyResult::kIgnored;

    //校验是否来自本线程
    if(ent->ownerWorker != wId_) 
        return ApplyResult::kBadSource;

    //校验空间版本是否合法
    if(ent->spatialVersion == std::numeric_limits<uint64_t>::max()) 
        return ApplyResult::kInvalid;

    //2.依据实例信息生成AoiEvent事件并递增空间版本号
    auto ev = FromEntity(*ent, AoiMsgKind::kLeave);
    ++ev.spatialVersion;

    //3.获取应用本地事件的结果并根据结果执行相应处理
    const auto result = ApplyLocal(ev);
    if(result == ApplyResult::kApplied) 
    {
        const auto* observer = viewSet_.findObserver(id);
        if(observer) 
            viewSet_.removeObserver(id, observer->clientViewEpoch);
        dirtyObservers_.erase(id);
    }
    return result;
}

//此时的player是已经修改后的状态
AoiService::ApplyResult AoiService::OnLocalEntityMoved(Player& p)
{
    //1.校验
    const auto id = EntityOf(p.id());
    const auto* ent = id ? replica_.findById(*id) : nullptr;
    //校验实例
    if(!ent) 
        return ApplyResult::kNeedSnapshot;
    //校验目标位置
    if(!ValidPosition(p)) 
        return ApplyResult::kOutOfBounds;
    //比较移动前后实例信息
    if(ent->x == uint32_t(p.x()) && ent->y == uint32_t(p.y()) && ent->dir == p.dir())
        return ApplyResult::kIgnored;
    //校验空间版本号
    if(ent->spatialVersion == std::numeric_limits<uint64_t>::max()) 
        return ApplyResult::kInvalid;

    //2.依据实例信息生成AoiEvent并递增空间版本号
    auto ev = FromEntity(*ent, AoiMsgKind::kMove);
    ev.target_x = static_cast<Position>(p.x());
    ev.target_y = static_cast<Position>(p.y());
    ev.target_dir = p.dir();
    ev.actorEpoch = p.epoch();
    ++ev.spatialVersion;

    return ApplyLocal(ev);
}

AoiService::RemoteResult AoiService::onRemoteEntityEvent(workerId sender, const AoiEvent& ev)
{
    //1.拒绝本线程消息和非法消息
    if(sender == wId_ || sender != ev.ownerThreadId) 
        return RemoteResult::kRejected;

    //2.若远端消息为移动消息时
    if(ev.msgKind == AoiMsgKind::kMove) 
    {
        //校验消息数据是否合法
        const auto size = map_.getSize();
        if(ev.eId == 0 || ev.target_x >= size.first || ev.target_y >= size.second ||
           (ev.entityKind != EntityKind::kPlayer && ev.entityKind != EntityKind::kMonster &&
            ev.entityKind != EntityKind::kNpc && ev.entityKind != EntityKind::kDrop))
            return RemoteResult::kRejected;
        
        //判断是否存在实例ID相同的移动消息
        auto it = pendingRemoteMoves_.find(ev.eId);
        if(it != pendingRemoteMoves_.end()) 
        {
            const auto& old = it->second;
            //新旧消息所属线程不同则直接视为非法消息
            if(old.ownerThreadId != sender) 
                return RemoteResult::kRejected;

            //新消息为旧世代/旧空间版本消息则直接忽略
            if(ev.entityGeneration < old.entityGeneration ||
               (ev.entityGeneration == old.entityGeneration && ev.spatialVersion <= old.spatialVersion))
                return RemoteResult::kIgnored;
        }
        pendingRemoteMoves_[ev.eId] = ev;
        return RemoteResult::kQueued;
    }
    
    //3.远端消息非Move则直接应用，根据结果执行相应处理
    const auto result = replica_.Apply(ev, AoiReplica::Source::kRemote, dirtyObservers_, &dirtyEntities_);

    if(result == ApplyResult::kApplied) 
    {
        //该实例存在待处理Move事件，先校验二者信息，二者基本信息相等且新消息为新世代/新空间世代/Leave消息则直接清除对应待处理的Move
        auto it = pendingRemoteMoves_.find(ev.eId);
        if(it != pendingRemoteMoves_.end() && it->second.ownerThreadId == sender &&
           (it->second.entityGeneration < ev.entityGeneration ||
            (it->second.entityGeneration == ev.entityGeneration &&
             (ev.msgKind == AoiMsgKind::kLeave || it->second.spatialVersion <= ev.spatialVersion))))
            pendingRemoteMoves_.erase(it);
        return RemoteResult::kApplied;
    }
    return result == ApplyResult::kIgnored ? RemoteResult::kIgnored : RemoteResult::kRejected;
}


AoiService::ApplyResult AoiService::ApplyOwnerSnapshot(workerId sender, const AoiOwnerSnapshot& snap)
{
    //1.拒绝本线程消息和非法消息
    if(sender == wId_ || sender != snap.ownerWokerId) 
        return ApplyResult::kBadSource;

    //2.应用快照
    const auto result = replica_.ApplyOwnerSnapshot(snap, dirtyObservers_);
    if(result == ApplyResult::kApplied) 
    {
        // 快照批量替换实体，被移除者没有逐条事件：本 tick 强制全量重建
        forceFullRebuild_ = true;
        for(auto it = pendingRemoteMoves_.begin(); it != pendingRemoteMoves_.end();) 
        {
            if(it->second.ownerThreadId == sender && it->second.ownerSeq <= snap.seq)
                it = pendingRemoteMoves_.erase(it);
            else ++it;
        }
        ownersNeedingSnapshot_.erase(sender);
    }
    return result;
}

AoiOwnerSnapshot AoiService::BuildOwnerSnapshot() const
{
    AoiOwnerSnapshot snap{ownerSeq_, wId_, {}};
    replica_.ForEachOwned([&](const AoiEntity& ent) { snap.ents.push_back(ent); });
    return snap;
}


//推送
AoiPushResult AoiService::pushToClient(const AoiClientPushRequest& request)
{
    AoiDiagnostics::Scope timing(diagnostics_, AoiDiagnostics::Stage::ClientAdmission);
    const auto result = sink_.sendToClient(request);
    if (diagnostics_) 
    {
        diagnostics_->add(AoiDiagnostics::Counter::Bytes, request.body.size());
        diagnostics_->add(result == AoiPushResult::kAccepted ? AoiDiagnostics::Counter::Accepted
                                                          : AoiDiagnostics::Counter::Rejected);
    }
    return result;
}

AoiWorkerPublishResult AoiService::pushToWorkers(const AoiWorkerPublishRequest& request)
{
    return sink_.publishToWorkers(request);
}


bool AoiService::OnPlayerOnline(uint64_t pid, uint64_t gatewayId, uint32_t epoch)
{
    // 路由由会话组件/Sink 管理；这里仅管理观察者发送基线
    (void)gatewayId;
    const auto id = EntityOf(pid);
    if(!id || !replica_.findById(*id)) 
        return false;
    if(!viewSet_.registryObserver(*id, epoch)) 
        return false;
    dirtyObservers_.insert(*id);
    return true;
}

bool AoiService::OnPlayerRebind(uint64_t pid, uint32_t oldEpoch, uint32_t newEpoch)
{
    const auto id = EntityOf(pid);
    if(!id || !viewSet_.resetObserver(*id, oldEpoch, newEpoch)) 
        return false;
    dirtyObservers_.insert(*id);
    return true;
}

bool AoiService::OnPlayerOffline(uint64_t pid, uint32_t epoch, uint8_t reason)
{
    (void)reason;
    const auto id = EntityOf(pid);
    if(!id || !viewSet_.removeObserver(*id, epoch)) return false;
    dirtyObservers_.erase(*id);
    return true; // 会话离线不自动代表实体离开场景。
}


bool AoiService::OnViewResyncReq(uint64_t pid, uint32_t epoch)
{
    const auto id = EntityOf(pid);
    return id && OnClientSendFailed(*id, epoch);
}

bool AoiService::OnClientSendFailed(EntityId observer, uint32_t epoch)
{
    if(!viewSet_.markNeedResync(observer, epoch)) 
        return false;
    dirtyObservers_.insert(observer);
    return true;
}


void AoiService::OnTick(int64_t now)
{
    if(now < 0 || now < nextTickTime_) 
        return;
    nextTickTime_ = now > std::numeric_limits<int64_t>::max() - aoiConfig_.tickMs
        ? std::numeric_limits<int64_t>::max() : now + aoiConfig_.tickMs;
    ++tickCount_;
    FlushRemoteMoves();
    RetryPublishes();
    if (ProcessObservers())
    {
        // 全部观察者都已成功处理：本帧变化集与"强制全量重建"标记都可以清除
        dirtyEntities_.clear();
        forceFullRebuild_ = false;
        return;
    }

    // 存在推送失败的观察者：保留变化集，下个 tick 用并集重试，避免该实体的更新永远不会再被推送。
    // 若持续失败导致集合无界增长，则退回"全量重同步"兜底，而不是无限累积。
    constexpr size_t kDirtyEntityHardLimit = 65536;
    if (dirtyEntities_.size() > kDirtyEntityHardLimit)
    {
        for (auto observer : dirtyObservers_)
        {
            const auto* st = viewSet_.findObserver(observer);
            if (st != nullptr)
                viewSet_.markNeedResync(observer, st->clientViewEpoch);
        }
        dirtyEntities_.clear();
        forceFullRebuild_ = false;
    }
}


AoiViewSet& AoiService::views() 
{ 
    return viewSet_; 
}

bool AoiService::ViewOf(EntityId observer, std::vector<AoiEntity>& out) const
{
    out.clear();
    return replica_.getVisibleEntity(observer, out, false, diagnostics_);
}

AoiServiceStats AoiService::Stats() const
{
    return {pendingRemoteMoves_.size(), pendingPublishes_.size(), dirtyObservers_.size(),
            viewSet_.forEachObserver().size(), ownersNeedingSnapshot_.size(), tickCount_};
}

std::optional<EntityId> AoiService::EntityOf(uint64_t pid) const
{
    const auto it = identities_.find(pid);
    if(it == identities_.end()) 
        return std::nullopt;
    return it->second.id;
}

std::optional<uint64_t> AoiService::BusinessIdOf(EntityId id) const
{
    const auto* entity = replica_.findById(id);
    if (entity == nullptr || entity->businessId == 0)
        return std::nullopt;
    return entity->businessId;
}

std::vector<workerId> AoiService::OwnersNeedingSnapshot() const
{
    return {ownersNeedingSnapshot_.begin(), ownersNeedingSnapshot_.end()};
}

const AoiEntity* AoiService::FindEntity(EntityId id) const
{
    return replica_.findById(id);
}

bool AoiService::StructureUnchanged(EntityId observer) const
{
    const auto* st = viewSet_.findObserver(observer);
    if (st == nullptr || st->structCells.empty() ||
        st->structCells.size() != st->structRevs.size())
        return false;

    const auto* ent = replica_.findById(observer);
    // 观察者自身跨格 => 视野格集合变了，必须重建
    if (ent == nullptr || ent->cellIdx != st->structCenterCell)
        return false;

    for (size_t i = 0; i < st->structCells.size(); ++i)
    {
        if (replica_.cellMembersRev(st->structCells[i]) != st->structRevs[i])
            return false;
    }
    return true;
}

bool AoiService::ObserverCellChanged(EntityId observer) const
{
    const auto* st = viewSet_.findObserver(observer);
    if (st == nullptr || st->structCells.empty())
        return true;
    const auto* ent = replica_.findById(observer);
    if (ent == nullptr)
        return true;
    return ent->cellIdx != st->structCenterCell;
}

std::optional<uint32_t> AoiService::CellOf(EntityId observer) const
{
    const auto* ent = replica_.findById(observer);
    if (ent == nullptr)
        return std::nullopt;
    return ent->cellIdx;
}

bool AoiService::CellVisibleFrom(uint32_t observerCell, uint32_t cellIdx) const
{
    return replica_.cellVisible(observerCell, cellIdx);
}

void AoiService::RefreshStructureCache(EntityId observer)
{
    auto* st = viewSet_.findObserverMutable(observer);
    if (st == nullptr)
        return;

    const auto* ent = replica_.findById(observer);
    if (ent == nullptr)
    {
        st->structCenterCell = 0xFFFFFFFFu;
        st->structCells.clear();
        st->structRevs.clear();
        return;
    }

    st->structCenterCell = ent->cellIdx;
    st->structCells = replica_.watchCellsOfCell(ent->cellIdx);
    st->structRevs.clear();
    st->structRevs.reserve(st->structCells.size());
    for (uint32_t c : st->structCells)
        st->structRevs.push_back(replica_.cellMembersRev(c));
}


//————————————————private————————————————
AoiService::ApplyResult AoiService::Enter(Player& p, bool reenter)
{
    //1.校验
    if(p.id() == 0) 
        return ApplyResult::kInvalid;
    if(!ValidPosition(p)) 
        return ApplyResult::kOutOfBounds;

    auto it = identities_.find(p.id());
    //陌生玩家
    if(it == identities_.end()) 
    {
        if(nextEntityCounter_ == std::numeric_limits<uint32_t>::max()) 
            return ApplyResult::kInvalid;

        //注册
        LocalIdentity identity;
        identity.id = (uint64_t(wId_) << 32) | nextEntityCounter_++;
        it = identities_.emplace(p.id(), identity).first;
    }

    auto& identity = it->second;
    //非重进且存在玩家状态，直接忽略
    if(!reenter && replica_.findById(identity.id)) 
        return ApplyResult::kIgnored;
    //非法实例世代
    if(identity.generation == std::numeric_limits<uint32_t>::max()) 
        return ApplyResult::kInvalid;

    //2.生成AoiEvent并应用
    AoiEvent ev;
    ev.msgKind = AoiMsgKind::kEntry;
    ev.entityKind = EntityKind::kPlayer;
    ev.eId = identity.id;
    ev.businessId = p.id();
    ev.entityGeneration = identity.generation + 1;
    ev.actorEpoch = p.epoch();
    ev.target_x = static_cast<Position>(p.x());
    ev.target_y = static_cast<Position>(p.y());
    ev.target_dir = p.dir();
    ev.spatialVersion = 1;
    ev.ownerThreadId = wId_;
    const auto result = ApplyLocal(ev);

    if(result == ApplyResult::kApplied) 
        identity.generation = ev.entityGeneration;
    return result;
}

AoiService::ApplyResult AoiService::ApplyLocal(const AoiEvent& event)
{
    //1.校验
    if(ownerSeq_ == std::numeric_limits<uint64_t>::max()) 
        return ApplyResult::kInvalid;
    
    //生成副本并使副本seq+1并将其加入到待发布队列中
    AoiEvent ev = event;
    ev.ownerSeq = ownerSeq_ + 1;
    pendingPublishes_.push_back({ev, hooks_.publishTargets});

    const auto result = replica_.Apply(ev, AoiReplica::Source::kLocal, dirtyObservers_, &dirtyEntities_);
    //应用失败则回退到执行前的状态
    if(result != ApplyResult::kApplied) 
        pendingPublishes_.pop_back();
    else 
    {   //成功时更新Seq
        ownerSeq_ = ev.ownerSeq;
        if(hooks_.publishTargets.empty()) 
            pendingPublishes_.pop_back();
    }
    return result;
}

void AoiService::FlushRemoteMoves()
{
    auto pending = std::move(pendingRemoteMoves_);
    pendingRemoteMoves_.clear();
    for(const auto& kv : pending) 
    {
        const auto result = replica_.Apply(kv.second, AoiReplica::Source::kRemote, dirtyObservers_, &dirtyEntities_);
        if(result == ApplyResult::kNeedSnapshot) 
            ownersNeedingSnapshot_.insert(kv.second.ownerThreadId);
    }
}

void AoiService::RetryPublishes()
{
    AoiDiagnostics::Scope timing(diagnostics_, AoiDiagnostics::Stage::RemotePublish);
    // encodeEvent 在这里只作为"是否启用跨 worker 发布"的开关；
    // 实际编码改为批量帧：一个 tick 的全部事件只编码一次。
    if(!hooks_.encodeEvent || pendingPublishes_.empty())
        return;

    // 1) 收齐本 tick 需要发布的事件，一次性编码
    std::vector<AoiEvent> events;
    events.reserve(pendingPublishes_.size());
    for(const auto& task : pendingPublishes_)
        events.push_back(task.event);

    std::string batchBody;
    if(!AoiWire::EncodeBatch(events, batchBody))
        return;   // 编码失败：保留任务，下个 tick 重试

    // 2) 目标集合 = 各任务剩余目标的并集
    std::unordered_set<workerId> targetSet;
    for(const auto& task : pendingPublishes_)
        targetSet.insert(task.remaining.begin(), task.remaining.end());
    if(targetSet.empty())
    {
        pendingPublishes_.clear();
        return;
    }

    AoiWorkerPublishRequest request;
    request.msgType = MSGTYPE_SERVICE;
    request.module = Module::AOI;
    request.method = Method::AOI_EVENT_BATCH;
    // 按 hooks_.publishTargets 的固定顺序输出，保证跨 worker 的行为可复现
    request.targets.reserve(targetSet.size());
    for(workerId target : hooks_.publishTargets)
    {
        const auto it = targetSet.find(target);
        if(it == targetSet.end()) continue;
        request.targets.push_back(target);
        targetSet.erase(it);
    }
    // 兜底：若存在不在 publishTargets 中的剩余目标（理论上不会），追加在后面
    for(workerId target : targetSet)
        request.targets.push_back(target);

    // 关键：一次编码，多目标共享；不再逐目标拷贝 body
    request.sharedBody = std::make_shared<const std::string>(std::move(batchBody));

    const auto result = sink_.publishToWorkers(request);

    // 3) 记录失败目标，并据此收缩每个任务的 remaining
    std::unordered_set<workerId> failed;
    for(workerId target : request.targets)
    {
        const auto delivery = std::find_if(result.deliveries.begin(), result.deliveries.end(),
            [&](const WorkerDeliveryResult& r) { return r.target == target; });
        if(delivery == result.deliveries.end() || !delivery->accepted)
            failed.insert(target);
    }

    for(auto it = pendingPublishes_.begin(); it != pendingPublishes_.end();)
    {
        std::vector<workerId> keep;
        keep.reserve(it->remaining.size());
        for(workerId target : it->remaining)
            if(failed.count(target) != 0) keep.push_back(target);
        it->remaining.swap(keep);
        if(it->remaining.empty())
            it = pendingPublishes_.erase(it);
        else
            ++it;
    }
}

bool AoiService::ProcessObservers()
{
    if(!hooks_.processObserver) 
        return true;
    // 推送 LOD：本轮是否为"推送轮次"。间隔 1 = 每轮都推（默认，等价关闭）。
    const bool lodSkipRound = aoiConfig_.lodPushInterval > 1 &&
                              (tickCount_ % aoiConfig_.lodPushInterval) != 0;
    bool allProcessed = true;
    for(auto observer : viewSet_.forEachObserver()) 
    {
        if(!dirtyObservers_.contains(observer) && !viewSet_.hasPending(observer) &&
           !viewSet_.needResync(observer)) 
            continue;

        // 视野足够大且本轮不是推送轮次：先跳过。
        // 跳过时**不擦除脏标记**，下一轮由"基线差分"把累积变化一次带出，因此不会丢更新；
        // 代价是该观察者的 AOI 更新粒度变为 lodPushInterval × tickMs。
        // 重同步（新进场/客户端请求）永远不跳过，避免进场时黑屏。
        if (lodSkipRound && !viewSet_.needResync(observer))
        {
            const auto* st = viewSet_.findObserver(observer);
            if (st != nullptr && st->committedState_.size() >= aoiConfig_.lodMinViewSize)
            {
                allProcessed = false;   // 保留 dirtyEntities_，供下一轮差分使用
                continue;
            }
        }

        AoiDiagnostics::Scope timing(diagnostics_, AoiDiagnostics::Stage::ObserverPass);
        if (diagnostics_) diagnostics_->add(AoiDiagnostics::Counter::Observers);
        if(hooks_.processObserver(*this, observer)) 
            dirtyObservers_.erase(observer);
        else
            allProcessed = false;   // 保留 dirtyEntities_，下个 tick 用并集重试
    }
    return allProcessed;
}

bool AoiService::ValidPosition(const Player& p) const 
{ 
    return map_.contains(p.x(), p.y()); 
}