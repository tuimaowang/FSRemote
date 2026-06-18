#include "system/WolDetector.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wbemidl.h>
#endif

#include <algorithm>
#include <cwctype>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace platform {
namespace {

#if defined(_WIN32)

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* get() const { return ptr_; }
    T** put()
    {
        reset();
        return &ptr_;
    }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void reset()
    {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

std::string narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

std::wstring wqlQuote(const std::wstring& value)
{
    std::wstring escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back(L'\'');
    for (wchar_t ch : value) {
        if (ch == L'\'') escaped.push_back(L'\'');
        escaped.push_back(ch);
    }
    escaped.push_back(L'\'');
    return escaped;
}

std::wstring lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

std::string hresultText(HRESULT hr)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << static_cast<unsigned long>(hr);
    return stream.str();
}

bool isAccessDenied(HRESULT hr)
{
    return hr == E_ACCESSDENIED
        || hr == WBEM_E_ACCESS_DENIED
        || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
}

void addDiagnostic(WolSetting& setting, const std::string& text)
{
    setting.diagnostics.push_back(text);
}

std::wstring variantToString(const VARIANT& value)
{
    if ((value.vt & VT_ARRAY) != 0) {
        SAFEARRAY* array = value.parray;
        if (!array) return {};
        LONG lowerBound = 0;
        LONG upperBound = -1;
        SafeArrayGetLBound(array, 1, &lowerBound);
        SafeArrayGetUBound(array, 1, &upperBound);
        std::wstring joined;
        for (LONG i = lowerBound; i <= upperBound; ++i) {
            VARIANT item;
            VariantInit(&item);
            if (SUCCEEDED(SafeArrayGetElement(array, &i, &item))) {
                if (!joined.empty()) joined += L",";
                joined += variantToString(item);
            }
            VariantClear(&item);
        }
        return joined;
    }

    switch (value.vt) {
    case VT_BSTR:
        return value.bstrVal ? std::wstring(value.bstrVal) : std::wstring();
    case VT_BOOL:
        return value.boolVal == VARIANT_TRUE ? L"true" : L"false";
    case VT_I1:
        return std::to_wstring(value.cVal);
    case VT_UI1:
        return std::to_wstring(value.bVal);
    case VT_I2:
        return std::to_wstring(value.iVal);
    case VT_UI2:
        return std::to_wstring(value.uiVal);
    case VT_I4:
    case VT_INT:
        return std::to_wstring(value.lVal);
    case VT_UI4:
    case VT_UINT:
        return std::to_wstring(value.ulVal);
    case VT_I8:
        return std::to_wstring(value.llVal);
    case VT_UI8:
        return std::to_wstring(value.ullVal);
    case VT_NULL:
    case VT_EMPTY:
        return {};
    default: {
        VARIANT copy;
        VariantInit(&copy);
        if (SUCCEEDED(VariantChangeType(&copy, const_cast<VARIANT*>(&value), 0, VT_BSTR))) {
            std::wstring result = copy.bstrVal ? std::wstring(copy.bstrVal) : std::wstring();
            VariantClear(&copy);
            return result;
        }
        return {};
    }
    }
}

std::vector<std::wstring> variantToStringArray(const VARIANT& value)
{
    std::vector<std::wstring> values;
    if ((value.vt & VT_ARRAY) == 0) {
        const std::wstring single = variantToString(value);
        if (!single.empty()) values.push_back(single);
        return values;
    }

    SAFEARRAY* array = value.parray;
    if (!array) return values;

    LONG lowerBound = 0;
    LONG upperBound = -1;
    SafeArrayGetLBound(array, 1, &lowerBound);
    SafeArrayGetUBound(array, 1, &upperBound);
    for (LONG i = lowerBound; i <= upperBound; ++i) {
        BSTR item = nullptr;
        if (SUCCEEDED(SafeArrayGetElement(array, &i, &item))) {
            values.emplace_back(item ? std::wstring(item) : std::wstring());
            SysFreeString(item);
        }
    }
    return values;
}

bool isEnabledValue(const std::wstring& value)
{
    const std::wstring normalized = lower(value);
    return normalized == L"enabled"
        || normalized == L"enable"
        || normalized == L"true"
        || normalized == L"1";
}

bool isDisabledValue(const std::wstring& value)
{
    const std::wstring normalized = lower(value);
    return normalized == L"disabled"
        || normalized == L"disable"
        || normalized == L"false"
        || normalized == L"0";
}

std::optional<unsigned long long> variantUnsignedInt(const VARIANT& value)
{
    switch (value.vt) {
    case VT_UI1:
        return value.bVal;
    case VT_UI2:
        return value.uiVal;
    case VT_UI4:
    case VT_UINT:
        return value.ulVal;
    case VT_UI8:
        return value.ullVal;
    case VT_I1:
        return value.cVal >= 0 ? std::optional<unsigned long long>(static_cast<unsigned long long>(value.cVal)) : std::nullopt;
    case VT_I2:
        return value.iVal >= 0 ? std::optional<unsigned long long>(static_cast<unsigned long long>(value.iVal)) : std::nullopt;
    case VT_I4:
    case VT_INT:
        return value.lVal >= 0 ? std::optional<unsigned long long>(static_cast<unsigned long long>(value.lVal)) : std::nullopt;
    case VT_I8:
        return value.llVal >= 0 ? std::optional<unsigned long long>(static_cast<unsigned long long>(value.llVal)) : std::nullopt;
    default:
        break;
    }
    return std::nullopt;
}

bool netAdapterPowerSettingSupported(const VARIANT& value)
{
    if (const auto number = variantUnsignedInt(value)) {
        return *number == 1 || *number == 2;
    }
    const std::wstring text = variantToString(value);
    return isEnabledValue(text) || isDisabledValue(text);
}

bool netAdapterPowerSettingEnabled(const VARIANT& value, bool fallback)
{
    if (const auto number = variantUnsignedInt(value)) {
        if (*number == 2) return true;
        if (*number == 1 || *number == 0) return false;
    }
    const std::wstring text = variantToString(value);
    if (isEnabledValue(text)) return true;
    if (isDisabledValue(text)) return false;
    return fallback;
}

bool getProperty(IWbemClassObject* object, const wchar_t* name, VARIANT* value)
{
    VariantInit(value);
    return object && SUCCEEDED(object->Get(name, 0, value, nullptr, nullptr));
}

bool putBoolProperty(IWbemClassObject* object, const wchar_t* name, bool value)
{
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_BOOL;
    variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    const HRESULT hr = object->Put(name, 0, &variant, 0);
    VariantClear(&variant);
    return SUCCEEDED(hr);
}

bool putStringProperty(IWbemClassObject* object, const wchar_t* name, const std::wstring& value)
{
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_BSTR;
    variant.bstrVal = SysAllocString(value.c_str());
    const HRESULT hr = object->Put(name, 0, &variant, 0);
    VariantClear(&variant);
    return SUCCEEDED(hr);
}

bool putStringArrayProperty(IWbemClassObject* object, const wchar_t* name, const std::vector<std::wstring>& values)
{
    SAFEARRAY* array = SafeArrayCreateVector(VT_BSTR, 0, static_cast<ULONG>(values.size()));
    if (!array) return false;

    for (LONG i = 0; i < static_cast<LONG>(values.size()); ++i) {
        BSTR item = SysAllocString(values[static_cast<size_t>(i)].c_str());
        HRESULT hr = SafeArrayPutElement(array, &i, item);
        SysFreeString(item);
        if (FAILED(hr)) {
            SafeArrayDestroy(array);
            return false;
        }
    }

    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_ARRAY | VT_BSTR;
    variant.parray = array;
    const HRESULT hr = object->Put(name, 0, &variant, 0);
    VariantClear(&variant);
    return SUCCEEDED(hr);
}

class ComApartment {
public:
    explicit ComApartment(WolSetting& setting)
    {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
        if (!initialized_) {
            addDiagnostic(setting, "failed: CoInitializeEx " + hresultText(hr_));
            return;
        }

        const HRESULT security = CoInitializeSecurity(
            nullptr,
            -1,
            nullptr,
            nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE,
            nullptr);
        if (FAILED(security) && security != RPC_E_TOO_LATE) {
            addDiagnostic(setting, "warning: CoInitializeSecurity " + hresultText(security));
        }
    }

