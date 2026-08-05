#include "ui/RemoteInputScript.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ui {
namespace {

// =====wjy====
constexpr int kInputScriptVersion = 1;
constexpr qsizetype kMaximumScriptBytes = 32 * 1024 * 1024;
constexpr qsizetype kMaximumScriptEvents = 100000; // wjy: 限制JSON DOM和回放队列规模，畸形脚本不能用超大事件数组拖死远控UI线程。
constexpr qint64 kMaximumScriptDurationMs = 24LL * 60LL * 60LL * 1000LL;

QString inputEventTypeName(RemoteInputEventType type)
{
    switch (type) {
    case RemoteInputEventType::AbsoluteMove: return QStringLiteral("absolute_move");
    case RemoteInputEventType::RelativeMove: return QStringLiteral("relative_move");
    case RemoteInputEventType::ButtonDown: return QStringLiteral("button_down");
    case RemoteInputEventType::ButtonUp: return QStringLiteral("button_up");
    case RemoteInputEventType::Wheel: return QStringLiteral("wheel");
    case RemoteInputEventType::KeyDown: return QStringLiteral("key_down");
    case RemoteInputEventType::KeyUp: return QStringLiteral("key_up");
    case RemoteInputEventType::CaptureRelease: return QStringLiteral("capture_release");
    }
    return {};
}

bool inputEventTypeFromName(const QString& name, RemoteInputEventType* type)
{
    if (!type) return false;
    if (name == QStringLiteral("absolute_move")) *type = RemoteInputEventType::AbsoluteMove;
    else if (name == QStringLiteral("relative_move")) *type = RemoteInputEventType::RelativeMove;
    else if (name == QStringLiteral("button_down")) *type = RemoteInputEventType::ButtonDown;
    else if (name == QStringLiteral("button_up")) *type = RemoteInputEventType::ButtonUp;
    else if (name == QStringLiteral("wheel")) *type = RemoteInputEventType::Wheel;
    else if (name == QStringLiteral("key_down")) *type = RemoteInputEventType::KeyDown;
    else if (name == QStringLiteral("key_up")) *type = RemoteInputEventType::KeyUp;
    else if (name == QStringLiteral("capture_release")) *type = RemoteInputEventType::CaptureRelease;
    else return false;
    return true;
}

bool integerInRange(const QJsonObject& object, const char* key, int minimum, int maximum, int* value)
{
    const QJsonValue jsonValue = object.value(QLatin1String(key));
    if (!jsonValue.isDouble()) return false;
    const double number = jsonValue.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number < minimum || number > maximum) {
        return false;
    }
    if (value) *value = static_cast<int>(number);
    return true;
}

bool finiteNumber(const QJsonObject& object, const char* key, double minimum, double maximum, double* value)
{
    const QJsonValue jsonValue = object.value(QLatin1String(key));
    if (!jsonValue.isDouble()) return false;
    const double number = jsonValue.toDouble();
    if (!std::isfinite(number) || number < minimum || number > maximum) return false;
    if (value) *value = number;
    return true;
}

QJsonObject inputEventToJson(const RemoteInputScriptEvent& event)
{
    QJsonObject object;
    object.insert(QStringLiteral("elapsedMs"), static_cast<double>(event.elapsedMs));
    object.insert(QStringLiteral("type"), inputEventTypeName(event.input.type));
    object.insert(QStringLiteral("normalizedX"), event.input.normalizedX);
    object.insert(QStringLiteral("normalizedY"), event.input.normalizedY);
    object.insert(QStringLiteral("relativeX"), event.input.relativeX);
    object.insert(QStringLiteral("relativeY"), event.input.relativeY);
    object.insert(QStringLiteral("fallbackDeltaX"), event.input.fallbackDeltaX);
    object.insert(QStringLiteral("fallbackDeltaY"), event.input.fallbackDeltaY);
    object.insert(QStringLiteral("buttons"), event.input.buttons);
    object.insert(QStringLiteral("button"), event.input.button);
    object.insert(QStringLiteral("wheelDelta"), event.input.wheelDelta);
    object.insert(QStringLiteral("virtualKey"), event.input.virtualKey);
    return object;
}

