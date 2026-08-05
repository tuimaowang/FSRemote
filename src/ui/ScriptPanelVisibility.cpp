#include "ui/ScriptPanelVisibility.h"

namespace ui {

// =====wjy====
ScriptPanelVisibility computeScriptPanelVisibility(const ScriptPanelViewContext& context)
{
    const bool deviceDetailPage = !context.settingsSelected
        && !context.remoteAssistSelected
        && !context.localInfoSelected; // wjy: 三个非设备详情页面共用同一屏蔽条件，避免各处重复组合布尔表达式。
    const bool scriptPage = deviceDetailPage && context.scriptTabSelected;

    ScriptPanelVisibility visibility;
    visibility.treeVisible = scriptPage; // wjy: 脚本目录树只在脚本标签页可见。
    visibility.outputVisible = scriptPage && context.outputRequested; // wjy: 输出控件还要受当前设备是否有输出状态控制。
    visibility.editorVisible = deviceDetailPage
        && context.configTabSelected
        && context.editorRequested; // wjy: 编辑器只在配置文件标签页并且已有编辑器状态时显示。
    return visibility;
}
// ===end====

} // namespace ui
