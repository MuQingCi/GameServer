#include "service/aoi/aoiGrid.h"
#include <cstdint>
#include <optional>
#include <vector>
#include <algorithm>

AoiGrid::AoiGrid(uint32_t minX,
                 uint32_t minY,
                 uint32_t cols,
                 uint32_t rows,
                 uint32_t cellSize,
                 uint32_t viewRadiusCells)
                : minX_(minX), 
                  minY_(minY), 
                  cols_(cols), 
                  rows_(rows), 
                  cellSize_(cellSize), 
                  r_(viewRadiusCells)
{
    // 分配所有格子
    if (cols_ > 0 && rows_ > 0)
        cells_.resize(static_cast<size_t>(cols_) * static_cast<size_t>(rows_));
}

std::optional<uint32_t> AoiGrid::TryCellOf(Position x, Position y) const
{
    if (cellSize_ == 0 || cols_ == 0 || rows_ == 0)
        return std::nullopt;

    // 用 int64 计算边界，避免溢出
    const int64_t x64    = static_cast<int64_t>(x);
    const int64_t y64    = static_cast<int64_t>(y);
    const int64_t minX64 = static_cast<int64_t>(minX_);
    const int64_t minY64 = static_cast<int64_t>(minY_);
    const int64_t spanX  = static_cast<int64_t>(cols_) * static_cast<int64_t>(cellSize_);
    const int64_t spanY  = static_cast<int64_t>(rows_) * static_cast<int64_t>(cellSize_);

    // 区间是 [min, min + span)，所以上界用 >=
    if (x64 < minX64 || x64 >= minX64 + spanX ||
        y64 < minY64 || y64 >= minY64 + spanY)
    {
        return std::nullopt;
    }

    const uint32_t col = static_cast<uint32_t>((x64 - minX64) / static_cast<int64_t>(cellSize_));
    const uint32_t row = static_cast<uint32_t>((y64 - minY64) / static_cast<int64_t>(cellSize_));
    return row * cols_ + col;
}

    
const AoiCell& AoiGrid::getCell(uint32_t cellIdx) const
{
    // 调用方负责合法性检查
    return cells_[cellIdx];
}


std::vector<uint32_t> AoiGrid::getWatchCellIdx(Position x, Position y) const
{
    //根据位置算出所在格子索引
    const auto optIdx = TryCellOf(x, y);
    if (!optIdx.has_value())
        return {};
    return watchCellsOfCell(*optIdx);
}

std::vector<uint32_t> AoiGrid::watchCellsOfCell(uint32_t cellIdx) const
{
    std::vector<uint32_t> result;
    result.reserve(static_cast<size_t>(2 * r_ + 1) * static_cast<size_t>(2 * r_ + 1));
    forEachWatchCellOfCell(cellIdx, [&](uint32_t idx, uint32_t) { result.push_back(idx); });
    return result;
}

uint32_t AoiGrid::membersRev(uint32_t cell) const
{
    if (cell >= cells_.size())
        return 0;
    return cells_[cell].membersRev_;
}


bool AoiGrid::visible(uint32_t cellA, uint32_t cellB) const
{
    if (cellA >= cells_.size() || cellB >= cells_.size())
        return false;

    const int32_t colA = static_cast<int32_t>(cellA % cols_);
    const int32_t rowA = static_cast<int32_t>(cellA / cols_);
    const int32_t colB = static_cast<int32_t>(cellB % cols_);
    const int32_t rowB = static_cast<int32_t>(cellB / cols_);

    const int32_t dx = colA > colB ? colA - colB : colB - colA;
    const int32_t dy = rowA > rowB ? rowA - rowB : rowB - rowA;
    const int32_t r  = static_cast<int32_t>(r_);

    // 切比雪夫距离：两个格子都在对方的方形视野内
    return dx <= r && dy <= r;
}


void AoiGrid::addEntity(EntityId entityId, uint32_t cell) 
{
    if (cell >= cells_.size())
        return;
    auto& target = cells_[cell];
    // 幂等：重复加入不制造重复条目，也不触发无谓的结构变更
    if (target.entPos_.find(entityId) != target.entPos_.end())
        return;
    target.entPos_.emplace(entityId, static_cast<uint32_t>(target.entys_.size()));
    target.entys_.push_back(entityId);
    ++target.membersRev_;   // 成员集合变化：让观察者知道需要重建
}

void AoiGrid::removeEntity(EntityId entityId, uint32_t cell)
{
    if (cell >= cells_.size())
        return;

    auto& target = cells_[cell];
    const auto it = target.entPos_.find(entityId);
    if (it == target.entPos_.end())
        return;   // 幂等删除不触发无谓重建

    // O(1) swap-pop：把末尾元素挪到被删位置，避免 std::remove 的尾部整体搬移
    const uint32_t idx = it->second;
    const EntityId last = target.entys_.back();
    target.entys_[idx] = last;
    target.entPos_[last] = idx;
    target.entys_.pop_back();
    target.entPos_.erase(it);
    ++target.membersRev_;
}


void AoiGrid::addWatcher(EntityId observerId, uint32_t cell)
{
    if (cell >= cells_.size())
        return;
    auto& target = cells_[cell];
    if (target.watcherPos_.find(observerId) != target.watcherPos_.end())
        return;   // 幂等
    target.watcherPos_.emplace(observerId, static_cast<uint32_t>(target.watchers_.size()));
    target.watchers_.push_back(observerId);
}

void AoiGrid::removeWatcher(EntityId observerId, uint32_t cell)
{
    if (cell >= cells_.size())
        return;

    auto& target = cells_[cell];
    const auto it = target.watcherPos_.find(observerId);
    if (it == target.watcherPos_.end())
        return;   // 幂等

    const uint32_t idx = it->second;
    const EntityId last = target.watchers_.back();
    target.watchers_[idx] = last;
    target.watcherPos_[last] = idx;
    target.watchers_.pop_back();
    target.watcherPos_.erase(it);
}


const std::vector<EntityId>& AoiGrid::entities(uint32_t cell) const
{
    // 越界时返回一个静态空 vector，避免 UB
    static const std::vector<EntityId> kEmpty;
    if (cell >= cells_.size())
        return kEmpty;
    return cells_[cell].entys_;
}

const std::vector<EntityId>& AoiGrid::watchers(uint32_t cell) const
{
    static const std::vector<EntityId> kEmpty;
    if (cell >= cells_.size())
        return kEmpty;
    return cells_[cell].watchers_;
}
