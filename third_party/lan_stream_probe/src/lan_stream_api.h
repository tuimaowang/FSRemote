#pragma once

#include <cstdint>
#include <d3d11.h>

#ifdef LAN_STREAM_CORE_EXPORTS
#define LSP_API extern "C" __declspec(dllexport)
#else
#define LSP_API extern "C" __declspec(dllimport)
#endif

struct LspStats {
    uint32_t frame_id;
    uint32_t width;
    uint32_t height;
    uint64_t frames;
    uint64_t packets;
    uint64_t bytes;
    uint64_t incomplete;
    uint64_t dropped;
    uint64_t bad_packets;
    uint64_t fec_recovered;
    uint64_t nack_sent;
    uint64_t pli_sent;
    uint64_t rtx_sent;
    uint64_t encode_errors;
    uint64_t send_errors;
};

using LspHostHandle = void*;
using LspViewerHandle = void*;

LSP_API LspHostHandle lsp_host_create();
LSP_API void lsp_host_destroy(LspHostHandle handle);
LSP_API int lsp_host_start(LspHostHandle handle, const char* viewer_ip, uint16_t port, uint32_t bitrate_kbps, uint32_t fps);
LSP_API void lsp_host_stop(LspHostHandle handle);
LSP_API void lsp_host_get_stats(LspHostHandle handle, LspStats* stats);
LSP_API const char* lsp_host_last_error(LspHostHandle handle);

LSP_API LspViewerHandle lsp_viewer_create();
LSP_API void lsp_viewer_destroy(LspViewerHandle handle);
LSP_API int lsp_viewer_start(LspViewerHandle handle, uint16_t port, ID3D11Device* device, ID3D11DeviceContext* context);
LSP_API void lsp_viewer_stop(LspViewerHandle handle);
LSP_API void lsp_viewer_get_stats(LspViewerHandle handle, LspStats* stats);
LSP_API int lsp_viewer_get_latest_frame(LspViewerHandle handle, ID3D11ShaderResourceView** srv, uint32_t* width, uint32_t* height, uint32_t* frame_id);
LSP_API const char* lsp_viewer_last_error(LspViewerHandle handle);
