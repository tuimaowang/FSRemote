## 1. Group workflow helpers

- [x] 1.1 Add helpers that generate a unique default group name, create a stable group, and assign a deduplicated set of valid devices to a destination group.
- [x] 1.2 Add a reveal helper that opens the sidebar and destination group, recalculates rows, clamps scrolling, and optionally starts inline rename with selected text.
- [x] 1.3 Reuse the common group-creation helper from the existing blank-area `新建分组` action without changing that action's current behavior.

## 2. Multi-device context menu

- [x] 2.1 Insert an `添加分组` submenu immediately below `系统设置` only for multi-device menu targets, with `新建分组` first and current group names below it.
- [x] 2.2 Handle `新建分组` by creating the group, assigning all menu targets, revealing it, and entering inline rename.
- [x] 2.3 Handle current-group actions by assigning only changed targets and always expanding and revealing the chosen group.
- [x] 2.4 Keep script action dispatch, system actions, deletion, single-device menus, and remote-window title-bar menus unchanged.

## 3. Verification

- [x] 3.1 Build the affected FSRemote target and run formatting/difference checks.
- [x] 3.2 Run relevant automated tests and manually review menu ordering, idempotent assignment, off-screen scrolling, expansion, and inline editor focus behavior.
- [x] 3.3 Validate the OpenSpec change and confirm no WJY change-log file was created.
