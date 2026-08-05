#pragma once

#include <QtGlobal>
#include <QString>
#include <QVector>

namespace platform {

// =====wjy====
struct LocalDiskInfo {
    QString rootPath; // wjy: 保存 C:\、D:\ 等实际盘符根路径，界面按枚举顺序逐项显示而不是假定只有系统盘。
    quint64 usedBytes = 0; // wjy: 已用容量由总容量减剩余容量得到，不扫描目录或读取用户文件内容。
    quint64 totalBytes = 0; // wjy: 总容量用于格式化 GB 和计算当前盘符占用百分比。
};

struct LocalGpuInfo {
    QString name; // wjy: 保存 DXGI 返回的首选硬件适配器名称，界面只展示摘要而不暴露驱动内部标识。
    quint64 dedicatedVideoMemoryBytes = 0; // wjy: 专用显存容量来自 DXGI，集成显卡可能合法返回 0 并由界面显示占位。
};

struct LocalMemoryModuleInfo {
    quint64 capacityBytes = 0; // wjy: 单条物理内存容量用于生成“2 × 16 GB”或混合容量组合。
    QString memoryType; // wjy: SMBIOS 类型转换为 DDR4、DDR5 等稳定文本，未知枚举保留为空。
    int configuredSpeedMtps = 0; // wjy: 使用 WMI ConfiguredClockSpeed 作为当前配置速率并按 MT/s 展示。
    QString deviceLocator; // wjy: 插槽定位只用于稳定排序，不在主界面公开序列号、厂商或零件编号。
};

struct LocalMemoryUsage {
    int percent = -1; // wjy: 当前物理内存负载限制为 0-100，查询失败保持 -1 供圆环显示采样占位。
    quint64 usedBytes = 0; // wjy: 已用容量由总量减当前可用物理内存得到，和百分比来自同一份 Windows 快照。
    quint64 totalBytes = 0; // wjy: 动态快照携带总量，避免刷新期间静态与动态查询时间点不一致。
    quint64 availableBytes = 0; // wjy: 可用物理内存直接用于界面显示，不通过整数百分比反推近似容量。
};

struct LocalSystemInfo {
    QString cpuModel; // wjy: Windows 从只读注册表读取 CPU 型号，缺失时回退到架构文本。
    QString cpuArchitecture; // wjy: 记录 x64/arm64 等架构，作为 CPU 型号缺失时的稳定回退信息。
    int logicalProcessorCount = 0; // wjy: 逻辑处理器数量用于判断控制端的可并行处理能力。
    LocalGpuInfo gpu; // wjy: 首选 GPU 静态摘要与实时 GPU Engine 占用分离，避免每秒重复枚举适配器。
    quint64 totalPhysicalMemoryBytes = 0; // wjy: 物理内存总量以字节保存，显示层统一格式化为 GB。
    QVector<LocalMemoryModuleInfo> memoryModules; // wjy: 只保存已安装且容量有效的内存条，条数直接代表已使用插槽数量。
    int totalMemorySlotCount = 0; // wjy: 物理数组没有公开插槽数时保持 0，界面只显示“已安装 N 条”而不伪造总数。
    QVector<LocalDiskInfo> disks; // wjy: 按本机实际可用盘符保存全部磁盘容量，界面行数直接由列表长度决定。
};

class LocalSystemInfoService final {
public:
    static LocalSystemInfo local(); // wjy: 只读采集 CPU、GPU、内存和磁盘硬件摘要，不再读取本机身份或 IPv4 网络信息。
    static QVector<LocalDiskInfo> localDisks(); // wjy: 周期刷新只重新查询盘符容量，不重复执行 CPU、GPU 和内存条静态枚举。
    static QString memoryCapacityComposition(const QVector<LocalMemoryModuleInfo>& modules); // wjy: 把相同容量合并为“数量 × 容量”，混合容量则按内存条逐项连接。
    static QString memoryTypeSummary(const QVector<LocalMemoryModuleInfo>& modules); // wjy: 全部内存条类型一致时返回类型，不一致时返回“混合”。
    static QString memorySpeedSummary(const QVector<LocalMemoryModuleInfo>& modules); // wjy: 全部有效速率一致时返回 MT/s，不一致时返回“混合速率”。
};

class CpuUsageSampler final {
public:
    CpuUsageSampler() = default;
    ~CpuUsageSampler(); // wjy: 析构时关闭 CPU 的 PDH 查询句柄，防止页面对象销毁后遗留系统资源。
    CpuUsageSampler(const CpuUsageSampler&) = delete;
    CpuUsageSampler& operator=(const CpuUsageSampler&) = delete;

    int samplePercent(); // wjy: 按任务管理器的 Processor Utility 口径返回 0-100；首次采样或计数器不可用时返回 -1。
    void reset(); // wjy: 离开本机页后关闭查询并清除采样基线，下次进入时重新建立连续样本。

private:
    quintptr m_queryHandle = 0; // wjy: 用整数保存不透明的 CPU PDH 查询句柄，避免头文件直接依赖 Windows SDK 类型。
    quintptr m_counterHandle = 0; // wjy: 保存 Processor Utility 计数器句柄，其生命周期由当前采样器统一管理。
};

class GpuUsageSampler final {
public:
    GpuUsageSampler() = default;
    ~GpuUsageSampler(); // wjy: 析构时关闭 Windows PDH 查询，避免本机页对象销毁后残留性能计数器句柄。
    GpuUsageSampler(const GpuUsageSampler&) = delete;
    GpuUsageSampler& operator=(const GpuUsageSampler&) = delete;

    int samplePercent(); // wjy: 按任务管理器口径汇总物理 GPU 引擎，返回最繁忙引擎的 0-100 占用；不可用时返回 -1。
    void reset(); // wjy: 离开本机页时关闭 GPU 查询并清除基线，下次进入重新建立性能计数器。

private:
    quintptr m_queryHandle = 0; // wjy: 用整数保存不透明 PDH 查询句柄，头文件无需暴露 Windows SDK 类型。
    quintptr m_counterHandle = 0; // wjy: 保存 GPU Engine 通配符计数器句柄，生命周期归当前采样器管理。
};

class MemoryUsageSampler final {
public:
    LocalMemoryUsage sample() const; // wjy: 一次只读查询同时返回占用率、已用、总量和可用容量，四个字段保持同一时间点。
    int samplePercent() const; // wjy: 保留百分比兼容入口，内部复用完整快照且不保存跨调用状态。
};
// ===end====

} // namespace platform
