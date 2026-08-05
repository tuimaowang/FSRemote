#pragma once

#include <QStringView>

#include <optional>

namespace ui {

// =====wjy====
inline std::optional<Qt::CursorShape> remoteCursorShapeFromStatus(QStringView message)
{
    constexpr QStringView kPrefix = u"__fsremote_cursor_v1 ";
    if (!message.startsWith(kPrefix)) {
        return std::nullopt; // wjy: 仅接受带明确版本前缀的原生状态消息，普通连接状态文本不得改变远控画面光标。
    }

    const QStringView token = message.sliced(kPrefix.size());
    if (token == u"arrow") return Qt::ArrowCursor;
    if (token == u"ibeam") return Qt::IBeamCursor;
    if (token == u"wait") return Qt::WaitCursor;
    if (token == u"busy") return Qt::BusyCursor;
    if (token == u"cross") return Qt::CrossCursor;
    if (token == u"hand") return Qt::PointingHandCursor;
    if (token == u"forbidden") return Qt::ForbiddenCursor;
    if (token == u"help") return Qt::WhatsThisCursor;
    if (token == u"up_arrow") return Qt::UpArrowCursor;
    if (token == u"size_all") return Qt::SizeAllCursor;
    if (token == u"size_hor") return Qt::SizeHorCursor;
    if (token == u"size_ver") return Qt::SizeVerCursor;
    if (token == u"size_nwse") return Qt::SizeFDiagCursor;
    if (token == u"size_nesw") return Qt::SizeBDiagCursor;
    return std::nullopt; // wjy: 未知或附带多余字段的形状保持当前光标，避免协议扩展被旧版查看器误解释。
}
// ===end====

} // namespace ui
