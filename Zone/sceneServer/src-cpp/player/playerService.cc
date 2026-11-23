#include "player/playerService.h"

#include <algorithm>
#include <cstdint>

PlayerService::PlayerService(PlayerManager& players, AoiService& aoi, const Map& map)
    : players_(players), aoi_(aoi), map_(map) 
{}

void PlayerService::configureScatterSpawn(bool enabled, uint32_t spacing)
{
    spawnScatter_ = enabled;
    if (spacing != 0)
        spawnSpacing_ = spacing;
}

void PlayerService::resolveSpawn(uint64_t pid, int32_t& x, int32_t& y) const
{
    if (!spawnScatter_)
        return;
    // 只处理"恰在地图原点"的新角色；有存档的玩家位置非 (0,0)，不会被改写
    if (x != static_cast<int32_t>(map_.getMinX()) || y != static_cast<int32_t>(map_.getMinY()))
        return;

    const auto [length, width] = map_.getSize();
    const uint32_t spacing = spawnSpacing_ == 0 ? 100u : spawnSpacing_;
    const uint32_t cols = std::max<uint32_t>(1u, length / spacing);
    const uint32_t rows = std::max<uint32_t>(1u, width / spacing);

    // playerId 的确定性强散列（splitmix64 风格的混合），保证同一角色每次落在同一格
    uint64_t h = pid * 0x9E3779B97F4A7C15ull;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;

    const uint32_t slot = static_cast<uint32_t>(h % (static_cast<uint64_t>(cols) * rows));
    x = static_cast<int32_t>(map_.getMinX() + (slot % cols) * spacing + spacing / 2);
    y = static_cast<int32_t>(map_.getMinY() + (slot / cols) * spacing + spacing / 2);
}

PlayerService::Result PlayerService::EnterScene(uint64_t pid, int32_t x, int32_t y, int32_t dir)
{
    auto* p = players_.get(pid);
    if(!p) 
        return Result::kNotFound;

    resolveSpawn(pid, x, y);
    if(!map_.contains(x,y)) 
        return Result::kOutOfBounds;

    const auto id = aoi_.EntityOf(pid);
    if(id && aoi_.IsEntityActive(*id)) 
        return Result::kAlreadyInScene;

    const auto oldX = p->x(), oldY = p->y(), oldDir = p->dir();
    const bool changed = oldX != x || oldY != y || oldDir != dir;
    if(changed) 
        p->ensureRevisionAvailable();

    p->x_ = x; p->y_ = y; p->dir_ = dir;
    const auto result = aoi_.OnEntityEnter(*p);
    if(result != AoiService::ApplyResult::kApplied) 
    {
        p->x_ = oldX; p->y_ = oldY; p->dir_ = oldDir;
        return Result::kAoiRejected;
    }

    if(changed) 
        p->recordMutation();

    const auto& binding=p->clientBinding();
    if(GatewayPlayer::Valid(binding))
        aoi_.OnPlayerOnline(pid,binding.gatewayId,binding.clientEpoch);
    return Result::kOk;
}

PlayerService::Result PlayerService::Move(uint64_t pid, int32_t x, int32_t y, int32_t dir)
{
    auto* p = players_.get(pid);
    if(!p) 
        return Result::kNotFound;
    if(!map_.contains(x,y)) 
        return Result::kOutOfBounds;

    const auto oldX = p->x(), oldY = p->y(), oldDir = p->dir();
    const bool changed = oldX != x || oldY != y || oldDir != dir;
    if(changed) 
        p->ensureRevisionAvailable();

    p->x_ = x; p->y_ = y; p->dir_ = dir;
    const auto result = aoi_.OnLocalEntityMoved(*p);
    if(result == AoiService::ApplyResult::kApplied || result == AoiService::ApplyResult::kIgnored) 
    {
        if(changed) p->recordMutation();
        return Result::kOk;
    }
    p->x_ = oldX; p->y_ = oldY; p->dir_ = oldDir;
    return result == AoiService::ApplyResult::kNeedSnapshot ? Result::kNotInScene : Result::kAoiRejected;
}

PlayerService::Result PlayerService::LeaveScene(uint64_t pid)
{
    if(!players_.get(pid)) 
        return Result::kNotFound;
    
    const auto id = aoi_.EntityOf(pid);
    if(!id) 
        return Result::kOk;
    
    const auto result = aoi_.OnEntityLeave(*id);
    return result == AoiService::ApplyResult::kApplied || result == AoiService::ApplyResult::kIgnored
        ? Result::kOk : Result::kAoiRejected;
}