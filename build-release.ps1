# 功能：在全新 Windows 工作区中校验 Git LFS 依赖、配置项目并构建完整 Release 发布目录。
param(
    [string]$QtRoot = 'C:\Qt\6.11.1\msvc2022_64', # 指定 Qt MSVC x64 安装根目录，默认匹配项目当前验证版本。
    [string]$BuildDirectory = 'build-release', # 指定独立构建目录，默认保持源码树之外的生成文件集中存放。
    [ValidateRange(1, 64)] # 限制并行任务数量，避免无效或过大的并行参数进入 CMake。
    [int]$Parallel = 4 # 指定 MSBuild 并行编译任务数量。
)

$ErrorActionPreference = 'Stop' # 任意依赖检查或构建命令失败时立即停止，避免误报构建完成。
$sourceRoot = $PSScriptRoot # 使用脚本所在目录作为稳定源码根目录，不依赖调用者当前路径。
$qtConfigPath = Join-Path $QtRoot 'lib\cmake\Qt6\Qt6Config.cmake' # 计算 CMake 查找 Qt6 所需的配置文件路径。
if (-not (Test-Path -LiteralPath $qtConfigPath -PathType Leaf)) { # 配置前验证 Qt 路径和 MSVC 套件是否完整。
    throw "Qt6Config.cmake was not found: $qtConfigPath" # 返回精确缺失路径，提示调用者通过 QtRoot 参数修正安装位置。
}

$requiredCommands = @('git', 'cmake') # 声明克隆依赖和 CMake 构建所需的命令行工具。
foreach ($commandName in $requiredCommands) { # 逐一检查命令是否可从当前 PATH 调用。
    if (-not (Get-Command $commandName -ErrorAction SilentlyContinue)) { # 缺少任意基础工具时禁止继续配置。
        throw "Required command is missing from PATH: $commandName" # 返回缺失工具名称，便于安装后重试。
    }
}

& git -C $sourceRoot lfs version | Out-Null # 验证当前 Git 安装已经提供 Git LFS 扩展。
if ($LASTEXITCODE -ne 0) { # Git LFS 子命令不可用时无法取得大型固定依赖。
    throw 'Git LFS is required. Install Git LFS and run git lfs install.' # 给出新电脑必须完成的依赖安装动作。
}

& git -C $sourceRoot lfs pull # 拉取 WebRTC、FFmpeg 和 FakerInput MSI 的真实 LFS 内容。
if ($LASTEXITCODE -ne 0) { # 网络或配额问题导致 LFS 拉取失败时拒绝继续配置。
    throw 'git lfs pull failed.' # 保留 Git LFS 已输出的具体错误并停止构建。
}

$absoluteBuildDirectory = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) { # 允许调用者传入绝对构建目录。
    $BuildDirectory # 绝对路径无需再拼接源码根目录。
} else {
    Join-Path $sourceRoot $BuildDirectory # 相对路径默认解析到当前项目目录内。
}

& cmake -S $sourceRoot -B $absoluteBuildDirectory -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$QtRoot" '-DFSREMOTE_BUILD_TESTS=OFF' # 使用 VS2022 x64 和指定 Qt 完整配置生产构建。
if ($LASTEXITCODE -ne 0) { # CMake 配置失败时停止，不执行可能使用旧缓存的构建命令。
    throw 'CMake configure failed.' # 保留 CMake 已输出的具体依赖或语法错误。
}

& cmake --build $absoluteBuildDirectory --config Release --target FSRemote --parallel $Parallel # 构建主程序及其 WebRTC、Bridge、更新器和发布依赖目标。
if ($LASTEXITCODE -ne 0) { # 编译、链接或部署任一步失败时返回失败状态。
    throw 'CMake Release build failed.' # 保留 MSBuild 已输出的具体错误并停止脚本。
}

$releaseDirectory = Join-Path $absoluteBuildDirectory 'Release' # 计算完整发布目录路径供调用者直接使用。
Write-Host "FSRemote Release build completed: $releaseDirectory" # 明确输出成功产物位置。
