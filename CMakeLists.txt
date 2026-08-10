cmake_minimum_required(VERSION 3.25)
project(realagent-plugins C)

# 插件 SDK 头目录：默认指向兄弟仓库 realagent/core/sdk，可用 REALAGENT_SDK_DIR 覆盖
if(NOT DEFINED REALAGENT_SDK_DIR)
  set(REALAGENT_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../realagent/core/sdk)
endif()

set(CMAKE_C_STANDARD 11)

# 每个插件一个 SHARED 库，PREFIX "" 使产物名为 <name>.dylib/.so（对齐 loader 约定）
function(add_plugin name src)
  add_library(${name} SHARED ${src})
  target_include_directories(${name} PRIVATE ${REALAGENT_SDK_DIR})
  set_target_properties(${name} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${name}"
    C_VISIBILITY_PRESET hidden)
endfunction()

add_plugin(perm-allow-all perm-allow-all/perm_allow_all.c)
add_plugin(core-tools core-tools/core_tools.c)

# 安装：拷到项目级 .realagent/extensions/<name>/
set(RA_EXT_DIR ${CMAKE_SOURCE_DIR}/../realagent/.realagent/extensions)
install(TARGETS perm-allow-all core-tools LIBRARY DESTINATION ${RA_EXT_DIR})
install(FILES perm-allow-all/plugin.json DESTINATION ${RA_EXT_DIR}/perm-allow-all)
install(FILES core-tools/plugin.json DESTINATION ${RA_EXT_DIR}/core-tools)
