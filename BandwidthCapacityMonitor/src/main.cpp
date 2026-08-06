#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0600

#include "MonitorMath.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::atomic_bool g_stopRequested = false;

// =====wjy====
BOOL WINAPI consoleControlHandler(DWORD controlType)
{
    if (controlType == CTRL_C_EVENT || controlType == CTRL_BREAK_EVENT || controlType == CTRL_CLOSE_EVENT) {
        g_stopRequested.store(true); // wjy: Ctrl+C、Ctrl+Break 或关闭控制台时只设置停止标志，让主循环完成当前采样后安全退出。
        return TRUE;
    }
    return FALSE;
}

std::uint64_t fileTimeValue(const FILETIME& value)
{
    ULARGE_INTEGER converted{};
    converted.LowPart = value.dwLowDateTime; // wjy: Windows FILETIME 的低 32 位先写入联合体。
    converted.HighPart = value.dwHighDateTime; // wjy: 高 32 位组合后得到 100 纳秒单位的完整计数。
    return converted.QuadPart;
}

std::string wideToUtf8(std::wstring_view value)
{
    if (value.empty()) {
        return {};
    }
    const int required = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr); // wjy: 先查询 UTF-8 字节数，避免按宽字符长度错误分配缓冲区。
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    ::WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr); // wjy: 网卡中文名称和进程名称统一转成 UTF-8 输出到现代 Windows 终端与 CSV。
    return result;
}

std::string localTimestamp()
{
    SYSTEMTIME time{};
    ::GetLocalTime(&time); // wjy: 使用本地时间为每次采样和 CSV 行生成可直接对照卡顿时刻的时间戳。
    std::ostringstream stream;
    stream << std::setfill('0')
           << std::setw(4) << time.wYear << '-'
           << std::setw(2) << time.wMonth << '-'
           << std::setw(2) << time.wDay << ' '
           << std::setw(2) << time.wHour << ':'
           << std::setw(2) << time.wMinute << ':'
           << std::setw(2) << time.wSecond;
    return stream.str();
}

struct Options {
    DWORD intervalMilliseconds = 1000; // wjy: 默认一秒采样兼顾突发可见性和很低的监控开销。
    DWORD durationSeconds = 0; // wjy: 零表示持续监控到用户按 Ctrl+C。
    NET_IFINDEX interfaceIndex = 0; // wjy: 零表示展示全部活动物理网卡，非零时只看指定接口。
    double capacityOverrideMbps = 0.0; // wjy: 可选实测有效容量覆盖协商速率，提高 Wi-Fi 或受限上联口余量可信度。
    std::wstring processName = L"FSRemote.exe"; // wjy: 默认同时观察 FSRemote 资源压力，帮助区分网络与本机性能瓶颈。
    std::filesystem::path csvPath;
    bool listOnly = false;
};

bool parseUnsigned(const wchar_t* text, unsigned long minimum, unsigned long maximum, unsigned long* output)
{
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long value = std::wcstoul(text, &end, 10); // wjy: 所有整数参数都要求完整十进制文本，拒绝尾随错误字符。
    if (errno != 0 || end == text || *end != L'\0' || value < minimum || value > maximum) {
        return false;
    }
    *output = value;
    return true;
}

bool parsePositiveDouble(const wchar_t* text, double* output)
{
    wchar_t* end = nullptr;
    errno = 0;
    const double value = std::wcstod(text, &end); // wjy: 有效容量允许小数 Mbps，便于填入 940.5 等实测值。
    if (errno != 0 || end == text || *end != L'\0' || !std::isfinite(value) || value <= 0.0 || value > 100'000.0) {
        return false;
    }
    *output = value;
    return true;
}