    ~ComApartment()
    {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }

    bool ok() const { return initialized_; }

private:
    HRESULT hr_ = E_FAIL;
    bool initialized_ = false;
};

class WmiConnection {
public:
    WmiConnection(const wchar_t* nameSpace, WolSetting& setting)
    {
        ComPtr<IWbemLocator> locator;
        HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(locator.put()));
        if (FAILED(hr)) {
            addDiagnostic(setting, "failed: CoCreateInstance IWbemLocator " + hresultText(hr));
            return;
        }

        BSTR ns = SysAllocString(nameSpace);
        hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, services_.put());
        SysFreeString(ns);
        if (FAILED(hr)) {
            addDiagnostic(setting, "failed: ConnectServer " + narrow(nameSpace) + " " + hresultText(hr));
            return;
        }

        hr = CoSetProxyBlanket(
            services_.get(),
            RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE,
            nullptr,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE);
        if (FAILED(hr)) {
            addDiagnostic(setting, "warning: CoSetProxyBlanket " + narrow(nameSpace) + " " + hresultText(hr));
        }
    }

    bool ok() const { return services_.get() != nullptr; }
    IWbemServices* services() const { return services_.get(); }

    std::vector<ComPtr<IWbemClassObject>> query(const std::wstring& wql, WolSetting& setting, int timeoutMs = 3500)
    {
        addDiagnostic(setting, "wql: " + narrow(wql));
        std::vector<ComPtr<IWbemClassObject>> objects;
        if (!services_) return objects;

        BSTR language = SysAllocString(L"WQL");
        BSTR queryText = SysAllocString(wql.c_str());
        ComPtr<IEnumWbemClassObject> enumerator;
        const HRESULT hr = services_->ExecQuery(
            language,
            queryText,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr,
            enumerator.put());
        SysFreeString(language);
        SysFreeString(queryText);
        if (FAILED(hr)) {
            addDiagnostic(setting, "failed: ExecQuery " + hresultText(hr));
            return objects;
        }

        while (enumerator) {
            ComPtr<IWbemClassObject> object;
            ULONG returned = 0;
            const HRESULT next = enumerator->Next(timeoutMs, 1, object.put(), &returned);
            if (next == WBEM_S_TIMEDOUT) {
                addDiagnostic(setting, "failed: WMI query timeout");
                break;
            }
            if (FAILED(next) || returned == 0) break;
            objects.push_back(std::move(object));
        }
        addDiagnostic(setting, "wql rows: " + std::to_string(objects.size()));
        return objects;
    }

