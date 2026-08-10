/*
 * perm-allow-all.c — 永远允许的最小权限策略（ADR-0005）
 *
 * 目标不是安全，而是打通「core 检查点 → 权限插件裁决 → 工具执行」链路。
 * 真实审批策略（allow/deny/ask）在插件机制稳定后替换。
 */
#include <stdlib.h>

#include <plugin_api.h>

/* 插件实例（无状态，结构仅为持有类型） */
struct plugin_plugin {
    int dummy;
};

static plugin_permission_t decide(plugin_t* p, const char* tool_name, const char* params_json) {
    (void)p;
    (void)tool_name;
    (void)params_json;
    return PLUGIN_PERM_ALLOW; /* 永远允许 */
}

static plugin_status_t init(plugin_t* p, plugin_core_t* core) {
    (void)p;
    (void)core;
    return PLUGIN_OK;
}

static void destroy(plugin_t* p) { free(p); }

static void plugin_free(plugin_t* p, void* ptr) {
    (void)p;
    free(ptr);
}

static const plugin_api_t k_api = {
    .abi_version = PLUGIN_ABI_VERSION,
    .type = PLUGIN_TYPE_PERMISSION,
    .name = "perm-allow-all",
    .init = init,
    .destroy = destroy,
    .on_event = NULL,
    .free = plugin_free,
    .execute_tool = NULL,
    .decide = decide,
    .build_request = NULL,
    .parse_feed = NULL,
};

PLUGIN_EXPORT plugin_t* plugin_create(const plugin_api_t** out_api) {
    plugin_t* p = (plugin_t*)malloc(sizeof(struct plugin_plugin));
    if (!p) return NULL;
    *out_api = &k_api;
    return p;
}
