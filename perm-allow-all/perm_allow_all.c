/*
 * perm-allow-all.c — 永远允许的最小权限策略（ADR-0005）
 *
 * 目标不是安全，而是打通「core 检查点 → 权限插件裁决 → 工具执行」链路。
 * 真实审批策略（allow/deny/ask）在插件机制稳定后替换。
 */
#include <stdlib.h>

#include <realagent/agent_caps.h>

/* 插件实例（无状态，结构仅为持有类型） */
struct realugin_plugin {
    int dummy;
};

static realagent_permission_t decide(realugin_plugin_t* p, const char* tool_name, const char* params_json) {
    (void)p;
    (void)tool_name;
    (void)params_json;
    return REALAGENT_PERM_ALLOW; /* 永远允许 */
}

static realugin_status_t init(realugin_plugin_t* p, realugin_host_t* core) {
    (void)p;
    (void)core;
    return REALUGIN_OK;
}

static void destroy(realugin_plugin_t* p) { free(p); }

/* 能力表：一个能力，一个函数。借阅静态表，寿命 = 本容器在位时长 */
static const realugin_capability_t k_caps[] = {
    {REALAGENT_CAP_PERMISSION, (realugin_fn_t)decide},
};

static size_t capabilities(realugin_plugin_t* p, const realugin_capability_t** out) {
    (void)p;
    *out = k_caps;
    return sizeof(k_caps) / sizeof(k_caps[0]);
}

static const realugin_plugin_api_t k_api = {
    .abi_version = REALUGIN_ABI_VERSION,
    .name = "perm-allow-all",
    .init = init,
    .destroy = destroy,
    .capabilities = capabilities,
};

REALUGIN_EXPORT realugin_plugin_t* realugin_plugin_create(const realugin_plugin_api_t** out_api) {
    realugin_plugin_t* p = (realugin_plugin_t*)malloc(sizeof(struct realugin_plugin));
    if (!p) return NULL;
    *out_api = &k_api;
    return p;
}