private:
    ComPtr<IWbemServices> services_;
};

struct AdapterInfo {
    std::wstring description;
    bool cableConnected = false;
    bool hasIp = false;
};

struct AdvancedPropertyValue {
    bool found = false;
    std::wstring displayValue;
    std::wstring registryValue;
};

std::vector<AdapterInfo> ethernetAdapters(WolSetting& setting)
{
    ULONG size = 15 * 1024;
    std::vector<unsigned char> buffer(size);
    ULONG ret = GetAdaptersAddresses(
        AF_UNSPEC,
        GAA_FLAG_INCLUDE_PREFIX,
        nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
        &size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        ret = GetAdaptersAddresses(
            AF_UNSPEC,
            GAA_FLAG_INCLUDE_PREFIX,
            nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
            &size);
    }
    if (ret != NO_ERROR) {
        addDiagnostic(setting, "failed: GetAdaptersAddresses " + std::to_string(ret));
        return {};
    }

    std::vector<AdapterInfo> adapters;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter;
         adapter = adapter->Next) {
        if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD) continue;
        if (adapter->PhysicalAddressLength == 0) continue;
        if (!adapter->Description || !*adapter->Description) continue;

        MIB_IF_ROW2 row = {};
        row.InterfaceIndex = adapter->IfIndex;
        if (GetIfEntry2(&row) != NO_ERROR) {
            addDiagnostic(setting, "warning: GetIfEntry2 failed for " + narrow(adapter->Description));
            continue;
        }
        if (!row.InterfaceAndOperStatusFlags.HardwareInterface) {
            continue;
        }

        AdapterInfo info;
        info.description = adapter->Description;
        info.cableConnected = row.MediaConnectState == MediaConnectStateConnected;
        for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
            if (address->Address.lpSockaddr && address->Address.lpSockaddr->sa_family == AF_INET) {
                info.hasIp = true;
                break;
            }
        }
        adapters.push_back(info);
        addDiagnostic(setting, "adapter: " + narrow(info.description)
            + " up=" + (info.cableConnected ? "true" : "false")
            + " hasIp=" + (info.hasIp ? "true" : "false"));
    }
    return adapters;
}

AdapterInfo chooseAdapter(const std::vector<AdapterInfo>& adapters, WolSetting& setting)
{
    for (const AdapterInfo& adapter : adapters) {
        if (adapter.cableConnected && adapter.hasIp) return adapter;
    }
    for (const AdapterInfo& adapter : adapters) {
        if (adapter.cableConnected) return adapter;
    }
    if (!adapters.empty()) {
        addDiagnostic(setting, "fallback: no active ethernet adapter; using first physical ethernet adapter for basic wol_supported detection");
        return adapters.front();
    }
    return {};
}

std::wstring objectPath(IWbemClassObject* object)
{
    VARIANT value;
    if (!getProperty(object, L"__PATH", &value)) return {};
    std::wstring path = variantToString(value);
    VariantClear(&value);
    return path;
}

