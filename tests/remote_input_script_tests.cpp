#include "ui/RemoteInputScript.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cassert>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temporaryDirectory;
    assert(temporaryDirectory.isValid());

    // =====wjy====
    ui::RemoteInputScript script;
    script.name = QString::fromUtf8("登录:测试/一");
    script.sourceHost = QStringLiteral("192.168.1.113");
    script.sourceFrameSize = QSize(1920, 1080);

    ui::RemoteInputScriptEvent move;
    move.elapsedMs = 25;
    move.input.type = ui::RemoteInputEventType::AbsoluteMove;
    move.input.normalizedX = 12345;
    move.input.normalizedY = 54321;
    script.events.push_back(move);

    ui::RemoteInputScriptEvent keyDown;
    keyDown.elapsedMs = 80;
    keyDown.input.type = ui::RemoteInputEventType::KeyDown;
    keyDown.input.virtualKey = 65;
    script.events.push_back(keyDown);

    ui::RemoteInputScriptEvent keyUp = keyDown;
    keyUp.elapsedMs = 140;
    keyUp.input.type = ui::RemoteInputEventType::KeyUp;
    script.events.push_back(keyUp);

    QString savedPath;
    QString error;
    assert(ui::RemoteInputScriptStore::saveToDirectory(
        temporaryDirectory.path(), script, &savedPath, &error));
    assert(error.isEmpty());
    assert(QFile::exists(savedPath));
    assert(!savedPath.contains(QLatin1Char(':')) || savedPath.indexOf(QLatin1Char(':')) == 1); // wjy: 盘符冒号允许保留，用户名称里的非法冒号必须已被替换。

    ui::RemoteInputScript loaded;
    assert(ui::RemoteInputScriptStore::loadFromFile(savedPath, &loaded, &error));
    assert(loaded.name == script.name);
    assert(loaded.sourceHost == script.sourceHost);
    assert(loaded.sourceFrameSize == script.sourceFrameSize);
    assert(loaded.events.size() == 3);
    assert(loaded.events.at(0).input.normalizedX == 12345);
    assert(loaded.events.at(1).input.virtualKey == 65);
    assert(loaded.events.at(2).input.type == ui::RemoteInputEventType::KeyUp);

    assert(ui::RemoteInputScriptStore::safeBaseName(QStringLiteral("CON")) == QStringLiteral("CON_"));
    assert(ui::RemoteInputScriptStore::safeBaseName(QStringLiteral("CON.txt")) == QStringLiteral("CON_.txt"));
    assert(ui::RemoteInputScriptStore::safeBaseName(QStringLiteral("a/b:c")) == QStringLiteral("a_b_c"));

    assert(ui::remoteInputScriptPlaybackTimeMs(1000, 2.0) == 500);
    assert(ui::remoteInputScriptPlaybackTimeMs(1000, 0.5) == 2000);
    assert(ui::remoteInputScriptPlaybackTimeMs(1000, 0.0) == 1000); // wjy: 非法速度回退到原始时间，调度器不会产生除零或负延迟。
    assert(ui::remoteInputScriptPlaybackTimeMs(1000, 100.0) == 100); // wjy: 外部异常倍速按UI最大10倍夹紧，避免瞬间发送整段脚本。
    assert(ui::remoteInputScriptShouldRepeat(0, 999999));
    assert(ui::remoteInputScriptShouldRepeat(3, 1));
    assert(!ui::remoteInputScriptShouldRepeat(3, 3)); // wjy: 正数循环次数包含第一次执行，完成配置轮数后必须自然结束。

    QString duplicatePath;
    assert(ui::RemoteInputScriptStore::saveToDirectory(
        temporaryDirectory.path(), script, &duplicatePath, &error));
    assert(duplicatePath != savedPath); // wjy: 同名二次保存必须生成带序号的新文件，不能覆盖第一次录制。

    QFile invalid(temporaryDirectory.filePath(QStringLiteral("invalid.fsinput.json")));
    assert(invalid.open(QIODevice::WriteOnly));
    invalid.write("{\"format\":\"other\",\"version\":1,\"events\":[]}");
    invalid.close();
    assert(!ui::RemoteInputScriptStore::loadFromFile(invalid.fileName(), &loaded, &error));

    QFile invalidButton(temporaryDirectory.filePath(QStringLiteral("invalid-button.fsinput.json")));
    assert(invalidButton.open(QIODevice::WriteOnly));
    invalidButton.write("{\"format\":\"fsremote-input-script\",\"version\":1,\"events\":[{\"elapsedMs\":0,\"type\":\"button_down\",\"normalizedX\":1,\"normalizedY\":1,\"relativeX\":0,\"relativeY\":0,\"fallbackDeltaX\":0,\"fallbackDeltaY\":0,\"buttons\":0,\"button\":3,\"wheelDelta\":0,\"virtualKey\":0}]}");
    invalidButton.close();
    assert(!ui::RemoteInputScriptStore::loadFromFile(invalidButton.fileName(), &loaded, &error)); // wjy: 非法按钮编码不能进入远端输入协议。
    // ===end====
    return 0;
}
