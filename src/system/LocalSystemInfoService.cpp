#include "system/LocalSystemInfoService.h"

#include <QDir>
#include <QSet>
#include <QStorageInfo>
#include <QStringList>
#include <QSysInfo>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_2.h>
#include <oleauto.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wbemidl.h>
#include <wrl/client.h>
#endif

namespace platform {
namespace {

#if defined(Q_OS_WIN)
std::wstring gpuPhysicalEngineKey(const wchar_t* instanceName)
{
    if (instanceName == nullptr || *instanceName == L'\0') {
        return {};
    }

    // =====wjy====
    const std::wstring name(instanceName); // wjy: GPU Engine 实例名包含进程、适配器 LUID、物理索引和引擎索引。
    const std::size_t luidPosition = name.find(L"_luid_"); // wjy: LUID 用于区分独立显卡、核显等不同 GPU 适配器。
    const std::size_t physicalPosition = name.find(L"_phys_", luidPosition); // wjy: phys 字段区分同一适配器下的物理节点。
    const std::size_t enginePosition = name.find(L"_eng_", physicalPosition); // wjy: eng 字段对应任务管理器统计的具体物理引擎。
    const std::size_t engineTypePosition = name.find(L"_engtype_", enginePosition); // wjy: engtype 只是引擎类型说明，不参与唯一标识。
    if (luidPosition == std::wstring::npos
        || physicalPosition == std::wstring::npos
        || enginePosition == std::wstring::npos
        || engineTypePosition == std::wstring::npos) {
        return {}; // wjy: 驱动若返回未知格式则交由调用方按完整实例名单独处理，避免错误合并。
    }
    return name.substr(
        luidPosition + 1,
        engineTypePosition - luidPosition - 1); // wjy: 返回 luid + phys + eng，故意去掉 pid，使同一引擎上的多个进程能够汇总。
    // ===end====
}

QString windowsProcessorModel()
{
    HKEY processorKey = nullptr;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0,
            KEY_READ,
            &processorKey) != ERROR_SUCCESS) {
        return {};
    }

    wchar_t buffer[256] = {};
    DWORD type = 0;
    DWORD byteCount = sizeof(buffer);
    const LONG result = RegQueryValueExW(
        processorKey,
        L"ProcessorNameString",
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(buffer),
        &byteCount);
    RegCloseKey(processorKey);
    if (result != ERROR_SUCCESS || type != REG_SZ) {
        return {};
    }
    return QString::fromWCharArray(buffer).simplified(); // wjy: 注册表字符串常含多余空格，展示前统一压缩为可读 CPU 型号。
}

// =====wjy====
class ScopedComApartment final {
public:
    ScopedComApartment()
        : m_result(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
        , m_shouldUninitialize(SUCCEEDED(m_result))
    {
    }

    ~ScopedComApartment()
    {
        if (m_shouldUninitialize) {
            CoUninitialize(); // wjy: 只平衡当前辅助查询自己成功增加的 COM 初始化引用，绝不撤销 Qt 或其它组件的 apartment。
        }
    }

    bool usable() const
    {
        return SUCCEEDED(m_result)
            || m_result == RPC_E_CHANGED_MODE; // wjy: UI 线程若已采用另一 apartment 模型仍可继续使用现有 COM 环境，避免把合法状态当成硬件不可用。
    }

private:
    HRESULT m_result = E_FAIL;
    bool m_shouldUninitialize = false;
};

bool configureComSecurity()
{
    const HRESULT result = CoInitializeSecurity(
        nullptr,
        -1,
        nullptr,
        nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        EOAC_NONE,
        nullptr);
    return SUCCEEDED(result)
        || result == RPC_E_TOO_LATE; // wjy: 进程级 COM 安全已由其它组件设置时直接复用，不能因“设置太晚”丢弃可读 WMI 数据。
}

Microsoft::WRL::ComPtr<IWbemServices> localWmiServices()
{
    Microsoft::WRL::ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(
            CLSID_WbemLocator,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(locator.GetAddressOf())))) {
        return {}; // wjy: WMI 组件缺失时返回空连接，调用方继续保留 CPU、占用率和磁盘等其它信息。
    }

    BSTR namespacePath = SysAllocString(L"ROOT\\CIMV2");
    if (namespacePath == nullptr) {
        return {};
    }
    Microsoft::WRL::ComPtr<IWbemServices> services;
    const HRESULT connectResult = locator->ConnectServer(
        namespacePath,
        nullptr,
        nullptr,
        nullptr,
        0,
        nullptr,
        nullptr,
        services.GetAddressOf());
    SysFreeString(namespacePath);
    if (FAILED(connectResult)) {
        return {};
    }

    if (FAILED(CoSetProxyBlanket(
            services.Get(),
            RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE,
            nullptr,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE))) {
        return {}; // wjy: 无法建立当前用户只读代理权限时不尝试提升权限，也不阻塞本机资源页面。
    }
    return services;
}