std::vector<std::wstring> propertyStringArray(IWbemClassObject* object, const wchar_t* property)
{
    VARIANT value;
    if (!getProperty(object, property, &value)) return {};
    std::vector<std::wstring> values = variantToStringArray(value);
    VariantClear(&value);
    return values;
}

std::optional<std::wstring> chooseEnabledDisplayValue(const std::vector<std::wstring>& validValues)
{
    for (const std::wstring& value : validValues) {
        if (isEnabledValue(value)) return value;
    }
    for (const std::wstring& value : validValues) {
        const std::wstring normalized = lower(value);
        if (normalized == L"on" || normalized == L"yes" || normalized.find(L"enable") != std::wstring::npos) {
            return value;
        }
    }
    return std::nullopt;
}

std::vector<std::wstring> chooseEnabledRegistryValue(const std::vector<std::wstring>& validValues)
{
    for (const std::wstring& value : validValues) {
        if (isEnabledValue(value)) return {value};
    }
    for (const std::wstring& value : validValues) {
        const std::wstring normalized = lower(value);
        if (normalized == L"on" || normalized == L"yes" || normalized.find(L"enable") != std::wstring::npos) {
            return {value};
        }
    }
    return {L"1"};
}

bool evaluateEnabledOrDisabled(const std::wstring& primary, const std::wstring& secondary, bool fallback, bool* recognized)
{
    const bool primaryEnabled = isEnabledValue(primary);
    const bool secondaryEnabled = isEnabledValue(secondary);
    const bool primaryDisabled = isDisabledValue(primary);
    const bool secondaryDisabled = isDisabledValue(secondary);
    *recognized = primaryEnabled || secondaryEnabled || primaryDisabled || secondaryDisabled;
    if (primaryEnabled || secondaryEnabled) return true;
    if (primaryDisabled || secondaryDisabled) return false;
    return fallback;
}

bool putInstance(WmiConnection& connection, IWbemClassObject* object, WolSetting& setting, bool* permissionDenied)
{
    if (!connection.services() || !object) return false;
    const std::wstring path = objectPath(object);
    addDiagnostic(setting, "put: " + narrow(path));
    const HRESULT hr = connection.services()->PutInstance(object, WBEM_FLAG_UPDATE_ONLY, nullptr, nullptr);
    if (FAILED(hr)) {
        addDiagnostic(setting, "failed: PutInstance " + hresultText(hr));
        if (isAccessDenied(hr)) *permissionDenied = true;
        return false;
    }
    return true;
}

bool invokePowerManagementMethod(
    WmiConnection& connection,
    IWbemClassObject* instance,
    const std::wstring& methodName,
    WolSetting& setting,
    bool* permissionDenied)
{
    if (!connection.services() || !instance) return false;

    ComPtr<IWbemClassObject> classObject;
    BSTR className = SysAllocString(L"MSFT_NetAdapterPowerManagementSettingData");
    HRESULT hr = connection.services()->GetObject(className, 0, nullptr, classObject.put(), nullptr);
    SysFreeString(className);
    if (FAILED(hr)) {
        addDiagnostic(setting, "failed: GetObject MSFT_NetAdapterPowerManagementSettingData " + hresultText(hr));
        if (isAccessDenied(hr)) *permissionDenied = true;
        return false;
    }

    ComPtr<IWbemClassObject> inSignature;
    BSTR method = SysAllocString(methodName.c_str());
    hr = classObject->GetMethod(method, 0, inSignature.put(), nullptr);
    SysFreeString(method);
    if (FAILED(hr)) {
        addDiagnostic(setting, "failed: GetMethod " + narrow(methodName) + " " + hresultText(hr));
        return false;
    }

    ComPtr<IWbemClassObject> inParams;
    hr = inSignature->SpawnInstance(0, inParams.put());
    if (FAILED(hr)) {
        addDiagnostic(setting, "failed: SpawnInstance " + narrow(methodName) + " " + hresultText(hr));
        return false;
    }

    if (!putBoolProperty(inParams.get(), L"WakeOnMagicPacket", true)
        || !putBoolProperty(inParams.get(), L"WakeOnPattern", true)) {
        addDiagnostic(setting, "failed: unable to build " + narrow(methodName) + " input parameters");
        return false;
    }

    const std::wstring path = objectPath(instance);
    addDiagnostic(setting, "method: " + narrow(methodName) + " " + narrow(path));

    ComPtr<IWbemClassObject> outParams;
    BSTR objectPathValue = SysAllocString(path.c_str());
    BSTR methodValue = SysAllocString(methodName.c_str());
    hr = connection.services()->ExecMethod(
        objectPathValue,
        methodValue,
        0,
        nullptr,
        inParams.get(),
        outParams.put(),
        nullptr);
    SysFreeString(objectPathValue);
    SysFreeString(methodValue);
    if (FAILED(hr)) {
        addDiagnostic(setting, "failed: ExecMethod " + narrow(methodName) + " " + hresultText(hr));
        if (isAccessDenied(hr)) *permissionDenied = true;
        return false;
    }

    VARIANT returnValue;
    if (getProperty(outParams.get(), L"ReturnValue", &returnValue)) {
        const std::wstring text = variantToString(returnValue);
        addDiagnostic(setting, "method " + narrow(methodName) + " ReturnValue: " + narrow(text));
        const auto number = variantUnsignedInt(returnValue);
        VariantClear(&returnValue);
        if (number.has_value() && *number != 0) {
            return false;
        }
    }

    return true;
}