bool inputEventFromJson(
    const QJsonObject& object,
    qint64 previousElapsedMs,
    RemoteInputScriptEvent* event,
    QString* errorMessage)
{
    if (!event || !object.value(QStringLiteral("elapsedMs")).isDouble()) {
        if (errorMessage) *errorMessage = QString::fromUtf8("脚本事件缺少有效时间字段。");
        return false;
    }
    const double elapsedNumber = object.value(QStringLiteral("elapsedMs")).toDouble();
    if (!std::isfinite(elapsedNumber) || std::floor(elapsedNumber) != elapsedNumber
        || elapsedNumber < previousElapsedMs || elapsedNumber > kMaximumScriptDurationMs) {
        if (errorMessage) *errorMessage = QString::fromUtf8("脚本事件时间顺序无效或持续时间过长。");
        return false;
    }

    RemoteInputEventType type;
    if (!inputEventTypeFromName(object.value(QStringLiteral("type")).toString(), &type)) {
        if (errorMessage) *errorMessage = QString::fromUtf8("脚本包含未知键鼠事件类型。");
        return false;
    }

    RemoteInputEvent input;
    input.type = type;
    if (!integerInRange(object, "normalizedX", 0, 65535, &input.normalizedX)
        || !integerInRange(object, "normalizedY", 0, 65535, &input.normalizedY)
        || !finiteNumber(object, "relativeX", -64.0, 64.0, &input.relativeX)
        || !finiteNumber(object, "relativeY", -64.0, 64.0, &input.relativeY)
        || !integerInRange(object, "fallbackDeltaX", -1000000, 1000000, &input.fallbackDeltaX)
        || !integerInRange(object, "fallbackDeltaY", -1000000, 1000000, &input.fallbackDeltaY)
        || !integerInRange(object, "buttons", 0, 7, &input.buttons)
        || !integerInRange(object, "button", 0, 4, &input.button)
        || !integerInRange(object, "wheelDelta", -12000, 12000, &input.wheelDelta)
        || !integerInRange(object, "virtualKey", 0, 65535, &input.virtualKey)) {
        if (errorMessage) *errorMessage = QString::fromUtf8("脚本包含越界或非数字键鼠参数。");
        return false;
    } // wjy: 相对输入保留控制端原始计数的合理宽范围，真正发送时既有序列化器仍会把单次远端位移夹紧到±200。

    const bool validMouseButton = input.button == 1 || input.button == 2 || input.button == 4;
    if (((type == RemoteInputEventType::ButtonDown || type == RemoteInputEventType::ButtonUp)
            && !validMouseButton)
        || ((type == RemoteInputEventType::KeyDown || type == RemoteInputEventType::KeyUp)
            && input.virtualKey <= 0)) {
        if (errorMessage) *errorMessage = QString::fromUtf8("脚本包含与事件类型不匹配的键鼠参数。");
        return false; // wjy: 鼠标按钮只接受左1、右2、中4，键盘事件必须携带有效虚拟键码，禁止畸形本地JSON进入Host注入协议。
    }

    event->elapsedMs = static_cast<qint64>(elapsedNumber);
    event->input = input;
    return true;
}

bool isReservedWindowsBaseName(const QString& name)
{
    const QString upper = name.section(QLatin1Char('.'), 0, 0).toUpper(); // wjy: Windows会把CON.txt等带点名称同样视为保留设备名，必须检查第一个点之前的部分。
    if (upper == QStringLiteral("CON") || upper == QStringLiteral("PRN")
        || upper == QStringLiteral("AUX") || upper == QStringLiteral("NUL")) {
        return true;
    }
    for (int index = 1; index <= 9; ++index) {
        if (upper == QStringLiteral("COM%1").arg(index)
            || upper == QStringLiteral("LPT%1").arg(index)) {
            return true;
        }
    }
    return false;
}
// ===end====

} // namespace