void printHelp()
{
    std::cout
        << "BandwidthCapacityMonitor - FSRemote 局域网带宽余量监控器\n\n"
        << "用法:\n"
        << "  BandwidthCapacityMonitor.exe [选项]\n\n"
        << "选项:\n"
        << "  --list                    列出当前活动物理网卡后退出\n"
        << "  --interface <索引>        只监控指定网卡；默认显示全部活动物理网卡\n"
        << "  --interval-ms <毫秒>      采样周期，范围 250-60000，默认 1000\n"
        << "  --duration <秒>           运行时长；0 表示持续到 Ctrl+C，默认 0\n"
        << "  --capacity-mbps <Mbps>    用实测有效容量替代理论链路速率\n"
        << "  --process <文件名>        要观察的进程，默认 FSRemote.exe\n"
        << "  --csv <文件路径>          同时把每次网卡采样保存为 UTF-8 CSV\n"
        << "  --help                    显示本帮助\n\n"
        << "示例:\n"
        << "  BandwidthCapacityMonitor.exe\n"
        << "  BandwidthCapacityMonitor.exe --list\n"
        << "  BandwidthCapacityMonitor.exe --interface 12 --capacity-mbps 940 --csv monitor.csv\n";
}

std::optional<Options> parseOptions(int argc, wchar_t* argv[], bool* helpDisplayed)
{
    Options options;
    *helpDisplayed = false; // wjy: 调用方据此区分正常帮助退出和参数错误，脚本自动化能得到正确退出码。
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        const auto requireValue = [&](const char* displayName) -> const wchar_t* {
            if (index + 1 >= argc) {
                std::cerr << "缺少参数值: " << displayName << '\n';
                return nullptr;
            }
            return argv[++index];
        };

        if (argument == L"--help" || argument == L"-h") {
            printHelp();
            *helpDisplayed = true;
            return std::nullopt; // wjy: 帮助请求正常结束，不启动任何网卡或进程采样。
        }
        if (argument == L"--list") {
            options.listOnly = true;
            continue;
        }
        if (argument == L"--interface") {
            const wchar_t* valueText = requireValue("--interface");
            unsigned long value = 0;
            if (!valueText || !parseUnsigned(valueText, 1, std::numeric_limits<ULONG>::max(), &value)) {
                std::cerr << "--interface 必须是有效的正整数网卡索引\n";
                return std::nullopt;
            }
            options.interfaceIndex = static_cast<NET_IFINDEX>(value); // wjy: 接口索引直接对应 --list 输出，避免依赖可能重复或变化的显示名称。
            continue;
        }
        if (argument == L"--interval-ms") {
            const wchar_t* valueText = requireValue("--interval-ms");
            unsigned long value = 0;
            if (!valueText || !parseUnsigned(valueText, 250, 60'000, &value)) {
                std::cerr << "--interval-ms 必须在 250 到 60000 之间\n";
                return std::nullopt;
            }
            options.intervalMilliseconds = static_cast<DWORD>(value);
            continue;
        }
        if (argument == L"--duration") {
            const wchar_t* valueText = requireValue("--duration");
            unsigned long value = 0;
            if (!valueText || !parseUnsigned(valueText, 0, 7 * 24 * 60 * 60, &value)) {
                std::cerr << "--duration 必须是 0 到 604800 之间的秒数\n";
                return std::nullopt;
            }
            options.durationSeconds = static_cast<DWORD>(value);
            continue;
        }
        if (argument == L"--capacity-mbps") {
            const wchar_t* valueText = requireValue("--capacity-mbps");
            if (!valueText || !parsePositiveDouble(valueText, &options.capacityOverrideMbps)) {
                std::cerr << "--capacity-mbps 必须是 0 到 100000 之间的正数\n";
                return std::nullopt;
            }
            continue;
        }
        if (argument == L"--process") {
            const wchar_t* valueText = requireValue("--process");
            if (!valueText || *valueText == L'\0') {
                std::cerr << "--process 不能为空\n";
                return std::nullopt;
            }
            options.processName = valueText;
            continue;
        }
        if (argument == L"--csv") {
            const wchar_t* valueText = requireValue("--csv");
            if (!valueText || *valueText == L'\0') {
                std::cerr << "--csv 路径不能为空\n";
                return std::nullopt;
            }
            options.csvPath = valueText;
            continue;
        }

        std::cerr << "未知参数: " << wideToUtf8(argument) << "\n使用 --help 查看用法。\n";
        return std::nullopt;
    }
    return options;
}