Microsoft::WRL::ComPtr<IEnumWbemClassObject> executeLocalWmiQuery(
    IWbemServices* services,
    const wchar_t* queryText)
{
    if (services == nullptr || queryText == nullptr) {
        return {};
    }
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(queryText);
    if (language == nullptr || query == nullptr) {
        if (language != nullptr) {
            SysFreeString(language);
        }
        if (query != nullptr) {
            SysFreeString(query);
        }
        return {};
    }

    Microsoft::WRL::ComPtr<IEnumWbemClassObject> enumerator;
    const HRESULT result = services->ExecQuery(
        language,
        query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        enumerator.GetAddressOf());
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(result)) {
        return {}; // wjy: 查询失败只清空本类结果，不让本机页静态刷新整体失败。
    }
    return enumerator;
}

bool variantUnsignedValue(const VARIANT& value, quint64* result)
{
    if (result == nullptr) {
        return false;
    }
    switch (value.vt) {
    case VT_UI1:
        *result = value.bVal;
        return true;
    case VT_UI2:
        *result = value.uiVal;
        return true;
    case VT_UI4:
        *result = value.ulVal;
        return true;
    case VT_UI8:
        *result = value.ullVal;
        return true;
    case VT_I1:
        if (value.cVal >= 0) {
            *result = static_cast<quint64>(value.cVal);
            return true;
        }
        return false;
    case VT_I2:
        if (value.iVal >= 0) {
            *result = static_cast<quint64>(value.iVal);
            return true;
        }
        return false;
    case VT_I4:
        if (value.lVal >= 0) {
            *result = static_cast<quint64>(value.lVal);
            return true;
        }
        return false;
    case VT_I8:
        if (value.llVal >= 0) {
            *result = static_cast<quint64>(value.llVal);
            return true;
        }
        return false;
    case VT_BSTR: {
        if (value.bstrVal == nullptr) {
            return false;
        }
        bool ok = false;
        const quint64 parsed = QString::fromWCharArray(value.bstrVal).trimmed().toULongLong(&ok);
        if (ok) {
            *result = parsed; // wjy: CIM_UINT64 常以 BSTR 返回，统一解析后再限制到目标字段范围。
        }
        return ok;
    }
    default:
        return false;
    }
}

quint64 unsignedWmiProperty(IWbemClassObject* object, const wchar_t* name)
{
    if (object == nullptr || name == nullptr) {
        return 0;
    }
    VARIANT value;
    VariantInit(&value);
    const HRESULT result = object->Get(name, 0, &value, nullptr, nullptr);
    quint64 parsed = 0;
    const bool available = SUCCEEDED(result) && variantUnsignedValue(value, &parsed);
    VariantClear(&value);
    return available ? parsed : 0; // wjy: 单个字段类型异常时仅返回未知值，其它内存条字段仍继续读取。
}

