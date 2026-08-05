#include "system/UpdateService.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

int main()
{
    // =====wjy====
    if (!(platform::UpdateService::compareSemanticVersions("1.2.3", "1.2.4") < 0)) return 1; // wjy: 显式返回失败码，Release 的 /DNDEBUG 也会真实验证远端 patch 更大时允许升级。
    if (!(platform::UpdateService::compareSemanticVersions("1.10.0", "1.2.99") > 0)) return 2; // wjy: 数字段比较不能退化为错误的字符串排序。
    if (!(platform::UpdateService::compareSemanticVersions("2.0.0", "2.0.0") == 0)) return 3; // wjy: 相同版本不得被普通版本比较误判为升级。
    if (platform::UpdateService::bumpPatchVersion("1.9.9") != "1.9.10") return 4; // wjy: 发布只递增 patch 并保留 major/minor。
    if (!platform::UpdateService::remoteVersionPath().replace('\\', '/').endsWith(QString::fromUtf8("/更新资源/FSRemote.version"))) return 5; // wjy: 新客户端只允许读取整洁共享结构，禁止无意恢复旧根目录兼容分支。

    QTemporaryDir runtimeDir; // wjy: 使用隔离临时目录模拟旧客户端升级后的安装目录，不读取或修改真实 FSRemote 发布文件。
    if (!runtimeDir.isValid()) return 6;
    if (!platform::UpdateService::runtimeDependenciesNeedRepair(runtimeDir.path())) return 7; // wjy: Bridge/MSI 全部缺失时必须识别为需要修复。
    if (!platform::UpdateService::remoteUpdateOrRepairAvailable("1.1.100", "1.1.100", runtimeDir.path())) return 8; // wjy: 同版本但缺件时重新开放更新，覆盖本次真实故障。

    const auto writeRuntimeFile = [&runtimeDir](const QString& fileName) -> bool {
        QFile file(QDir(runtimeDir.path()).filePath(fileName)); // wjy: 写入一个非空字节，模拟已完整落盘的最小普通文件。
        return file.open(QIODevice::WriteOnly) && file.write("x", 1) == 1; // wjy: 创建或写入失败直接交给主测试返回非零，Release 构建不能静默跳过。
    };
    if (!writeRuntimeFile(QStringLiteral("FakerInputBridge.exe"))) return 9;
    if (!writeRuntimeFile(QStringLiteral("FakerInput_Setup_0.1.1_x64.msi"))) return 10;
    if (platform::UpdateService::runtimeDependenciesNeedRepair(runtimeDir.path())) return 11; // wjy: 两个依赖齐全后不得继续提示同版本更新。
    if (platform::UpdateService::remoteUpdateOrRepairAvailable("1.1.100", "1.1.100", runtimeDir.path())) return 12; // wjy: 修复完成后版本相同，更新状态必须恢复为完成。
    if (!platform::UpdateService::remoteUpdateOrRepairAvailable("1.1.101", "1.1.100", runtimeDir.path())) return 13; // wjy: 正常更高版本升级不受依赖修复分支影响。

    if (!QFile::remove(QDir(runtimeDir.path()).filePath(QStringLiteral("FakerInputBridge.exe")))) return 14;
    if (platform::UpdateService::remoteUpdateOrRepairAvailable("1.1.99", "1.1.100", runtimeDir.path())) return 15; // wjy: 即使本地缺件，远端旧版本也绝不能覆盖较新的本机程序。
    // ===end====
    return 0;
}