struct AdapterSample {
    NET_IFINDEX index = 0;
    std::wstring alias;
    std::wstring description;
    ULONG type = IF_TYPE_OTHER; // wjy: Windows SDK 将接口类型保存为 ULONG 数值，避免依赖不存在的 IF_TYPE 类型别名。
    bool hardware = false;
    bool wireless = false;
    std::uint64_t receiveLinkSpeed = 0;
    std::uint64_t transmitLinkSpeed = 0;
    std::uint64_t inOctets = 0;
    std::uint64_t outOctets = 0;
    std::uint64_t inDiscards = 0;
    std::uint64_t outDiscards = 0;
    std::uint64_t inErrors = 0;
    std::uint64_t outErrors = 0;
};

bool isUsableAdapter(const MIB_IF_ROW2& row)
{
    return row.OperStatus == IfOperStatusUp
        && row.MediaConnectState == MediaConnectStateConnected
        && row.Type != IF_TYPE_SOFTWARE_LOOPBACK
        && row.Type != IF_TYPE_TUNNEL; // wjy: 排除断开、回环和隧道接口，避免把本机虚拟流量误当成局域网容量。
}

std::vector<AdapterSample> queryAdapters()
{
    PMIB_IF_TABLE2 table = nullptr;
    if (::GetIfTable2(&table) != NO_ERROR || table == nullptr) {
        return {};
    }

    std::vector<AdapterSample> allUsable;
    allUsable.reserve(table->NumEntries);
    bool hasHardwareAdapter = false;
    for (ULONG index = 0; index < table->NumEntries; ++index) {
        const MIB_IF_ROW2& row = table->Table[index];
        if (!isUsableAdapter(row)) {
            continue;
        }
        AdapterSample sample;
        sample.index = row.InterfaceIndex;
        sample.alias = row.Alias;
        sample.description = row.Description;
        sample.type = row.Type;
        sample.hardware = row.InterfaceAndOperStatusFlags.HardwareInterface != 0; // wjy: 优先保留真实硬件，过滤 VMware、Hyper-V 等虚拟交换接口的重复计数。
        sample.wireless = row.Type == IF_TYPE_IEEE80211;
        sample.receiveLinkSpeed = row.ReceiveLinkSpeed;
        sample.transmitLinkSpeed = row.TransmitLinkSpeed;
        sample.inOctets = row.InOctets;
        sample.outOctets = row.OutOctets;
        sample.inDiscards = row.InDiscards;
        sample.outDiscards = row.OutDiscards;
        sample.inErrors = row.InErrors;
        sample.outErrors = row.OutErrors;
        hasHardwareAdapter = hasHardwareAdapter || sample.hardware;
        allUsable.push_back(std::move(sample));
    }
    ::FreeMibTable(table); // wjy: IP Helper 分配的网卡表在复制必要字段后立即释放，长期监控不会积累内存。

    if (hasHardwareAdapter) {
        std::erase_if(allUsable, [](const AdapterSample& sample) {
            return !sample.hardware; // wjy: 存在物理接口时去掉虚拟接口，避免同一数据包在虚拟交换层和物理层被重复显示。
        });
    }
    std::ranges::sort(allUsable, {}, &AdapterSample::index);
    return allUsable;
}

const AdapterSample* findAdapter(const std::vector<AdapterSample>& adapters, NET_IFINDEX index)
{
    const auto found = std::ranges::find(adapters, index, &AdapterSample::index);
    return found == adapters.end() ? nullptr : &*found;
}

struct SystemCpuSample {
    std::uint64_t idle = 0;
    std::uint64_t kernel = 0;
    std::uint64_t user = 0;
};

std::optional<SystemCpuSample> readSystemCpuSample()
{
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (!::GetSystemTimes(&idle, &kernel, &user)) {
        return std::nullopt;
    }
    return SystemCpuSample{fileTimeValue(idle), fileTimeValue(kernel), fileTimeValue(user)}; // wjy: 保存累计 CPU 时间，下一周期用差值计算整机忙碌比例。
}

double calculateSystemCpuPercent(const SystemCpuSample& previous, const SystemCpuSample& current)
{
    const std::uint64_t idleDelta = bandwidth_monitor::safeCounterDelta(current.idle, previous.idle);
    const std::uint64_t kernelDelta = bandwidth_monitor::safeCounterDelta(current.kernel, previous.kernel);
    const std::uint64_t userDelta = bandwidth_monitor::safeCounterDelta(current.user, previous.user);
    const std::uint64_t totalDelta = kernelDelta + userDelta;
    if (totalDelta == 0 || idleDelta > totalDelta) {
        return 0.0;
    }
    return static_cast<double>(totalDelta - idleDelta) * 100.0 / static_cast<double>(totalDelta); // wjy: kernel 时间包含 idle，必须先减 idle 才得到整机真实使用率。
}