QString stringWmiProperty(IWbemClassObject* object, const wchar_t* name)
{
    if (object == nullptr || name == nullptr) {
        return {};
    }
    VARIANT value;
    VariantInit(&value);
    const HRESULT result = object->Get(name, 0, &value, nullptr, nullptr);
    const QString text = SUCCEEDED(result) && value.vt == VT_BSTR && value.bstrVal != nullptr
        ? QString::fromWCharArray(value.bstrVal).simplified()
        : QString();
    VariantClear(&value);
    return text; // wjy: 定位字段只用于稳定排序，空值不会影响内存条容量和数量统计。
}

QString smbiosMemoryTypeName(quint64 type)
{
    switch (type) {
    case 18:
        return QStringLiteral("DDR");
    case 19:
        return QStringLiteral("DDR2");
    case 20:
        return QStringLiteral("DDR2 FB-DIMM");
    case 24:
        return QStringLiteral("DDR3");
    case 26:
        return QStringLiteral("DDR4");
    case 27:
        return QStringLiteral("LPDDR");
    case 28:
        return QStringLiteral("LPDDR2");
    case 29:
        return QStringLiteral("LPDDR3");
    case 30:
        return QStringLiteral("LPDDR4");
    case 32:
        return QStringLiteral("HBM");
    case 33:
        return QStringLiteral("HBM2");
    case 34:
        return QStringLiteral("DDR5");
    case 35:
        return QStringLiteral("LPDDR5");
    case 36:
        return QStringLiteral("HBM3");
    default:
        return {}; // wjy: 主板返回保留值或旧 BIOS 未填充时保持未知，不把编号直接暴露给用户。
    }
}

LocalGpuInfo localGpuSummary()
{
    LocalGpuInfo selected;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        return selected;
    }

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumResult = factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf());
        if (enumResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(enumResult)) {
            continue;
        }

        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))
            || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue; // wjy: 排除 Microsoft Basic Render 等软件适配器，避免把回退设备当成用户显卡。
        }

        const QString adapterName = QString::fromWCharArray(description.Description).simplified();
        if (adapterName.isEmpty()) {
            continue;
        }
        const quint64 dedicatedBytes = static_cast<quint64>(description.DedicatedVideoMemory);
        if (selected.name.isEmpty()
            || dedicatedBytes > selected.dedicatedVideoMemoryBytes) {
            selected.name = adapterName; // wjy: 多显卡机器优先展示专用显存最大的硬件适配器，只有核显时仍保留首个可用设备。
            selected.dedicatedVideoMemoryBytes = dedicatedBytes;
        }
    }
    return selected;
}

