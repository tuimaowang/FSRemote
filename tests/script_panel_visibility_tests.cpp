#include "ui/ScriptPanelVisibility.h"

#include <cassert>

int main()
{
    // =====wjy====
    ui::ScriptPanelViewContext context;
    context.scriptTabSelected = true;
    context.outputRequested = true;
    auto visibility = ui::computeScriptPanelVisibility(context);
    assert(visibility.treeVisible);
    assert(visibility.outputVisible);
    assert(!visibility.editorVisible);

    context.configTabSelected = true;
    context.scriptTabSelected = false;
    context.editorRequested = true;
    visibility = ui::computeScriptPanelVisibility(context);
    assert(!visibility.treeVisible);
    assert(!visibility.outputVisible);
    assert(visibility.editorVisible);

    context.settingsSelected = true;
    visibility = ui::computeScriptPanelVisibility(context);
    assert(!visibility.treeVisible);
    assert(!visibility.outputVisible);
    assert(!visibility.editorVisible); // wjy: 设置页优先级最高，不能让脚本控件残留在其它页面之上。
    // ===end====
    return 0;
}