struct ProcessReport {
    std::size_t processCount = 0;
    std::size_t threadCount = 0;
    std::uint64_t workingSetBytes = 0;
    std::uint64_t privateBytes = 0;
    double cpuPercent = 0.0;
};

struct ProcessCpuTime {
    std::uint64_t total = 0;
};

class ProcessMonitor final {
public:
    ProcessMonitor()
    {
        SYSTEM_INFO information{};
        ::GetSystemInfo(&information);
        processorCount_ = std::max<DWORD>(1, information.dwNumberOfProcessors); // wjy: 进程 CPU 按整机总逻辑处理器容量归一化，数值可与整机 CPU 直接比较。
    }

    ProcessReport sample(const std::wstring& targetName, double elapsedSeconds)
    {
        ProcessReport report;
        std::unordered_map<DWORD, ProcessCpuTime> currentTimes;
        const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            previousTimes_.clear();
            return report;
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        BOOL hasEntry = ::Process32FirstW(snapshot, &entry);
        while (hasEntry) {
            if (_wcsicmp(entry.szExeFile, targetName.c_str()) == 0) {
                ++report.processCount; // wjy: 同名进程全部聚合，兼容用户意外启动多个 FSRemote 实例时仍能看到总压力。
                report.threadCount += entry.cntThreads;
                sampleProcess(entry.th32ProcessID, &report, &currentTimes);
            }
            hasEntry = ::Process32NextW(snapshot, &entry);
        }
        ::CloseHandle(snapshot);

        std::uint64_t processTimeDelta = 0;
        for (const auto& [processId, current] : currentTimes) {
            const auto previous = previousTimes_.find(processId);
            if (previous != previousTimes_.end()) {
                processTimeDelta += bandwidth_monitor::safeCounterDelta(current.total, previous->second.total); // wjy: 新启动进程没有基线，本周期不把其历史 CPU 时间误算成瞬时峰值。
            }
        }
        const double availableTime = elapsedSeconds * 10'000'000.0 * static_cast<double>(processorCount_);
        report.cpuPercent = availableTime > 0.0
            ? static_cast<double>(processTimeDelta) * 100.0 / availableTime
            : 0.0; // wjy: FILETIME 使用 100 纳秒单位，并除以逻辑处理器数得到占整机容量百分比。
        previousTimes_ = std::move(currentTimes);
        return report;
    }

private:
    static void sampleProcess(
        DWORD processId,
        ProcessReport* report,
        std::unordered_map<DWORD, ProcessCpuTime>* currentTimes)
    {
        const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, processId);
        if (process == nullptr) {
            return; // wjy: 权限不足时仍保留快照提供的进程数和线程数，不中断网络监控。
        }

        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        if (::GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
            (*currentTimes)[processId] = ProcessCpuTime{fileTimeValue(kernel) + fileTimeValue(user)}; // wjy: 保存每个 PID 的内核与用户累计时间，避免进程重启后的计数混淆。
        }

        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (::GetProcessMemoryInfo(
                process,
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                sizeof(memory))) {
            report->workingSetBytes += memory.WorkingSetSize; // wjy: 工作集反映当前占用物理内存，便于识别多窗口解码缓存压力。
            report->privateBytes += memory.PrivateUsage; // wjy: 私有提交量帮助观察是否存在随远控窗口数量增长的内存占用。
        }
        ::CloseHandle(process);
    }

    DWORD processorCount_ = 1;
    std::unordered_map<DWORD, ProcessCpuTime> previousTimes_;
};

const char* riskLabel(bandwidth_monitor::RiskLevel risk)
{
    switch (risk) {
    case bandwidth_monitor::RiskLevel::Normal:
        return "正常";
    case bandwidth_monitor::RiskLevel::Attention:
        return "关注";
    case bandwidth_monitor::RiskLevel::High:
        return "高风险";
    case bandwidth_monitor::RiskLevel::Saturated:
        return "接近饱和";
    }
    return "未知";
}

double bytesToMegabytes(std::uint64_t bytes)
{
    return static_cast<double>(bytes) / 1024.0 / 1024.0;
}