void queryLocalMemoryHardware(
    QVector<LocalMemoryModuleInfo>* modules,
    int* totalSlotCount)
{
    if (modules == nullptr || totalSlotCount == nullptr) {
        return;
    }
    modules->clear();
    *totalSlotCount = 0;

    ScopedComApartment apartment;
    if (!apartment.usable() || !configureComSecurity()) {
        return;
    }
    Microsoft::WRL::ComPtr<IWbemServices> services = localWmiServices();
    if (services.Get() == nullptr) {
        return;
    }

    Microsoft::WRL::ComPtr<IEnumWbemClassObject> moduleEnumerator = executeLocalWmiQuery(
        services.Get(),
        L"SELECT Capacity, SMBIOSMemoryType, ConfiguredClockSpeed, DeviceLocator FROM Win32_PhysicalMemory");
    if (moduleEnumerator.Get() != nullptr) {
        constexpr int maximumModules = 64;
        for (int index = 0; index < maximumModules; ++index) {
            Microsoft::WRL::ComPtr<IWbemClassObject> object;
            ULONG returned = 0;
            const HRESULT nextResult = moduleEnumerator->Next(
                750,
                1,
                object.GetAddressOf(),
                &returned); // wjy: 每次等待最多 750ms，WMI 服务异常时本机页不会无限阻塞。
            if (nextResult == WBEM_S_TIMEDOUT || returned == 0) {
                break;
            }
            if (FAILED(nextResult)) {
                break;
            }

            LocalMemoryModuleInfo module;
            module.capacityBytes = unsignedWmiProperty(object.Get(), L"Capacity");
            if (module.capacityBytes == 0) {
                continue; // wjy: 只统计真正安装且报告有效容量的模块，空插槽由物理数组总数单独表示。
            }
            module.memoryType = smbiosMemoryTypeName(
                unsignedWmiProperty(object.Get(), L"SMBIOSMemoryType"));
            const quint64 configuredSpeed = unsignedWmiProperty(object.Get(), L"ConfiguredClockSpeed");
            module.configuredSpeedMtps = configuredSpeed <= static_cast<quint64>((std::numeric_limits<int>::max)())
                ? static_cast<int>(configuredSpeed)
                : 0; // wjy: 异常超大速率按未知处理，防止无符号值截断成负数。
            module.deviceLocator = stringWmiProperty(object.Get(), L"DeviceLocator");
            modules->append(std::move(module));
        }
    }

    std::sort(modules->begin(), modules->end(), [](const LocalMemoryModuleInfo& left, const LocalMemoryModuleInfo& right) {
        const int locatorOrder = left.deviceLocator.compare(right.deviceLocator, Qt::CaseInsensitive);
        if (locatorOrder != 0) {
            return locatorOrder < 0; // wjy: 优先按 DIMM/插槽定位稳定排列，刷新页面不会无故交换混合容量顺序。
        }
        return left.capacityBytes > right.capacityBytes;
    });

    Microsoft::WRL::ComPtr<IEnumWbemClassObject> arrayEnumerator = executeLocalWmiQuery(
        services.Get(),
        L"SELECT MemoryDevices FROM Win32_PhysicalMemoryArray WHERE Use = 3");
    if (arrayEnumerator.Get() != nullptr) {
        quint64 slotSum = 0;
        constexpr int maximumArrays = 16;
        for (int index = 0; index < maximumArrays; ++index) {
            Microsoft::WRL::ComPtr<IWbemClassObject> object;
            ULONG returned = 0;
            const HRESULT nextResult = arrayEnumerator->Next(750, 1, object.GetAddressOf(), &returned);
            if (nextResult == WBEM_S_TIMEDOUT || returned == 0) {
                break;
            }
            if (FAILED(nextResult)) {
                break;
            }
            slotSum += unsignedWmiProperty(object.Get(), L"MemoryDevices"); // wjy: 多路主板可能公开多个系统内存数组，物理插槽总数按数组相加。
        }
        if (slotSum <= static_cast<quint64>((std::numeric_limits<int>::max)())) {
            *totalSlotCount = static_cast<int>(slotSum);
        }
    }
    if (*totalSlotCount > 0) {
        *totalSlotCount = qMax(
            *totalSlotCount,
            static_cast<int>(modules->size())); // wjy: 固件若报告的总插槽小于已安装条数，至少使用可信的模块数量避免显示“2 / 1”。
    }
}
// ===end====
#endif

// =====wjy====
QString hardwareCapacityText(quint64 bytes)
{
    if (bytes == 0) {
        return QStringLiteral("--");
    }
    constexpr double bytesPerGiB = 1024.0 * 1024.0 * 1024.0;
    const double gibibytes = bytes / bytesPerGiB;
    const qint64 rounded = qRound64(gibibytes);
    return std::abs(gibibytes - static_cast<double>(rounded)) < 0.05
        ? QStringLiteral("%1 GB").arg(rounded)
        : QStringLiteral("%1 GB").arg(gibibytes, 0, 'f', 1); // wjy: 标称 8/16/32 GB 内存条使用整数，非整容量仍保留一位小数。
}
// ===end====

