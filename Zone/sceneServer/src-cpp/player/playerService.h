#ifndef CLEARMOON_SCENE_PLAYER_SERVICE_H
#define CLEARMOON_SCENE_PLAYER_SERVICE_H

#include "player/playerManager.h"
#include "service/aoi/aoiService.h"

// 所有操作必须在 owner worker 执行。业务决定何时进场；Actor 创建不是进场。
class PlayerService
{
public:
    enum class Result { kOk, kNotFound, kNotInScene, kAlreadyInScene, kOutOfBounds, kAoiRejected };
    
    PlayerService(PlayerManager& players, AoiService& aoi, const Map& map);

    // 分散出生点（默认关闭）：开启后，位置恰为地图原点的新角色按 playerId 确定性分散，
    // 避免压测/新服开荒时全员落在同一格导致 AOI 密度 V=N。
    void configureScatterSpawn(bool enabled, uint32_t spacing);

    Result EnterScene(uint64_t pid, int32_t x, int32_t y, int32_t dir);
    Result Move(uint64_t pid, int32_t x, int32_t y, int32_t dir);
    Result LeaveScene(uint64_t pid);

private:
    // 解析实际出生位置：仅在开启分散且入参为地图原点时改写
    void resolveSpawn(uint64_t pid, int32_t& x, int32_t& y) const;

    PlayerManager& players_;
    AoiService& aoi_;
    const Map& map_;
    bool spawnScatter_ = false;
    uint32_t spawnSpacing_ = 100;
};
#endif
