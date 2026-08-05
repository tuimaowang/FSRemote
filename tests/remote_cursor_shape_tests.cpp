#include "ui/RemoteCursorShape.h"

#include <QString>

#include <cstdlib>
#include <iostream>

namespace {

// =====wjy====
void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "remote_cursor_shape_tests failed: " << message << '\n';
        std::exit(1); // wjy: Release 会定义 NDEBUG，因此使用显式退出而不是会被编译器移除的 assert。
    }
}
// ===end====

} // namespace

int main()
{
    using ui::remoteCursorShapeFromStatus;

    // =====wjy====
    const struct {
        QStringView token;
        Qt::CursorShape expected;
    } cases[] = {
        {u"arrow", Qt::ArrowCursor},
        {u"ibeam", Qt::IBeamCursor},
        {u"wait", Qt::WaitCursor},
        {u"busy", Qt::BusyCursor},
        {u"cross", Qt::CrossCursor},
        {u"hand", Qt::PointingHandCursor},
        {u"forbidden", Qt::ForbiddenCursor},
        {u"help", Qt::WhatsThisCursor},
        {u"up_arrow", Qt::UpArrowCursor},
        {u"size_all", Qt::SizeAllCursor},
        {u"size_hor", Qt::SizeHorCursor},
        {u"size_ver", Qt::SizeVerCursor},
        {u"size_nwse", Qt::SizeFDiagCursor},
        {u"size_nesw", Qt::SizeBDiagCursor},
    };

    constexpr QStringView prefix = u"__fsremote_cursor_v1 ";
    for (const auto& cursorCase : cases) {
        const QString message = prefix.toString() + cursorCase.token;
        const auto parsed = remoteCursorShapeFromStatus(message);
        require(parsed.has_value(), "known cursor token must parse");
        require(*parsed == cursorCase.expected, "known cursor token must preserve its Qt cursor direction"); // wjy: 每个原生标准光标都必须映射到 Windows 语义一致的 Qt 系统光标。
    }

    require(!remoteCursorShapeFromStatus(u"__fsremote_cursor_v2 size_hor").has_value(), "future cursor protocol version must be ignored");
    require(!remoteCursorShapeFromStatus(u"__fsremote_cursor_v1 unknown").has_value(), "unknown cursor token must be ignored");
    require(!remoteCursorShapeFromStatus(u"__fsremote_cursor_v1 size_hor extra").has_value(), "extra cursor fields must be rejected");
    require(!remoteCursorShapeFromStatus(u"viewer connected").has_value(), "ordinary viewer status must not change the cursor");
    // ===end====
    return 0;
}