// =====wjy====
QVector<LocalDiskInfo> localDiskList()
{
    QVector<LocalDiskInfo> disks;
#if defined(Q_OS_WIN)
    const DWORD requiredCharacters = GetLogicalDriveStringsW(0, nullptr); // wjy: 先取得所有逻辑盘符所需缓冲区长度，盘符数量不再写死为一个系统盘。
    if (requiredCharacters == 0) {
        return disks;
    }

    std::vector<wchar_t> driveBuffer(static_cast<std::size_t>(requiredCharacters) + 1, L'\0');
    if (GetLogicalDriveStringsW(static_cast<DWORD>(driveBuffer.size()), driveBuffer.data()) == 0) {
        return disks; // wjy: 枚举失败时返回空列表，界面显示统一占位且其它系统信息继续可用。
    }

    const wchar_t* driveRoot = driveBuffer.data();
    while (*driveRoot != L'\0') {
        const std::wstring rootText(driveRoot); // wjy: Windows 返回以双空字符结束的 C:\、D:\ 序列，每次推进一个完整根路径。
        const UINT driveType = GetDriveTypeW(driveRoot);
        if (driveType == DRIVE_FIXED || driveType == DRIVE_REMOVABLE) {
            ULARGE_INTEGER availableBytes{}; // wjy: Windows API 要求接收当前调用用户可用容量；实际占用仍使用卷总剩余容量，避免配额影响显示。
            ULARGE_INTEGER totalBytes{};
            ULARGE_INTEGER freeBytes{};
            if (GetDiskFreeSpaceExW(driveRoot, &availableBytes, &totalBytes, &freeBytes)
                && totalBytes.QuadPart > 0) {
                LocalDiskInfo disk;
                disk.rootPath = QDir::toNativeSeparators(QString::fromWCharArray(driveRoot)).trimmed(); // wjy: 只展示当前可读取容量的本地固定盘和可移动盘，排除网络映射盘及无介质光驱。
                disk.totalBytes = static_cast<quint64>(totalBytes.QuadPart);
                disk.usedBytes = disk.totalBytes - qMin(
                    disk.totalBytes,
                    static_cast<quint64>(freeBytes.QuadPart)); // wjy: 使用卷总剩余容量计算占用，并限制异常系统返回值避免无符号下溢。
                disks.append(std::move(disk));
            }
        }
        driveRoot += rootText.size() + 1;
    }
#else
    const QList<QStorageInfo> mountedVolumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo& storage : mountedVolumes) {
        if (!storage.isValid() || !storage.isReady() || storage.bytesTotal() <= 0) {
            continue;
        }
        LocalDiskInfo disk;
        disk.rootPath = QDir::toNativeSeparators(storage.rootPath()).trimmed();
        disk.totalBytes = static_cast<quint64>(storage.bytesTotal());
        disk.usedBytes = disk.totalBytes - qMin(
            disk.totalBytes,
            static_cast<quint64>(qMax<qint64>(0, storage.bytesFree()))); // wjy: 非 Windows 平台按已挂载且就绪的卷回退枚举，仍保持同一列表数据结构。
        disks.append(std::move(disk));
    }
#endif
    std::sort(disks.begin(), disks.end(), [](const LocalDiskInfo& left, const LocalDiskInfo& right) {
        return left.rootPath.compare(right.rootPath, Qt::CaseInsensitive) < 0; // wjy: 按盘符或挂载点稳定排序，刷新本机页时列表不会无故交换位置。
    });
    return disks;
}
// ===end====

} // namespace

// =====wjy====
LocalSystemInfo LocalSystemInfoService::local()
{
    LocalSystemInfo info;
    info.cpuArchitecture = QSysInfo::currentCpuArchitecture().trimmed();
    info.logicalProcessorCount = qMax(0, QThread::idealThreadCount());

#if defined(Q_OS_WIN)
    info.cpuModel = windowsProcessorModel();
    info.gpu = localGpuSummary(); // wjy: GPU 名称和专用显存只在静态刷新时枚举一次，不加入一秒资源采样路径。
    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus)) {
        info.totalPhysicalMemoryBytes = memoryStatus.ullTotalPhys; // wjy: 只读取物理内存总量，不采集进程内存或写入任何持久化状态。
    }
    queryLocalMemoryHardware(
        &info.memoryModules,
        &info.totalMemorySlotCount); // wjy: WMI 失败时保留空列表，界面仍能显示总容量、实时占用和磁盘。
