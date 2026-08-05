#pragma once

namespace ui {

// =====wjy====
struct ScriptPanelViewContext {
    bool settingsSelected = false; // wjy: 设置页打开时脚本树、输出和编辑器都必须从页面中隐藏。
    bool remoteAssistSelected = false; // wjy: 远控辅助页与脚本详情互斥，避免真实子控件覆盖其它页面。
    bool localInfoSelected = false; // wjy: 本机信息页不显示设备脚本面板。
    bool scriptTabSelected = false; // wjy: 只有设备详情的脚本标签才允许显示脚本树和输出。
    bool configTabSelected = false; // wjy: 编辑器只属于设备详情的配置文件标签。
    bool outputRequested = false; // wjy: 当前设备是否已有可见脚本输出状态。
    bool editorRequested = false; // wjy: 当前设备是否已有可见编辑器状态。
};

struct ScriptPanelVisibility {
    bool treeVisible = false;
    bool outputVisible = false;
    bool editorVisible = false;
};

ScriptPanelVisibility computeScriptPanelVisibility(const ScriptPanelViewContext& context);
// ===end====

} // namespace ui