// =====wjy====
qint64 remoteInputScriptPlaybackTimeMs(qint64 recordedElapsedMs, double speedMultiplier)
{
    const qint64 safeRecordedMs = std::max<qint64>(0, recordedElapsedMs);
    const double safeSpeedMultiplier = std::isfinite(speedMultiplier) && speedMultiplier > 0.0
        ? std::clamp(speedMultiplier, 0.10, 10.00)
        : 1.0; // wjy: 与播放弹窗使用相同边界，外部调用也不能用极小正数制造时间溢出或用异常大值瞬间灌入全部事件。
    return std::max<qint64>(0, static_cast<qint64>(std::llround(
        static_cast<double>(safeRecordedMs) / safeSpeedMultiplier))); // wjy: 事件全部基于同一原始绝对时间缩放，不会逐间隔累积舍入误差。
}

bool remoteInputScriptShouldRepeat(int configuredLoopCount, int completedLoopCount)
{
    if (configuredLoopCount == 0) return true;
    return completedLoopCount < std::max(1, configuredLoopCount); // wjy: 正数代表总执行次数；完成第N轮后只有N仍小于配置值才继续。
}
// ===end====

QString RemoteInputScriptStore::defaultDirectory()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("script"));
}

QString RemoteInputScriptStore::fileDialogFilter()
{
    return QString::fromUtf8("FSRemote键鼠脚本 (*.fsinput.json);;JSON文件 (*.json)");
}

QString RemoteInputScriptStore::safeBaseName(const QString& requestedName)
{
    // =====wjy====
    QString safe = requestedName.trimmed();
    static const QString invalidCharacters = QStringLiteral("<>:\"/\\|?*");
    for (qsizetype index = 0; index < safe.size(); ++index) {
        QChar character = safe.at(index);
        if (character.unicode() < 32 || invalidCharacters.contains(character)) {
            safe[index] = QLatin1Char('_'); // wjy: Windows非法文件名字符只替换不删除，用户仍能辨认原脚本名称结构。
        }
    }
    while (safe.endsWith(QLatin1Char('.')) || safe.endsWith(QLatin1Char(' '))) safe.chop(1);
    safe = safe.left(80).trimmed(); // wjy: 限制文件名长度，为目录、扩展名和自动重名后缀保留空间。
    while (safe.endsWith(QLatin1Char('.')) || safe.endsWith(QLatin1Char(' '))) safe.chop(1); // wjy: 截断位置也可能恰好落在点或空格上，二次清理保证最终文件名可由Windows创建。
    if (safe.isEmpty()) safe = QStringLiteral("input-script");
    if (isReservedWindowsBaseName(safe)) {
        const qsizetype firstDot = safe.indexOf(QLatin1Char('.'));
        if (firstDot < 0) safe += QLatin1Char('_');
        else safe.insert(firstDot, QLatin1Char('_')); // wjy: 下划线必须进入第一个点之前，CON.txt_仍会被Windows按CON设备名拒绝。
    }
    return safe;
    // ===end====
}