#endif

    if (info.cpuModel.isEmpty()) {
        info.cpuModel = info.cpuArchitecture; // wjy: 注册表字段缺失或非 Windows 时至少显示 CPU 架构，页面不会留下空白。
    }

    info.disks = localDisks(); // wjy: 首次静态快照和后续低频容量刷新共用同一个公开磁盘采集入口。
    return info;
}

QVector<LocalDiskInfo> LocalSystemInfoService::localDisks()
{
    return localDiskList(); // wjy: 只枚举本地固定盘和可移动盘的总量/剩余量，不触发其它硬件信息查询。
}

QString LocalSystemInfoService::memoryCapacityComposition(const QVector<LocalMemoryModuleInfo>& modules)
{
    QVector<quint64> capacities;
    capacities.reserve(modules.size());
    for (const LocalMemoryModuleInfo& module : modules) {
        if (module.capacityBytes > 0) {
            capacities.append(module.capacityBytes); // wjy: 跳过缺失容量的异常对象，安装数量和组合只由可用物理模块构成。
        }
    }
    if (capacities.isEmpty()) {
        return QStringLiteral("--");
    }

    const bool allEqual = std::all_of(
        capacities.cbegin(),
        capacities.cend(),
        [firstCapacity = capacities.first()](quint64 capacity) {
            return capacity == firstCapacity;
        });
    if (allEqual) {
        return capacities.size() > 1
            ? QStringLiteral("%1 × %2").arg(capacities.size()).arg(hardwareCapacityText(capacities.first()))
            : hardwareCapacityText(capacities.first()); // wjy: 相同容量直接压缩为“2 × 16 GB”，单条内存不添加多余乘号。
    }

    QStringList capacityTexts;
    capacityTexts.reserve(capacities.size());
    for (const quint64 capacity : capacities) {
        capacityTexts.append(hardwareCapacityText(capacity));
    }
    return capacityTexts.join(QStringLiteral(" + ")); // wjy: 混合容量按稳定插槽顺序逐条展示，避免误导为相同规格套条。
}

QString LocalSystemInfoService::memoryTypeSummary(const QVector<LocalMemoryModuleInfo>& modules)
{
    QSet<QString> types;
    for (const LocalMemoryModuleInfo& module : modules) {
        const QString type = module.memoryType.trimmed();
        if (!type.isEmpty()) {
            types.insert(type);
        }
    }
    if (types.isEmpty()) {
        return QStringLiteral("--");
    }
    return types.size() == 1
        ? *types.cbegin()
        : QString::fromUtf8("混合"); // wjy: 任意两条类型不同即明确显示混合，不任意挑选一个类型代表整机。
}

QString LocalSystemInfoService::memorySpeedSummary(const QVector<LocalMemoryModuleInfo>& modules)
{
    QSet<int> speeds;
    for (const LocalMemoryModuleInfo& module : modules) {
        if (module.configuredSpeedMtps > 0) {
            speeds.insert(module.configuredSpeedMtps);
        }
    }
    if (speeds.isEmpty()) {
        return QStringLiteral("--");
    }
    return speeds.size() == 1
        ? QStringLiteral("%1 MT/s").arg(*speeds.cbegin())
        : QString::fromUtf8("混合速率"); // wjy: 配置速率不同的混插内存不显示虚假的单一 MT/s。
}

