/*
 * perm-ask.c — 危险工具一律询问用户（ADR-0005）
 *
 * 与 perm-allow-all 相反：decide 永远返回 PLUGIN_PERM_ASK，
 * 触发 core → 客户端的审批链路（TUI 确认框）。目标不是安全，
 * 而是验证「core 检查点 → 权限插件裁决 → ASK → 用户裁决」全链路。
 */
#include <stdlib.h>

#include <realugin/plugin_api.h>

/* 插件实例（无状态，结构仅为持有类型） */
struct plugin_plugin {
    int dummy;
};

static plugin_permission_t decide(plugin_t* p, const char* tool_name, const char* params_json) {
    (void)p;
    (void)tool_name;
    (void)params_json;
    return PLUGIN_PERM_ASK; /* 一律询问用户 */
}

static plugin_status_t init(plugin_t* p, plugin_core_t* core) {
    (void)p;
    (void)core;
    return PLUGIN_OK;
}

static void destroy(plugin_t* p) { free(p); }

/* 能力表：一个能力，一个函数。借阅静态表，寿命 = 本容器在位时长 */
static const plugin_capability_t k_caps[] = {
    {PLUGIN_CAP_PERMISSION, (plugin_fn_t)decide},
};

static size_t capabilities(plugin_t* p, const plugin_capability_t** out) {
    (void)p;
    *out = k_caps;
    return sizeof(k_caps) / sizeof(k_caps[0]);
}

static const plugin_api_t k_api = {
    .abi_version = PLUGIN_ABI_VERSION,
    .name = "perm-ask",
    .init = init,
    .destroy = destroy,
    .capabilities = capabilities,
};

PLUGIN_EXPORT plugin_t* plugin_create(const plugin_api_t** out_api) {
    plugin_t* p = (plugin_t*)malloc(sizeof(struct plugin_plugin));
    if (!p) return NULL;
    *out_api = &k_api;
    return p;
}
