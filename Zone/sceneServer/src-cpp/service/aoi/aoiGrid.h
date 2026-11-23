#ifndef CLEARMOON_ZONE_SCENESERVER_SERVICE_AOIGRID_H
#define CLEARMOON_ZONE_SCENESERVER_SERVICE_AOIGRID_H

#include "map.h"
#include "service/aoi/aoiTypes.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>


struct AoiCell
{
    std::vector<EntityId> entys_;  //本格内的实例
    std::vector<EntityId> watchers_;
    // 位置索引：id -> 在对应 vector 中的下标，使删除为 O(1) swap-pop。
    // 默认构造的 unordered_map 不分配桶，只有真正有成员时才占内存。
    std::unordered_map<EntityId, uint32_t> entPos_;
    std::unordered_map<EntityId, uint32_t> watcherPos_;
    // 本格"实体成员集合"的版本号：只要有实体进入/离开本格就递增。
    // 观察者据此判断自己的可见成员集合是否变化，从而跳过无谓的全量重建。
    uint32_t membersRev_ = 0;
};

//AOI区域
class AoiGrid
{
public:                                           
    AoiGrid(uint32_t minX,
            uint32_t minY,
            uint32_t cols,
            uint32_t rows,
            uint32_t cellSize,
            uint32_t viewRadiusCells);

    // 坐标 -> 格子索引；越界返回 nullopt
    std::optional<uint32_t> TryCellOf(Position x, Position y) const;
    
     // 取格子，调用方保证索引合法（和 vector::operator[] 一致）
    const AoiCell& getCell(uint32_t cellIdx) const;

    // 以 (x, y) 为中心、半径 r_ 个格子覆盖的所有格子索引
    std::vector<uint32_t> getWatchCellIdx(Position x, Position y) const;

    // 两个格子是否互相可见（切比雪夫距离 <= r_）
    bool visible(uint32_t cellA, uint32_t cellB) const;

    void addEntity(EntityId entityId, uint32_t cell);
    void removeEntity(EntityId entityId, uint32_t cell);

    void addWatcher(EntityId observerId, uint32_t cell);
    void removeWatcher(EntityId observerId, uint32_t cell);

    const std::vector<EntityId>& entities(uint32_t cell) const;
    const std::vector<EntityId>& watchers(uint32_t cell) const;

    // 本格实体成员版本；越界返回 0
    uint32_t membersRev(uint32_t cell) const;
    // 以给定格子为中心、半径 r_ 的视野格索引；越界返回空
    std::vector<uint32_t> watchCellsOfCell(uint32_t cellIdx) const;

    // 以格子索引为中心的视野遍历：不分配内存，fn(cellIdx, slot)，slot 在 [0,9) 内稳定编号。
    template <typename Fn>
    void forEachWatchCellOfCell(uint32_t cellIdx, Fn&& fn) const
    {
        if (cellSize_ == 0 || cols_ == 0 || rows_ == 0) return;
        if (cellIdx >= static_cast<uint32_t>(cols_) * static_cast<uint32_t>(rows_)) return;

        const uint32_t centerCol = cellIdx % cols_;
        const uint32_t centerRow = cellIdx / cols_;
        const int32_t r     = static_cast<int32_t>(r_);
        const int32_t colsI = static_cast<int32_t>(cols_);
        const int32_t rowsI = static_cast<int32_t>(rows_);

        const int32_t col0 = std::max(0, static_cast<int32_t>(centerCol) - r);
        const int32_t col1 = std::min(colsI - 1, static_cast<int32_t>(centerCol) + r);
        const int32_t row0 = std::max(0, static_cast<int32_t>(centerRow) - r);
        const int32_t row1 = std::min(rowsI - 1, static_cast<int32_t>(centerRow) + r);

        uint32_t slot = 0;
        for (int32_t row = row0; row <= row1; ++row)
            for (int32_t col = col0; col <= col1; ++col)
                fn(static_cast<uint32_t>(row) * cols_ + static_cast<uint32_t>(col), slot++);
    }

    // 以坐标为中心的视野遍历（同上，不分配）
    template <typename Fn>
    void forEachWatchCell(Position x, Position y, Fn&& fn) const
    {
        const auto optIdx = TryCellOf(x, y);
        if (!optIdx) return;
        forEachWatchCellOfCell(*optIdx, std::forward<Fn>(fn));
    }

    uint32_t viewRadiusCells() const { return r_; }

private:
    Position minX_ = 0;
    Position minY_ = 0;
    uint32_t cols_ = 0;
    uint32_t rows_ = 0;
    uint32_t cellSize_ = 0;
    uint32_t r_ = 0;

    std::vector<AoiCell> cells_;
};

#endif