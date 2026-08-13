# 功能：校验仓库内的固定版本依赖归档，并只在归档变化时解压到当前构建目录。
function(fsremote_prepare_archive_dependency archive_path expected_sha256 extracted_root marker_relative_path output_variable)
    if(NOT EXISTS "${archive_path}") # 检查 Git LFS 管理的依赖归档是否已经下载到工作区。
        message(FATAL_ERROR "Missing dependency archive: ${archive_path}. Run 'git lfs pull' before configuring FSRemote.") # 明确提示新电脑先拉取 LFS 文件，而不是继续产生难以理解的缺头文件错误。
    endif()

    file(SHA256 "${archive_path}" actual_sha256) # 对归档计算 SHA-256，阻止损坏文件或 Git LFS 指针文本进入解压流程。
    string(TOLOWER "${actual_sha256}" actual_sha256) # 将实际哈希统一为小写，避免大小写差异造成错误拒绝。
    string(TOLOWER "${expected_sha256}" normalized_expected_sha256) # 将清单中的期望哈希统一为小写以便稳定比较。
    if(NOT actual_sha256 STREQUAL normalized_expected_sha256) # 只接受本项目验证过的固定版本依赖内容。
        message(FATAL_ERROR "Dependency archive hash mismatch: ${archive_path}. Expected ${normalized_expected_sha256}, got ${actual_sha256}. Run 'git lfs pull' again.") # 给出期望值和实际值，方便定位 LFS 未拉取或文件损坏问题。
    endif()

    set(archive_stamp "${extracted_root}/.fsremote-archive-sha256") # 在构建目录记录已解压归档的哈希，避免每次配置重复解压大型依赖。
    set(extraction_required TRUE) # 默认执行解压，只有完整标记和目标文件都匹配时才复用缓存。
    if(EXISTS "${archive_stamp}" AND EXISTS "${extracted_root}/${marker_relative_path}") # 同时验证哈希标记和关键文件，防止半次解压被误判为可用。
        file(READ "${archive_stamp}" extracted_sha256) # 读取上一次成功解压的归档哈希。
        string(STRIP "${extracted_sha256}" extracted_sha256) # 去除标记文件末尾换行，保证字符串比较准确。
        if(extracted_sha256 STREQUAL normalized_expected_sha256) # 仅当缓存确实来自当前固定归档时复用已解压目录。
            set(extraction_required FALSE) # 跳过重复解压以缩短后续 CMake 配置时间。
        endif()
    endif()

    if(extraction_required) # 依赖首次使用、归档升级或缓存不完整时重新生成干净目录。
        file(REMOVE_RECURSE "${extracted_root}") # 只清理当前构建目录中的依赖缓存，不触碰源码仓库或用户安装目录。
        get_filename_component(extraction_parent "${extracted_root}" DIRECTORY) # 计算归档顶层目录应落入的父目录。
        file(MAKE_DIRECTORY "${extraction_parent}") # 在解压前确保构建依赖根目录存在。
        file(ARCHIVE_EXTRACT INPUT "${archive_path}" DESTINATION "${extraction_parent}") # 使用 CMake 自带能力解压，避免要求新电脑额外安装 7-Zip。
        if(NOT EXISTS "${extracted_root}/${marker_relative_path}") # 解压后再次检查关键头文件或运行库，阻止不完整归档继续配置。
            message(FATAL_ERROR "Dependency archive did not create the expected file: ${extracted_root}/${marker_relative_path}") # 报告缺失的精确路径，便于重新生成或重新拉取归档。
        endif()
        file(WRITE "${archive_stamp}" "${normalized_expected_sha256}\n") # 仅在完整性检查通过后写入缓存标记。
    endif()

    set(${output_variable} "${extracted_root}" PARENT_SCOPE) # 将已验证的依赖根目录返回给调用方用于后续 CMake 目标配置。
endfunction()