int CpuUsageSampler::samplePercent()
{
#if defined(Q_OS_WIN)
    if (m_queryHandle == 0 || m_counterHandle == 0) {
        PDH_HQUERY query = nullptr;
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            return -1; // wjy: PDH 不可用时保留占位，不能把采样失败误显示为 CPU 空闲。
        }

        PDH_HCOUNTER counter = nullptr;
        const PDH_STATUS addStatus = PdhAddEnglishCounterW(
            query,
            L"\\Processor Information(_Total)\\% Processor Utility",
            0,
            &counter); // wjy: 使用任务管理器 CPU 百分比所依据的 Processor Utility，并用英文路径避免系统语言差异。
        if (addStatus != ERROR_SUCCESS) {
            PdhCloseQuery(query);
            return -1; // wjy: 系统未提供该计数器时安全降级为不可用，不退回到不同口径造成误导。
        }

        m_queryHandle = reinterpret_cast<quintptr>(query); // wjy: 保存查询句柄，后续每秒只采集数据，不重复创建性能计数器。
        m_counterHandle = reinterpret_cast<quintptr>(counter);
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
            reset();
            return -1;
        }
        return -1; // wjy: 速率型计数器第一次采集只建立基线，第二个样本才有与任务管理器可比的百分比。
    }

    const PDH_HQUERY query = reinterpret_cast<PDH_HQUERY>(m_queryHandle);
    const PDH_HCOUNTER counter = reinterpret_cast<PDH_HCOUNTER>(m_counterHandle);
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
        reset();
        return -1; // wjy: 查询失效时立即释放旧句柄，下一轮允许重新建立，避免长期卡在无效状态。
    }

    PDH_FMT_COUNTERVALUE value{};
    if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS
        || (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA)
        || !std::isfinite(value.doubleValue)) {
        return -1; // wjy: 跳过无效或非有限数据，不让异常计数污染圆环动画。
    }
    return qBound(0, qRound(value.doubleValue), 100); // wjy: Processor Utility 可因睿频超过 100，任务管理器界面口径需要限制在 0-100。
#else
    return -1;
#endif
}

CpuUsageSampler::~CpuUsageSampler()
{
    reset(); // wjy: CPU 查询只属于当前采样器，析构统一释放 PDH 查询和附属计数器。
}

void CpuUsageSampler::reset()
{
#if defined(Q_OS_WIN)
    if (m_queryHandle != 0) {
        PdhCloseQuery(reinterpret_cast<PDH_HQUERY>(m_queryHandle)); // wjy: 关闭查询会同时释放 Processor Utility 计数器。
    }
#endif
    m_queryHandle = 0;
    m_counterHandle = 0; // wjy: 清零句柄使下次进入本机页时重新建立采样基线。
}

GpuUsageSampler::~GpuUsageSampler()
{
    reset(); // wjy: GPU 查询只属于当前采样器，析构统一走 reset 关闭句柄。
}

