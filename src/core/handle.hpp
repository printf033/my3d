#pragma once

// my3d::core —— 句柄与槽容器。
//
// 目的：替换旧 engine.hpp 里 "字符串键 → 后端 GPU 对象" 的索引方式
// （getView("myView") / getCamera("myCamera") / getEntity("myLight")）。
// 字符串键的问题是：拼写错误在运行期才暴露，且 map 查找带堆分配与哈希开销。
// Handle 是 POD，可比较、可哈希、可放进 std::vector 做剔除结果，且
// 带 generation 校验 —— 悬垂句柄会被检测出来而不是读到复用后的对象。

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace my3d
{

inline constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

// 每个资源类别一个 Tag（MeshTag / TextureTag / ...），
// 让不同类别的句柄在编译期就不可互换。
template <typename Tag>
struct Handle
{
    uint32_t index = kInvalidIndex;
    uint32_t generation = 0;

    constexpr bool valid() const noexcept { return index != kInvalidIndex; }
    constexpr explicit operator bool() const noexcept { return valid(); }

    friend constexpr bool operator==(Handle a, Handle b) noexcept
    {
        return a.index == b.index && a.generation == b.generation;
    }
    friend constexpr bool operator!=(Handle a, Handle b) noexcept { return !(a == b); }
    friend constexpr bool operator<(Handle a, Handle b) noexcept
    {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    }
};

// 语义化标签：只用名字，不定义实体。
struct MeshTag;
struct TextureTag;
struct MaterialTag;
struct IblTag;
struct EntityTag;
struct ViewTag;
struct CameraTag;
struct LightTag;

using MeshHandle = Handle<MeshTag>;
using TextureHandle = Handle<TextureTag>;
using MaterialHandle = Handle<MaterialTag>;
using IblHandle = Handle<IblTag>;
using EntityHandle = Handle<EntityTag>;
using ViewHandle = Handle<ViewTag>;
using CameraHandle = Handle<CameraTag>;
using LightHandle = Handle<LightTag>;

// SlotMap<T, Tag>：紧凑数组 + 空闲列表，O(1) 增删查。
//
// generation 从 1 起递增，0 保留表示空闲槽；回绕（40 亿次复用同一槽）后
// 理论上存在旧句柄复活，实际不作处理。
template <typename T, typename Tag>
class SlotMap
{
public:
    using HandleType = Handle<Tag>;

    HandleType insert(T value)
    {
        uint32_t idx;
        if (!freeList_.empty())
        {
            idx = freeList_.back();
            freeList_.pop_back();
            slots_[idx].value = std::move(value);
        }
        else
        {
            idx = static_cast<uint32_t>(slots_.size());
            slots_.push_back(Slot{std::move(value), 0});
        }
        Slot &slot = slots_[idx];
        if (slot.generation == 0)
            slot.generation = 1;
        ++alive_;
        return HandleType{idx, slot.generation};
    }

    // 校验句柄。悬垂（已删除或 generation 不匹配）返回 nullptr。
    T *get(HandleType h) noexcept
    {
        if (!isAlive(h))
            return nullptr;
        return &slots_[h.index].value;
    }
    const T *get(HandleType h) const noexcept
    {
        if (!isAlive(h))
            return nullptr;
        return &slots_[h.index].value;
    }

    bool contains(HandleType h) const noexcept { return isAlive(h); }

    bool erase(HandleType h)
    {
        if (!isAlive(h))
            return false;
        Slot &slot = slots_[h.index];
        slot.value = T{};                 // 立刻释放其持有的资源
        ++slot.generation;
        if (slot.generation == 0)
            slot.generation = 1;          // 跳过保留值 0
        freeList_.push_back(h.index);
        --alive_;
        return true;
    }

    size_t size() const noexcept { return alive_; }
    bool empty() const noexcept { return alive_ == 0; }

    // 槽容量（含已删除的洞），调试/预留用
    size_t capacity() const noexcept { return slots_.size(); }

    void reserve(size_t n) { slots_.reserve(n); freeList_.reserve(n); }

    // 遍历存活项；Fn 签名 void(const T&) 或 void(HandleType, const T&)。
    template <typename Fn>
    void forEach(Fn &&fn) const
    {
        for (uint32_t i = 0; i < slots_.size(); ++i)
        {
            const Slot &slot = slots_[i];
            if (slot.generation == 0)
                continue;
            if constexpr (std::is_invocable_v<Fn, HandleType, const T &>)
                fn(HandleType{i, slot.generation}, slot.value);
            else
                fn(slot.value);
        }
    }

    // 收集全部存活句柄（剔除/排序结果用）
    std::vector<HandleType> handles() const
    {
        std::vector<HandleType> out;
        out.reserve(alive_);
        for (uint32_t i = 0; i < slots_.size(); ++i)
        {
            if (slots_[i].generation != 0)
                out.push_back(HandleType{i, slots_[i].generation});
        }
        return out;
    }

    void clear()
    {
        slots_.clear();
        freeList_.clear();
        alive_ = 0;
    }

private:
    struct Slot
    {
        T value{};
        uint32_t generation = 0;   // 0 表示空闲
    };

    bool isAlive(HandleType h) const noexcept
    {
        return h.valid() && h.index < slots_.size() &&
               slots_[h.index].generation == h.generation;
    }

    std::vector<Slot> slots_;
    std::vector<uint32_t> freeList_;
    size_t alive_ = 0;
};

} // namespace my3d