std::string csvEscape(const std::string& value)
{
    std::string escaped = "\"";
    for (const char character : value) {
        if (character == '\"') {
            escaped += "\"\""; // wjy: CSV 字段中的双引号按标准重复，中文网卡名称可安全被 Excel 读取。
        } else {
            escaped += character;
        }
    }
    escaped += '\"';
    return escaped;
}

class CsvWriter final {
public:
    explicit CsvWriter(const std::filesystem::path& path)
    {
        if (path.empty()) {
            return;
        }
        file_.open(path, std::ios::binary | std::ios::trunc);
        if (!file_) {
            return;
        }
        file_ << "\xEF\xBB\xBF"; // wjy: 写入 UTF-8 BOM，让 Windows Excel 直接识别中文网卡名称和风险文字。
        file_ << "timestamp,interface_index,interface_name,rx_mbps,rx_capacity_mbps,rx_headroom_mbps,rx_utilization_percent,rx_risk,"
                 "tx_mbps,tx_capacity_mbps,tx_headroom_mbps,tx_utilization_percent,tx_risk,in_discards_delta,out_discards_delta,"
                 "in_errors_delta,out_errors_delta,system_cpu_percent,process_count,process_cpu_percent,process_working_set_mb,"
                 "process_private_mb,process_threads\n";
    }

    bool isOpen() const
    {
        return file_.is_open();
    }

    void append(
        const std::string& timestamp,
        const AdapterSample& current,
        const bandwidth_monitor::DirectionMetrics& receive,
        const bandwidth_monitor::DirectionMetrics& transmit,
        std::uint64_t inDiscardsDelta,
        std::uint64_t outDiscardsDelta,
        std::uint64_t inErrorsDelta,
        std::uint64_t outErrorsDelta,
        double systemCpuPercent,
        const ProcessReport& process)
    {
        if (!file_) {
            return;
        }
        file_ << timestamp << ','
              << current.index << ','
              << csvEscape(wideToUtf8(current.alias)) << ','
              << receive.currentMbps << ','
              << receive.capacityMbps << ','
              << receive.headroomMbps << ','
              << receive.utilizationPercent << ','
              << csvEscape(riskLabel(receive.risk)) << ','
              << transmit.currentMbps << ','
              << transmit.capacityMbps << ','
              << transmit.headroomMbps << ','
              << transmit.utilizationPercent << ','
              << csvEscape(riskLabel(transmit.risk)) << ','
              << inDiscardsDelta << ','
              << outDiscardsDelta << ','
              << inErrorsDelta << ','
              << outErrorsDelta << ','
              << systemCpuPercent << ','
              << process.processCount << ','
              << process.cpuPercent << ','
              << bytesToMegabytes(process.workingSetBytes) << ','
              << bytesToMegabytes(process.privateBytes) << ','
              << process.threadCount << '\n';
        file_.flush(); // wjy: 每次采样立即落盘，即使卡死或强制关机也尽量保留故障前最后几秒数据。
    }

private:
    std::ofstream file_;
};

void printAdapterList(const std::vector<AdapterSample>& adapters)
{
    if (adapters.empty()) {
        std::cout << "未找到已连接的活动物理网卡。\n";
        return;
    }
    std::cout << "活动物理网卡:\n";
    for (const AdapterSample& adapter : adapters) {
        std::cout << "  [" << adapter.index << "] " << wideToUtf8(adapter.alias)
                  << " | " << wideToUtf8(adapter.description)
                  << " | 接收链路 " << std::fixed << std::setprecision(1)
                  << static_cast<double>(adapter.receiveLinkSpeed) / 1'000'000.0 << " Mbps"
                  << " | 发送链路 " << static_cast<double>(adapter.transmitLinkSpeed) / 1'000'000.0 << " Mbps";
        if (adapter.wireless) {
            std::cout << " | Wi-Fi";
        }
        std::cout << '\n';
    }
}

