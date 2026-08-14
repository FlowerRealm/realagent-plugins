cmake_minimum_required(VERSION 3.30)
project(realagent-plugins C CXX)

# 插件 SDK 头目录：默认指向兄弟仓库 realagent/core/sdk，可用 REALAGENT_SDK_DIR 覆盖
if(NOT DEFINED REALAGENT_SDK_DIR)
  set(REALAGENT_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../realagent/core/sdk)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 每个插件一个 SHARED 库，PREFIX "" 使产物名为 <name>.dylib/.so（对齐 loader 约定）
function(add_plugin name src)
  add_library(${name} SHARED ${src})
  target_include_directories(${name} PRIVATE ${REALAGENT_SDK_DIR})
  set_target_properties(${name} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${name}"
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden)
endfunction()

# —— 协议链（ADR-0004 嵌套：deepseek 壳包住 v1-messages 协议层） ——
add_plugin(v1-messages v1-messages/v1_messages.cpp)
add_plugin(deepseek deepseek/deepseek.cpp)

add_plugin(perm-allow-all perm-allow-all/perm_allow_all.c)
add_plugin(perm-ask perm-ask/perm_ask.c)
add_plugin(core-tools core-tools/core_tools.c)

# v1-messages / deepseek 用 C++ + boost::json（协议转换 / 壳兜底需要完整 JSON）
find_package(Boost REQUIRED COMPONENTS json)
target_link_libraries(v1-messages PRIVATE Boost::json)
target_link_libraries(deepseek PRIVATE Boost::json)

# 安装：每个插件一个目录（dylib + plugin.json 同目录，对齐 loader 发现约定）。
# 落点是 loader 唯一扫描的全局目录 ~/.realagent/extensions——装到别处等于没装。
set(RA_EXT_DIR $ENV{HOME}/.realagent/extensions)
foreach(p v1-messages deepseek perm-allow-all perm-ask core-tools)
  install(TARGETS ${p} LIBRARY DESTINATION ${RA_EXT_DIR}/${p})
  install(FILES ${p}/plugin.json DESTINATION ${RA_EXT_DIR}/${p})
  # 出厂模型数据表（ADR-0009）：打包产物，只读。用户接管版在 ~/.realagent/models/<name>.json
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${p}/models.json)
    install(FILES ${p}/models.json DESTINATION ${RA_EXT_DIR}/${p})
  endif()
endforeach()
