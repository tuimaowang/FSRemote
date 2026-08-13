[CmdletBinding()] # 启用标准 PowerShell 参数绑定和错误处理行为。
param() # 当前独立构建脚本不需要额外参数。

$ErrorActionPreference = 'Stop' # 任意命令失败时立即停止，避免把不完整产物报告为成功。
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path # 使用脚本所在目录作为独立 Bridge 项目根目录。
$buildRoot = Join-Path $projectRoot 'build' # 将所有生成文件集中放入独立构建目录。
$cmake = (Get-Command cmake -ErrorAction Stop).Source # 从 PATH 查找 CMake，避免依赖某台电脑的固定安装路径。

$configureArguments = @( # 使用数组保存配置参数，避免 PowerShell 续行符与行尾注释冲突。
    '-S' # 指定源码目录参数名称。
    $projectRoot # 使用当前 Bridge 项目目录作为源码根目录。
    '-B' # 指定构建目录参数名称。
    $buildRoot # 将生成文件写入独立 build 目录。
    '-G' # 指定 CMake 生成器参数名称。
    'Visual Studio 17 2022' # 使用项目验证过的 Visual Studio 2022 生成器。
    '-A' # 指定目标处理器架构参数名称。
    'x64' # FakerInput 驱动和主程序均使用 Windows x64 架构。
) # 完成独立 Bridge 配置参数集合。
& $cmake @configureArguments # 使用稳定参数边界执行 CMake 配置。

if ($LASTEXITCODE -ne 0) { # 配置失败时禁止继续使用旧缓存构建。
    throw "CMake configure failed with exit code $LASTEXITCODE." # 保留 CMake 输出并返回失败退出码。
}

$buildArguments = @( # 使用数组保存构建参数，确保 Windows PowerShell 5 可以直接解析。
    '--build' # 切换到 CMake 构建模式。
    $buildRoot # 使用刚完成配置的构建目录。
    '--config' # 指定多配置生成器的构建配置参数。
    'Release' # 生成与 FSRemote 发布包一致的优化版本。
    '--parallel' # 让 CMake 按当前机器能力并行编译。
) # 完成独立 Bridge 构建参数集合。
& $cmake @buildArguments # 执行独立 Bridge 的 Release 构建。

if ($LASTEXITCODE -ne 0) { # 编译或链接失败时停止后续产物校验。
    throw "CMake build failed with exit code $LASTEXITCODE." # 返回实际构建失败状态供自动化调用者识别。
}

$exe = Join-Path $buildRoot 'Release\FakerInputBridge.exe' # 使用 CMake 默认的多配置生成器输出路径定位成品。
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { # 验证链接成功后确实生成了可执行文件。
    throw "Build completed but the executable was not found: $exe" # 报告预期产物路径便于检查生成器输出。
}

Write-Host "Built: $exe" # 向调用者显示可直接部署的 Bridge 路径。
Get-FileHash -LiteralPath $exe -Algorithm SHA256 | # 计算成品哈希，便于发布前核对文件一致性。
    Format-List Algorithm, Hash, Path # 以易读格式输出算法、哈希和文件路径。