std::wstring firstProperty(WmiConnection& connection, const std::wstring& wql, const wchar_t* property, WolSetting& setting)
{
    auto rows = connection.query(wql, setting);
    if (rows.empty()) return {};
    VARIANT value;
    if (!getProperty(rows.front().get(), property, &value)) return {};
    std::wstring result = variantToString(value);
    VariantClear(&value);
    addDiagnostic(setting, "property " + narrow(property) + ": " + narrow(result));
    return result;
}

AdvancedPropertyValue propertyForAdvancedKeyword(
    WmiConnection& connection,
    const std::wstring& interfaceDescription,
    const std::wstring& keyword,
    WolSetting& setting)
{
    AdvancedPropertyValue result;
    const std::wstring wql =
        L"SELECT * FROM MSFT_NetAdapterAdvancedPropertySettingData WHERE InterfaceDescription = "
        + wqlQuote(interfaceDescription);
    auto rows = connection.query(wql, setting);
    for (auto& row : rows) {
        VARIANT keywordValue;
        if (!getProperty(row.get(), L"RegistryKeyword", &keywordValue)) continue;
        const std::wstring actualKeyword = variantToString(keywordValue);
        VariantClear(&keywordValue);
        if (actualKeyword != keyword) continue;

        result.found = true;

        VARIANT displayValue;
        if (getProperty(row.get(), L"DisplayValue", &displayValue)) {
            result.displayValue = variantToString(displayValue);
            VariantClear(&displayValue);
            addDiagnostic(setting, "advanced " + narrow(keyword) + " DisplayValue: " + narrow(result.displayValue));
        }
        VARIANT registryValue;
        if (getProperty(row.get(), L"RegistryValue", &registryValue)) {
            result.registryValue = variantToString(registryValue);
            VariantClear(&registryValue);
            addDiagnostic(setting, "advanced " + narrow(keyword) + " RegistryValue: " + narrow(result.registryValue));
        }
        return result;
    }
    return result;
}

bool setAdvancedKeywordEnabled(
    WmiConnection& connection,
    const std::wstring& interfaceDescription,
    const std::wstring& keyword,
    WolSetting& setting,
    bool* permissionDenied)
{
    const std::wstring wql =
        L"SELECT * FROM MSFT_NetAdapterAdvancedPropertySettingData WHERE InterfaceDescription = "
        + wqlQuote(interfaceDescription);
    auto rows = connection.query(wql, setting);
    for (auto& row : rows) {
        VARIANT keywordValue;
        if (!getProperty(row.get(), L"RegistryKeyword", &keywordValue)) continue;
        const std::wstring actualKeyword = variantToString(keywordValue);
        VariantClear(&keywordValue);
        if (actualKeyword != keyword) continue;

        const std::vector<std::wstring> validDisplayValues = propertyStringArray(row.get(), L"ValidDisplayValues");
        const std::vector<std::wstring> validRegistryValues = propertyStringArray(row.get(), L"ValidRegistryValues");
        const std::optional<std::wstring> displayValue = chooseEnabledDisplayValue(validDisplayValues);
        const std::vector<std::wstring> registryValue = chooseEnabledRegistryValue(validRegistryValues);

        if (displayValue.has_value()) {
            putStringProperty(row.get(), L"DisplayValue", *displayValue);
            addDiagnostic(setting, "set advanced " + narrow(keyword) + " DisplayValue -> " + narrow(*displayValue));
        }
        putStringArrayProperty(row.get(), L"RegistryValue", registryValue);
        addDiagnostic(setting, "set advanced " + narrow(keyword) + " RegistryValue -> " + narrow(registryValue.front()));
        return putInstance(connection, row.get(), setting, permissionDenied);
    }
    addDiagnostic(setting, "advanced keyword not found for enable: " + narrow(keyword));
    return false;
}

