cmake_minimum_required(VERSION 3.30)
project(realagent-plugins C CXX)

# realugin（插件体系）默认装在 ~/.local。装到别处就自己给 -DCMAKE_PREFIX_PATH=...
list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local")
# realugin_add_plugin / realugin_install_plugin 随 realugin 一起装，见 AddPlugin.cmake
find_package(realugin REQUIRED)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# —— 协议链（ADR-0004 嵌套：deepseek 壳包住 v1-messages 协议层） ——
realugin_add_plugin(v1-messages v1-messages/v1_messages.cpp)
realugin_add_plugin(deepseek deepseek/deepseek.cpp)

realugin_add_plugin(perm-allow-all perm-allow-all/perm_allow_all.c)
realugin_add_plugin(perm-ask perm-ask/perm_ask.c)
realugin_add_plugin(core-tools core-tools/core_tools.c)

# v1-messages / deepseek 用 C++ + boost::json（协议转换 / 壳兜底需要完整 JSON）
find_package(Boost REQUIRED COMPONENTS json)
target_link_libraries(v1-messages PRIVATE Boost::json)
target_link_libraries(deepseek PRIVATE Boost::json)

# 安装：每个插件一个目录（dylib + plugin.json 同目录，对齐 loader 发现约定）。
# 落点是 core 唯一扫描的全局目录 ~/.realagent/extensions——装到别处等于没装。
# models.json 是出厂模型数据表（ADR-0009）：有就一起打包，只读；
# 用户接管版在 ~/.realagent/models/<name>.json。
set(RA_EXT_DIR $ENV{HOME}/.realagent/extensions)
foreach(p v1-messages deepseek perm-allow-all perm-ask core-tools)
  realugin_install_plugin(${p} ${RA_EXT_DIR} models.json)
endforeach()
