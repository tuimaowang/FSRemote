param(
    [Parameter(Mandatory=$true)]
    [string]$WebRtcRoot,
    [string]$BuildDir = ".\uu_stream_webrtc\build"
)

$ErrorActionPreference = "Stop"

cmake -S .\uu_stream_webrtc -B $BuildDir -DWEBRTC_ROOT="$WebRtcRoot"
cmake --build $BuildDir --config Release --parallel