std::wstring pnpDeviceIdForDescription(WmiConnection& cimv2, const std::wstring& description, WolSetting& setting)
{
    return firstProperty(
        cimv2,
        L"SELECT * FROM Win32_NetworkAdapter WHERE Description = " + wqlQuote(description),
        L"PNPDeviceID",
        setting);
}

bool instanceMatchesPnp(const std::wstring& instanceName, const std::wstring& pnpDeviceId)
{
    return lower(instanceName).find(lower(pnpDeviceId)) != std::wstring::npos;
}

std::optional<bool> firstMatchingWmiOptionalBool(
    WmiConnection& rootWmi,
    const std::wstring& className,
    const wchar_t* property,
    const std::wstring& pnpDeviceId,
    WolSetting& setting)
{
    auto rows = rootWmi.query(L"SELECT * FROM " + className, setting);
    for (auto& row : rows) {
        VARIANT instanceName;
        if (!getProperty(row.get(), L"InstanceName", &instanceName)) continue;
        const std::wstring instance = variantToString(instanceName);
        VariantClear(&instanceName);
        if (!instanceMatchesPnp(instance, pnpDeviceId)) continue;

        VARIANT value;
        if (!getProperty(row.get(), property, &value)) {
            break;
        }
        const std::wstring text = variantToString(value);
        addDiagnostic(setting, "property " + narrow(property) + ": " + narrow(text));
        const bool enabledKnown = isEnabledValue(text);
        const bool disabledKnown = isDisabledValue(text);
        VariantClear(&value);
        if (enabledKnown) return true;
        if (disabledKnown) return false;
        addDiagnostic(setting, "fallback: unrecognized WMI bool value for " + narrow(property));
        return std::nullopt;
    }
    return std::nullopt;
}

bool setFirstMatchingWmiBool(
    WmiConnection& rootWmi,
    const std::wstring& className,
    const wchar_t* property,
    const std::wstring& pnpDeviceId,
    bool desiredValue,
    WolSetting& setting,
    bool* permissionDenied)
{
    auto rows = rootWmi.query(L"SELECT * FROM " + className, setting);
    for (auto& row : rows) {
        VARIANT instanceName;
        if (!getProperty(row.get(), L"InstanceName", &instanceName)) continue;
        const std::wstring instance = variantToString(instanceName);
        VariantClear(&instanceName);
        if (!instanceMatchesPnp(instance, pnpDeviceId)) continue;

        if (!putBoolProperty(row.get(), property, desiredValue)) {
            addDiagnostic(setting, "failed: unable to set " + narrow(property));
            return false;
        }
        addDiagnostic(setting, "set " + narrow(property) + " -> " + (desiredValue ? "true" : "false"));
        return putInstance(rootWmi, row.get(), setting, permissionDenied);
    }
    addDiagnostic(setting, "wmi row not found for set: " + narrow(className) + "." + narrow(property));
    return false;
}

bool isFullyEnabled(const WolSetting& setting)
{
    return setting.wol_supported
        && setting.cable_connected
        && setting.allow_S5_wake_on_lan
        && setting.allow_wake_on_magic_packet
        && setting.allow_wake_up_this_device;
}

#endif

} // namespace

