#include "dxgi_capture.h"

#include <algorithm>
#include <cstdio>

namespace lsp {

std::string win32_error(const char* what, long hr)
{
    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), "%s failed: 0x%08lx", what, static_cast<unsigned long>(hr));
    return buffer;
}

bool DxgiCapture::initialize(std::string* error)
{
    reset();

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) {
        if (error) *error = win32_error("CreateDXGIFactory1", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> selected_adapter;
    UINT selected_output = 0;
    LONG best_area = -1;

    for (UINT adapter_index = 0;; ++adapter_index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        for (UINT output_index = 0;; ++output_index) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC desc = {};
            output->GetDesc(&desc);
            if (!desc.AttachedToDesktop) {
                continue;
            }
            const LONG width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            const LONG height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            const LONG area = width * height;
            const bool primary = desc.DesktopCoordinates.left <= 0 && desc.DesktopCoordinates.top <= 0
                && desc.DesktopCoordinates.right > 0 && desc.DesktopCoordinates.bottom > 0;
            if (primary || area > best_area) {
                selected_adapter = adapter;
                selected_output = output_index;
                best_area = area;
                if (primary) {
                    break;
                }
            }
        }
        if (selected_adapter && best_area > 0) {
            break;
        }
    }

    if (!selected_adapter) {
        if (error) *error = "no attached DXGI output found";
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL created_level = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(selected_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                           &device_, &created_level, &context_);
    if (FAILED(hr)) {
        if (error) *error = win32_error("D3D11CreateDevice", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    hr = selected_adapter->EnumOutputs(selected_output, &output);
    if (FAILED(hr)) {
        if (error) *error = win32_error("EnumOutputs", hr);
        return false;
    }

    DXGI_OUTPUT_DESC output_desc = {};
    output->GetDesc(&output_desc);
    size_.width = static_cast<uint32_t>(output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left);
    size_.height = static_cast<uint32_t>(output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top);

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) {
        if (error) *error = win32_error("Query IDXGIOutput1", hr);
        return false;
    }

    hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        if (error) *error = win32_error("DuplicateOutput", hr);
        return false;
    }
    return true;
}

bool DxgiCapture::capture(CapturedFrame* frame, std::string* error)
{
    if (!duplication_ && !initialize(error)) {
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO info = {};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (error) *error = "timeout";
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        reset();
        if (error) *error = "DXGI access lost";
        return false;
    }
    if (FAILED(hr)) {
        if (error) *error = win32_error("AcquireNextFrame", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktop_texture;
    hr = resource.As(&desktop_texture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        if (error) *error = win32_error("Query texture", hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desktop_texture->GetDesc(&desc);
    if (!recreate_frame_texture(desc, error)) {
        duplication_->ReleaseFrame();
        return false;
    }

    context_->CopyResource(frame_texture_.Get(), desktop_texture.Get());
    duplication_->ReleaseFrame();

    frame->texture = frame_texture_;
    frame->size = {desc.Width, desc.Height};
    return true;
}

bool DxgiCapture::recreate_frame_texture(const D3D11_TEXTURE2D_DESC& source_desc, std::string* error)
{
    if (frame_texture_) {
        D3D11_TEXTURE2D_DESC current = {};
        frame_texture_->GetDesc(&current);
        if (current.Width == source_desc.Width && current.Height == source_desc.Height
            && current.Format == source_desc.Format) {
            return true;
        }
        frame_texture_.Reset();
    }

    D3D11_TEXTURE2D_DESC desc = source_desc;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &frame_texture_);
    if (FAILED(hr)) {
        if (error) *error = win32_error("CreateTexture2D", hr);
        return false;
    }
    return true;
}

void DxgiCapture::reset()
{
    frame_texture_.Reset();
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
    size_ = {};
}

} // namespace lsp
