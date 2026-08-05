#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "session_protocol.h"

#include <optional>

namespace uu {

// =====wjy====
inline std::optional<StandardCursorShape> standard_resize_cursor_shape_from_hit_test(LRESULT hit_test)
{
    switch (hit_test) {
    case HTLEFT:
    case HTRIGHT:
        return StandardCursorShape::SizeHorizontal; // wjy: 左右边框都使用 Windows 水平双箭头。
    case HTTOP:
    case HTBOTTOM:
        return StandardCursorShape::SizeVertical; // wjy: 上下边框都使用 Windows 垂直双箭头。
    case HTTOPLEFT:
    case HTBOTTOMRIGHT:
        return StandardCursorShape::SizeNorthwestSoutheast; // wjy: 左上与右下保持同一条西北到东南对角线。
    case HTTOPRIGHT:
    case HTBOTTOMLEFT:
        return StandardCursorShape::SizeNortheastSouthwest; // wjy: 右上与左下保持另一条东北到西南对角线。
    default:
        return std::nullopt; // wjy: 客户区、标题栏和按钮不伪装成缩放边缘，继续采用真实 HCURSOR 分类。
    }
}
// ===end====

} // namespace uu