WolSetting WolDetector::detect()
{
    WolSetting setting;

#if !defined(_WIN32)
    setting.diagnostics.push_back("failed: WOL detection is only implemented on Windows");
    return setting;
#else
    ComApartment apartment(setting);
    if (!apartment.ok()) {
        setting.wol_supported = true;
        setting.allow_S5_wake_on_lan = true;
        setting.allow_wake_on_magic_packet = true;
        setting.allow_wake_up_this_device = true;
        setting.diagnostics.push_back("fallback: COM initialization failed; treating WOL settings as supported/enabled");
        return setting;
    }

    WmiConnection cimv2(L"ROOT\\CIMV2", setting);
    if (cimv2.ok()) {
        setting.board_manufacturer = narrow(firstProperty(
            cimv2,
            L"SELECT * FROM Win32_BaseBoard",
            L"Manufacturer",
            setting));
    }

    const std::vector<AdapterInfo> adapters = ethernetAdapters(setting);
    const AdapterInfo adapter = chooseAdapter(adapters, setting);
    if (adapter.description.empty()) {
        setting.cable_connected = false;
        setting.wol_supported = false;
        setting.diagnostics.push_back("failed: no physical ethernet adapter found via IP Helper");
        return setting;
    }

    setting.cable_connected = adapter.cableConnected;
    const std::wstring interfaceDescription = adapter.description;

    WmiConnection standardCim(L"ROOT\\StandardCimv2", setting);
    if (!standardCim.ok()) {
        setting.wol_supported = true;
        setting.diagnostics.push_back("fallback: ROOT\\StandardCimv2 unavailable; treating wol_supported=true");
    } else {
        const std::wstring powerWql =
            L"SELECT * FROM MSFT_NetAdapterPowerManagementSettingData WHERE InterfaceDescription = "
            + wqlQuote(interfaceDescription);
        auto powerRows = standardCim.query(powerWql, setting);
        if (powerRows.empty()) {
            setting.wol_supported = true;
            setting.diagnostics.push_back("fallback: power management setting row not found; treating wol_supported=true");
        } else {
            VARIANT allowTurnOff;
            if (getProperty(powerRows.front().get(), L"AllowComputerToTurnOffDevice", &allowTurnOff)) {
                const std::wstring valueText = variantToString(allowTurnOff);
                setting.wol_supported = netAdapterPowerSettingSupported(allowTurnOff);
                addDiagnostic(setting, "property AllowComputerToTurnOffDevice: " + narrow(valueText));
                VariantClear(&allowTurnOff);
            } else {
                setting.wol_supported = true;
                setting.diagnostics.push_back("fallback: AllowComputerToTurnOffDevice missing; treating wol_supported=true");
            }
        }

        if (!setting.cable_connected) {
            return setting;
        }

        AdvancedPropertyValue s5Value = propertyForAdvancedKeyword(standardCim, interfaceDescription, L"S5WakeOnLan", setting);
        if (!s5Value.found) {
            s5Value = propertyForAdvancedKeyword(standardCim, interfaceDescription, L"EnablePME", setting);
        }
        if (!s5Value.found) {
            setting.allow_S5_wake_on_lan = true;
            setting.diagnostics.push_back("fallback: S5WakeOnLan/EnablePME query failed; treating allow_S5_wake_on_lan=true");
        } else {
            bool recognized = false;
            setting.allow_S5_wake_on_lan = evaluateEnabledOrDisabled(
                s5Value.displayValue,
                s5Value.registryValue,
                true,
                &recognized);
            if (!recognized) {
                setting.diagnostics.push_back("fallback: unrecognized S5WakeOnLan/EnablePME value; treating enabled");
            }
        }

        auto wakeRows = standardCim.query(powerWql, setting);
        bool magicPacket = true;
        bool pattern = true;
        if (wakeRows.empty()) {
            setting.diagnostics.push_back("fallback: WakeOnMagicPacket/WakeOnPattern row missing; treating enabled");
        } else {
            VARIANT magic;
            if (getProperty(wakeRows.front().get(), L"WakeOnMagicPacket", &magic)) {
                const std::wstring valueText = variantToString(magic);
                magicPacket = netAdapterPowerSettingEnabled(magic, true);
                addDiagnostic(setting, "property WakeOnMagicPacket: " + narrow(valueText));
                VariantClear(&magic);
            } else {
                setting.diagnostics.push_back("fallback: WakeOnMagicPacket missing; treating enabled");
            }

            VARIANT wakePattern;
            if (getProperty(wakeRows.front().get(), L"WakeOnPattern", &wakePattern)) {
                const std::wstring valueText = variantToString(wakePattern);
                pattern = netAdapterPowerSettingEnabled(wakePattern, true);
                addDiagnostic(setting, "property WakeOnPattern: " + narrow(valueText));
                VariantClear(&wakePattern);
            } else {
                setting.diagnostics.push_back("fallback: WakeOnPattern missing; treating enabled");
            }
        }
        setting.allow_wake_on_magic_packet = magicPacket && pattern;
    }

    if (!setting.cable_connected) {
        return setting;
    }

    std::wstring pnpDeviceId;
    if (cimv2.ok()) {
        pnpDeviceId = pnpDeviceIdForDescription(cimv2, interfaceDescription, setting);
    }

    if (pnpDeviceId.empty()) {
        setting.allow_wake_up_this_device = true;
        setting.diagnostics.push_back("fallback: PNPDeviceID query failed; treating allow_wake_up_this_device=true");
        return setting;
    }

    WmiConnection rootWmi(L"ROOT\\WMI", setting);
    if (!rootWmi.ok()) {
        setting.allow_wake_up_this_device = true;
        setting.diagnostics.push_back("fallback: ROOT\\WMI unavailable; treating allow_wake_up_this_device=true");
        return setting;
    }

    const std::optional<bool> deviceWakeEnabled = firstMatchingWmiOptionalBool(
        rootWmi,
        L"MSPower_DeviceWakeEnable",
        L"Enable",
        pnpDeviceId,
        setting);
    const std::optional<bool> magicOnlyEnabled = firstMatchingWmiOptionalBool(
        rootWmi,
        L"MSNdis_DeviceWakeOnMagicPacketOnly",
        L"EnableWakeOnMagicPacketOnly",
        pnpDeviceId,
        setting);

    if (deviceWakeEnabled.has_value() && !*deviceWakeEnabled) {
        setting.allow_wake_up_this_device = false;
    } else if (magicOnlyEnabled.has_value() && *magicOnlyEnabled) {
        setting.allow_wake_up_this_device = true;
    } else if (deviceWakeEnabled.has_value() && *deviceWakeEnabled) {
        setting.allow_wake_up_this_device = true;
    } else {
        setting.allow_wake_up_this_device = true;
        setting.diagnostics.push_back("fallback: wake-up-this-device query failed/no decisive value; treating enabled");
    }
    return setting;
#endif
}