void printAdapterReport(
    const AdapterSample& previous,
    const AdapterSample& current,
    double elapsedSeconds,
    double capacityOverrideMbps,
    double systemCpuPercent,
    const ProcessReport& process,
    CsvWriter* csv,
    const std::string& timestamp)
{
    const auto receive = bandwidth_monitor::calculateDirection(
        bandwidth_monitor::safeCounterDelta(current.inOctets, previous.inOctets),
        elapsedSeconds,
        current.receiveLinkSpeed,
        capacityOverrideMbps); // wjy: 接收方向对应 FSRemote 多路视频的主要流量，独立计算当前占用和理论余量。
    const auto transmit = bandwidth_monitor::calculateDirection(
        bandwidth_monitor::safeCounterDelta(current.outOctets, previous.outOctets),
        elapsedSeconds,
        current.transmitLinkSpeed,
        capacityOverrideMbps); // wjy: 发送方向包含输入、控制和其它局域网流量，避免只看下载忽略反向拥塞。
    const std::uint64_t inDiscardsDelta = bandwidth_monitor::safeCounterDelta(current.inDiscards, previous.inDiscards);
    const std::uint64_t outDiscardsDelta = bandwidth_monitor::safeCounterDelta(current.outDiscards, previous.outDiscards);
    const std::uint64_t inErrorsDelta = bandwidth_monitor::safeCounterDelta(current.inErrors, previous.inErrors);
    const std::uint64_t outErrorsDelta = bandwidth_monitor::safeCounterDelta(current.outErrors, previous.outErrors);

    std::cout << "网卡 [" << current.index << "] " << wideToUtf8(current.alias);
    if (current.wireless) {
        std::cout << " (Wi-Fi，链路余量仅作上限参考)";
    }
    std::cout << '\n'
              << "  接收: " << std::fixed << std::setprecision(1)
              << receive.currentMbps << " / " << receive.capacityMbps << " Mbps"
              << " | 理论余量 " << receive.headroomMbps << " Mbps"
              << " | 利用率 " << receive.utilizationPercent << "% [" << riskLabel(receive.risk) << "]\n"
              << "  发送: " << transmit.currentMbps << " / " << transmit.capacityMbps << " Mbps"
              << " | 理论余量 " << transmit.headroomMbps << " Mbps"
              << " | 利用率 " << transmit.utilizationPercent << "% [" << riskLabel(transmit.risk) << "]\n"
              << "  本周期网卡计数: 入丢弃 " << inDiscardsDelta
              << "，出丢弃 " << outDiscardsDelta
              << "，入错误 " << inErrorsDelta
              << "，出错误 " << outErrorsDelta << '\n';

    if (inDiscardsDelta > 0 || outDiscardsDelta > 0 || inErrorsDelta > 0 || outErrorsDelta > 0) {
        std::cout << "  警告: 网卡丢弃或错误正在增长，可能已经出现驱动、队列或链路压力。\n"; // wjy: 丢弃增量比单纯利用率更接近已经发生的本机网络异常证据。
    }
    if (csv != nullptr) {
        csv->append(
            timestamp,
            current,
            receive,
            transmit,
            inDiscardsDelta,
            outDiscardsDelta,
            inErrorsDelta,
            outErrorsDelta,
            systemCpuPercent,
            process);
    }
}
// ===end====

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    // =====wjy====
    ::SetConsoleOutputCP(CP_UTF8); // wjy: 保证中文监控结果在 Windows Terminal 和重定向文件中使用 UTF-8。
    ::SetConsoleCtrlHandler(consoleControlHandler, TRUE); // wjy: 注册安全退出处理，持续监控时可随时按 Ctrl+C 停止。

    bool helpDisplayed = false;
    const std::optional<Options> parsedOptions = parseOptions(argc, argv, &helpDisplayed);
    if (!parsedOptions.has_value()) {
        return helpDisplayed ? 0 : 1; // wjy: --help 返回成功，未知参数或非法数值返回失败，便于批处理可靠判断。
    }
    const Options& options = *parsedOptions;
    std::vector<AdapterSample> previousAdapters = queryAdapters();
    if (options.listOnly) {
        printAdapterList(previousAdapters);
        return previousAdapters.empty() ? 1 : 0;
    }
    if (previousAdapters.empty()) {
        std::cerr << "未找到已连接的活动物理网卡，请先检查网络连接。\n";
        return 2;
    }
    if (options.interfaceIndex != 0 && findAdapter(previousAdapters, options.interfaceIndex) == nullptr) {
        std::cerr << "没有找到索引为 " << options.interfaceIndex << " 的活动物理网卡，请先运行 --list。\n";
        return 2;
    }

    CsvWriter csv(options.csvPath);
    if (!options.csvPath.empty() && !csv.isOpen()) {
        std::cerr << "无法创建 CSV 文件: " << wideToUtf8(options.csvPath.wstring()) << '\n';
        return 3;
    }

    std::cout << "FSRemote 局域网带宽余量监控已启动，按 Ctrl+C 停止。\n"
              << "说明: 默认余量 = Windows 网卡协商速率 - 当前实际速率；它不代表交换机、Wi-Fi 空口或远端设备的保证余量。\n";
    if (options.capacityOverrideMbps > 0.0) {
        std::cout << "当前使用手工有效容量: " << std::fixed << std::setprecision(1)
                  << options.capacityOverrideMbps << " Mbps。\n";
    } else {
        std::cout << "如已知实测有效容量，请用 --capacity-mbps 填入以提高余量可信度。\n";
    }
    if (!options.csvPath.empty()) {
        std::cout << "CSV 记录: " << wideToUtf8(std::filesystem::absolute(options.csvPath).wstring()) << '\n';
    }
    printAdapterList(previousAdapters);
    std::cout << '\n';

    ProcessMonitor processMonitor;
    (void)processMonitor.sample(options.processName, 0.0); // wjy: 首次只建立各 FSRemote PID 的 CPU 时间基线，不输出虚假的启动峰值。
    std::optional<SystemCpuSample> previousSystemCpu = readSystemCpuSample();
    const Clock::time_point startedAt = Clock::now();
    Clock::time_point previousTime = startedAt;

    while (!g_stopRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options.intervalMilliseconds)); // wjy: 被动等待采样周期，不轮询网卡也不产生任何网络测试流量。
        const Clock::time_point currentTime = Clock::now();
        const double elapsedSeconds = std::chrono::duration<double>(currentTime - previousTime).count();
        std::vector<AdapterSample> currentAdapters = queryAdapters();
        const std::optional<SystemCpuSample> currentSystemCpu = readSystemCpuSample();
        const double systemCpuPercent = previousSystemCpu && currentSystemCpu
            ? calculateSystemCpuPercent(*previousSystemCpu, *currentSystemCpu)
            : 0.0;
        const ProcessReport process = processMonitor.sample(options.processName, elapsedSeconds);
        const std::string timestamp = localTimestamp();

        std::cout << '[' << timestamp << "] 整机 CPU " << std::fixed << std::setprecision(1)
                  << systemCpuPercent << "% | " << wideToUtf8(options.processName)
                  << ": 进程 " << process.processCount
                  << "，CPU " << process.cpuPercent << "%"
                  << "，工作集 " << bytesToMegabytes(process.workingSetBytes) << " MB"
                  << "，私有内存 " << bytesToMegabytes(process.privateBytes) << " MB"
                  << "，线程 " << process.threadCount << '\n';

        bool printedAdapter = false;
        for (const AdapterSample& current : currentAdapters) {
            if (options.interfaceIndex != 0 && current.index != options.interfaceIndex) {
                continue;
            }
            const AdapterSample* previous = findAdapter(previousAdapters, current.index);
            if (previous == nullptr) {
                continue; // wjy: 新接入网卡先建立一周期基线，防止把开机以来累计字节当成瞬时流量。
            }
            printAdapterReport(
                *previous,
                current,
                elapsedSeconds,
                options.capacityOverrideMbps,
                systemCpuPercent,
                process,
                &csv,
                timestamp);
            printedAdapter = true;
        }
        if (!printedAdapter) {
            std::cout << "  指定网卡当前不可用或刚刚重新连接，等待下一次采样。\n";
        }
        std::cout << std::flush << '\n';

        previousAdapters = std::move(currentAdapters);
        previousSystemCpu = currentSystemCpu;
        previousTime = currentTime;
        if (options.durationSeconds > 0
            && std::chrono::duration_cast<std::chrono::seconds>(currentTime - startedAt).count()
                >= options.durationSeconds) {
            break; // wjy: 到达用户指定时长后正常结束并保证 CSV 已逐行落盘。
        }
    }

    std::cout << "监控已停止。\n";
    return 0;
    // ===end====
}