int GpuUsageSampler::samplePercent()
{
#if defined(Q_OS_WIN)
    if (m_queryHandle == 0 || m_counterHandle == 0) {
        PDH_HQUERY query = nullptr;
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            return -1; // wjy: PDH 服务不可用时保持占位，不影响 CPU 和内存采样。
        }

        PDH_HCOUNTER counter = nullptr;
        const PDH_STATUS addStatus = PdhAddEnglishCounterW(
            query,
            L"\\GPU Engine(*)\\Utilization Percentage",
            0,
            &counter); // wjy: 使用英文通配符计数器跨系统语言读取全部 GPU 引擎实例。
        if (addStatus != ERROR_SUCCESS) {
            PdhCloseQuery(query);
            return -1; // wjy: 旧系统或驱动未提供 GPU Engine 计数器时安全降级为不可用。
        }

        m_queryHandle = reinterpret_cast<quintptr>(query); // wjy: 成功创建后保存查询句柄，后续每秒只采集数据而不重复创建对象。
        m_counterHandle = reinterpret_cast<quintptr>(counter);
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
            reset();
            return -1;
        }
        return -1; // wjy: 速率型 GPU 计数器首次采集只建立基线，下一秒才有可用占用值。
    }

    const PDH_HQUERY query = reinterpret_cast<PDH_HQUERY>(m_queryHandle);
    const PDH_HCOUNTER counter = reinterpret_cast<PDH_HCOUNTER>(m_counterHandle);
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
        reset();
        return -1; // wjy: 查询失效时立即关闭旧句柄，下一轮允许重新建立而不是永久停住。
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(
        counter,
        PDH_FMT_DOUBLE,
        &bufferSize,
        &itemCount,
        nullptr); // wjy: 第一次调用只询问包含全部通配符实例所需的缓冲区大小。
    if (status == PDH_CSTATUS_NO_INSTANCE) {
        return 0; // wjy: 计数器存在但当前没有活动 GPU 引擎实例时按空闲显示 0%。
    }
    if (status != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0) {
        return -1;
    }

    const std::size_t storageCount =
        (static_cast<std::size_t>(bufferSize) + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    std::vector<std::max_align_t> storage(storageCount); // wjy: 使用最大对齐存储承接 PDH 的条目数组和尾随实例名，避免未对齐转换。
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
    status = PdhGetFormattedCounterArrayW(
        counter,
        PDH_FMT_DOUBLE,
        &bufferSize,
        &itemCount,
        items);
    if (status == PDH_CSTATUS_NO_INSTANCE) {
        return 0;
    }
    if (status != ERROR_SUCCESS) {
        return -1;
    }

    std::unordered_map<std::wstring, double> physicalEnginePercents; // wjy: 以 luid + phys + eng 为键，汇总同一物理引擎上所有进程实例。
    for (DWORD index = 0; index < itemCount; ++index) {
        const PDH_FMT_COUNTERVALUE& value = items[index].FmtValue;
        if ((value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA)
            || !std::isfinite(value.doubleValue)) {
            continue; // wjy: 跳过刚失效或驱动未填充的引擎实例，避免单个坏值污染整张 GPU 卡片。
        }

        std::wstring engineKey = gpuPhysicalEngineKey(items[index].szName); // wjy: 去掉 pid 后，相同 GPU 0 - 3D 等物理引擎会得到相同键。
        if (engineKey.empty() && items[index].szName != nullptr) {
            engineKey = std::wstring(L"instance:") + items[index].szName; // wjy: 未知驱动格式按完整实例名隔离，保留读数但绝不错误合并。
        }
        if (engineKey.empty()) {
            continue;
        }
        physicalEnginePercents[engineKey] += (std::max)(0.0, value.doubleValue); // wjy: 同一物理引擎跨进程求和，修正原先只显示最高单进程占用的问题。
    }

    double busiestEnginePercent = 0.0;
    for (const auto& engine : physicalEnginePercents) {
        busiestEnginePercent = (std::max)(busiestEnginePercent, engine.second); // wjy: 任务管理器用最繁忙物理引擎代表整体 GPU 占用。
    }
    return !physicalEnginePercents.empty()
        ? qBound(0, qRound(busiestEnginePercent), 100) // wjy: 多个上下文相加可能略超 100，最终按界面百分比范围截断。
        : -1;
#else
    return -1;
#endif
}

void GpuUsageSampler::reset()
{
#if defined(Q_OS_WIN)
    if (m_queryHandle != 0) {
        PdhCloseQuery(reinterpret_cast<PDH_HQUERY>(m_queryHandle)); // wjy: 关闭查询会一并释放其 GPU 通配符计数器。
    }
#endif
    m_queryHandle = 0;
    m_counterHandle = 0;
}

LocalMemoryUsage MemoryUsageSampler::sample() const
{
    LocalMemoryUsage sample;
#if defined(Q_OS_WIN)
    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (!GlobalMemoryStatusEx(&memoryStatus)) {
        return sample;
    }
    sample.percent = qBound(
        0,
        static_cast<int>(memoryStatus.dwMemoryLoad),
        100); // wjy: Windows 直接提供当前物理内存负载百分比，限制后交给环形动画显示。
    sample.totalBytes = static_cast<quint64>(memoryStatus.ullTotalPhys);
    sample.availableBytes = qMin(
        sample.totalBytes,
        static_cast<quint64>(memoryStatus.ullAvailPhys)); // wjy: 限制可用容量不超过总量，防止异常系统快照造成无符号下溢。
    sample.usedBytes = sample.totalBytes - sample.availableBytes;
#endif
    return sample;
}

int MemoryUsageSampler::samplePercent() const
{
    return sample().percent; // wjy: 旧调用者仍只取百分比，但底层与详细内存容量使用同一采样逻辑。
}
// ===end====

} // namespace platform