WolApplyResult WolDetector::enable()
{
    WolApplyResult result;

#if !defined(_WIN32)
    result.setting.diagnostics.push_back("failed: WOL enable is only implemented on Windows");
    return result;
#else
    result.setting = detect();
    if (isFullyEnabled(result.setting)) {
        result.success = true;
        return result;
    }

    WolSetting applyLog;
    bool permissionDenied = false;

    ComApartment apartment(applyLog);
    if (!apartment.ok()) {
        applyLog.diagnostics.push_back("failed: COM initialization failed while enabling WOL");
    } else {
        const std::vector<AdapterInfo> adapters = ethernetAdapters(applyLog);
        const AdapterInfo adapter = chooseAdapter(adapters, applyLog);
        if (adapter.description.empty()) {
            applyLog.diagnostics.push_back("failed: no physical ethernet adapter found for enable");
        } else {
            const std::wstring interfaceDescription = adapter.description;

            WmiConnection standardCim(L"ROOT\\StandardCimv2", applyLog);
            if (!standardCim.ok()) {
                applyLog.diagnostics.push_back("failed: ROOT\\StandardCimv2 unavailable while enabling WOL");
            } else {
                const std::wstring powerWql =
                    L"SELECT * FROM MSFT_NetAdapterPowerManagementSettingData WHERE InterfaceDescription = "
                    + wqlQuote(interfaceDescription);
                auto powerRows = standardCim.query(powerWql, applyLog);
                if (powerRows.empty()) {
                    applyLog.diagnostics.push_back("failed: power management setting row not found while enabling WOL");
                } else {
                    invokePowerManagementMethod(
                        standardCim,
                        powerRows.front().get(),
                        L"Enable",
                        applyLog,
                        &permissionDenied);
                }

                const bool s5Set = setAdvancedKeywordEnabled(
                    standardCim,
                    interfaceDescription,
                    L"S5WakeOnLan",
                    applyLog,
                    &permissionDenied);
                const bool pmeSet = setAdvancedKeywordEnabled(
                    standardCim,
                    interfaceDescription,
                    L"EnablePME",
                    applyLog,
                    &permissionDenied);
                if (!s5Set && !pmeSet) {
                    applyLog.diagnostics.push_back("failed: neither S5WakeOnLan nor EnablePME could be enabled");
                }
            }

            WmiConnection cimv2(L"ROOT\\CIMV2", applyLog);
            std::wstring pnpDeviceId;
            if (cimv2.ok()) {
                pnpDeviceId = pnpDeviceIdForDescription(cimv2, interfaceDescription, applyLog);
            }
            if (pnpDeviceId.empty()) {
                applyLog.diagnostics.push_back("failed: PNPDeviceID query failed while enabling WOL");
            } else {
                WmiConnection rootWmi(L"ROOT\\WMI", applyLog);
                if (!rootWmi.ok()) {
                    applyLog.diagnostics.push_back("failed: ROOT\\WMI unavailable while enabling WOL");
                } else {
                    setFirstMatchingWmiBool(
                        rootWmi,
                        L"MSPower_DeviceWakeEnable",
                        L"Enable",
                        pnpDeviceId,
                        true,
                        applyLog,
                        &permissionDenied);
                    setFirstMatchingWmiBool(
                        rootWmi,
                        L"MSNdis_DeviceWakeOnMagicPacketOnly",
                        L"EnableWakeOnMagicPacketOnly",
                        pnpDeviceId,
                        true,
                        applyLog,
                        &permissionDenied);
                }
            }
        }
    }

    WolSetting finalSetting = detect();
    finalSetting.diagnostics.insert(
        finalSetting.diagnostics.begin(),
        applyLog.diagnostics.begin(),
        applyLog.diagnostics.end());

    result.permission_denied = permissionDenied;
    result.setting = std::move(finalSetting);
    result.success = isFullyEnabled(result.setting);
    return result;
#endif
}

} // namespace platform
