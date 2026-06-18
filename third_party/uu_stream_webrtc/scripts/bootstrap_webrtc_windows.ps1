param(
    [string]$Root = "C:\webrtc-checkout",
    [string]$DepotTools = "C:\depot_tools",
    [string]$Out = "uu_release"
)

$ErrorActionPreference = "Stop"

function Require-Command($Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command not found: $Name"
    }
}

Require-Command git
Require-Command python

if (-not (Test-Path $DepotTools)) {
    git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git $DepotTools
}

$env:PATH = "$DepotTools;$env:PATH"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"

Require-Command fetch
Require-Command gclient

New-Item -ItemType Directory -Force -Path $Root | Out-Null
Push-Location $Root
try {
    if (-not (Test-Path ".\src\api\peer_connection_interface.h")) {
        fetch --nohooks webrtc
    }

    gclient sync

    Push-Location ".\src"
    try {
        $gnArgs = @(
            'is_debug=false',
            'target_os="win"',
            'target_cpu="x64"',
            'is_component_build=false',
            'rtc_include_tests=false',
            'rtc_build_examples=false',
            'rtc_build_tools=false',
            'rtc_use_h264=true',
            'rtc_use_h265=true',
            'proprietary_codecs=true',
            'ffmpeg_branding="Chrome"',
            'use_rtti=true',
            'symbol_level=0'
        ) -join ' '

        gn gen "out\$Out" "--args=$gnArgs"
        ninja -C "out\$Out" webrtc
    }
    finally {
        Pop-Location
    }
}
finally {
    Pop-Location
}

Write-Host "WebRTC build root: $Root"
Write-Host "Use CMake with: -DWEBRTC_ROOT=$Root"