bool RemoteInputScriptStore::saveToDirectory(
    const QString& directoryPath,
    const RemoteInputScript& script,
    QString* savedFilePath,
    QString* errorMessage)
{
    if (savedFilePath) savedFilePath->clear();
    if (errorMessage) errorMessage->clear();
    if (script.events.size() > kMaximumScriptEvents
        || (!script.events.isEmpty()
            && (script.events.last().elapsedMs < 0
                || script.events.last().elapsedMs > kMaximumScriptDurationMs))) {
        if (errorMessage) *errorMessage = QString::fromUtf8("键鼠脚本事件数量过多或持续时间过长。");
        return false; // wjy: 保存端与加载端使用相同硬边界，程序不会生成随后自己也拒绝打开的脚本。
    }
    QDir directory(directoryPath);
    if (!directory.mkpath(QStringLiteral("."))) {
        if (errorMessage) *errorMessage = QString::fromUtf8("无法创建本地script文件夹。");
        return false;
    }

    const QString baseName = safeBaseName(script.name);
    QString filePath = directory.filePath(baseName + QStringLiteral(".fsinput.json"));
    for (int suffix = 2; QFileInfo::exists(filePath); ++suffix) {
        filePath = directory.filePath(QStringLiteral("%1-%2.fsinput.json").arg(baseName).arg(suffix)); // wjy: 同名脚本自动追加序号，不静默覆盖用户以前录制的操作。
    }

    QJsonArray events;
    qint64 previousElapsedMs = 0;
    for (const RemoteInputScriptEvent& event : script.events) {
        const QJsonObject eventObject = inputEventToJson(event);
        RemoteInputScriptEvent validatedEvent;
        QString validationError;
        if (!inputEventFromJson(
                eventObject, previousElapsedMs, &validatedEvent, &validationError)) {
            if (errorMessage) *errorMessage = QString::fromUtf8("无法保存键鼠脚本：%1").arg(validationError);
            return false; // wjy: 保存前复用加载端的严格校验，程序不会写出时间倒序或参数越界且随后无法回放的文件。
        }
        previousElapsedMs = event.elapsedMs;
        events.append(eventObject);
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("fsremote-input-script"));
    root.insert(QStringLiteral("version"), kInputScriptVersion);
    root.insert(QStringLiteral("name"), script.name.trimmed());
    root.insert(QStringLiteral("sourceHost"), script.sourceHost.trimmed());
    root.insert(QStringLiteral("sourceWidth"), script.sourceFrameSize.width());
    root.insert(QStringLiteral("sourceHeight"), script.sourceFrameSize.height());
    root.insert(QStringLiteral("createdAt"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    root.insert(QStringLiteral("events"), events);

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) *errorMessage = QString::fromUtf8("无法创建键鼠脚本文件：%1").arg(file.errorString());
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact); // wjy: 紧凑JSON显著降低长录制的磁盘占用和写入暂停，字段语义与版本保持不变。
    if (payload.size() > kMaximumScriptBytes) {
        if (errorMessage) *errorMessage = QString::fromUtf8("键鼠脚本内容过大，请缩短录制时间。");
        file.cancelWriting();
        return false;
    }
    if (file.write(payload) != payload.size() || !file.commit()) {
        if (errorMessage) *errorMessage = QString::fromUtf8("写入键鼠脚本失败：%1").arg(file.errorString());
        return false;
    }
    if (savedFilePath) *savedFilePath = QDir::toNativeSeparators(filePath);
    return true;
}

bool RemoteInputScriptStore::loadFromFile(
    const QString& filePath,
    RemoteInputScript* script,
    QString* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (!script) {
        if (errorMessage) *errorMessage = QString::fromUtf8("脚本输出对象为空。");
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = QString::fromUtf8("无法打开键鼠脚本：%1").arg(file.errorString());
        return false;
    }
    if (file.size() < 0 || file.size() > kMaximumScriptBytes) {
        if (errorMessage) *errorMessage = QString::fromUtf8("键鼠脚本文件过大。");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage) *errorMessage = QString::fromUtf8("键鼠脚本JSON无效：%1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("fsremote-input-script")
        || root.value(QStringLiteral("version")).toInt(-1) != kInputScriptVersion
        || !root.value(QStringLiteral("events")).isArray()) {
        if (errorMessage) *errorMessage = QString::fromUtf8("不是受支持的FSRemote键鼠脚本。");
        return false;
    }

    const QJsonArray eventsJson = root.value(QStringLiteral("events")).toArray();
    if (eventsJson.size() > kMaximumScriptEvents) {
        if (errorMessage) *errorMessage = QString::fromUtf8("键鼠脚本事件数量过多。");
        return false;
    }

    RemoteInputScript loaded;
    loaded.name = root.value(QStringLiteral("name")).toString().trimmed();
    if (loaded.name.isEmpty()) loaded.name = QFileInfo(filePath).completeBaseName();
    loaded.sourceHost = root.value(QStringLiteral("sourceHost")).toString().trimmed();
    loaded.sourceFrameSize = QSize(
        std::max(0, root.value(QStringLiteral("sourceWidth")).toInt()),
        std::max(0, root.value(QStringLiteral("sourceHeight")).toInt()));
    loaded.events.reserve(eventsJson.size());
    qint64 previousElapsedMs = 0;
    for (const QJsonValue& value : eventsJson) {
        if (!value.isObject()) {
            if (errorMessage) *errorMessage = QString::fromUtf8("键鼠脚本包含非对象事件。");
            return false;
        }
        RemoteInputScriptEvent event;
        if (!inputEventFromJson(value.toObject(), previousElapsedMs, &event, errorMessage)) return false;
        previousElapsedMs = event.elapsedMs;
        loaded.events.push_back(event);
    }
    *script = std::move(loaded);
    return true;
}

} // namespace ui
