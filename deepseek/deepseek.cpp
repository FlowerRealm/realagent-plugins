/*
 * deepseek.cpp — DeepSeek 供应商壳（type = protocol）
 *
 * 纯套壳（ADR-0004 嵌套链的外层）：包住 v1-messages 协议层。壳只做两件事——
 *   1. 声明供应商身份：deps 声明包住 v1-messages，core 据此解析协议链入口；
 *   2. 兜底供应商默认配置：端点 / 模型，凭证（api_key）。
 *
 * 无任何供应商特殊逻辑：不做模型映射、不重写请求结构。协议层留空处填默认
 * （url 用供应商端点、model 用供应商默认、缺 Authorization 补凭证），其余原样透传。
 * 配置了就用配置（env > settings.json），没配才用默认。
 */
#include <plugin_api.h>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <cstring>
#include <string>

namespace bj = boost::json;

struct plugin_plugin {
    std::string base_url;      // init：config 或 DeepSeek 默认
    std::string api_key;
    std::string model;

    const plugin_api_t* inner_api = nullptr; // 内层 v1-messages（get_dependency 注入）
    plugin_t* inner_inst = nullptr;
};

/* ==================== 工具函数 ==================== */

/* 内层请求 → 最终请求：model 留空才填供应商默认（无映射），其余不动 */
static std::string refine_body(const char* inner_body, const std::string& default_model) {
    if (!inner_body) return "{}";
    boost::system::error_code ec;
    bj::value v = bj::parse(inner_body, ec);
    if (ec || !v.is_object()) return inner_body; // 解析失败原样透传
    auto& o = v.as_object();
    if (!o.contains("model") || bj::value_to<std::string>(o.at("model")).empty())
        o["model"] = default_model;
    return bj::serialize(v);
}

/* 内层 headers → 最终 headers：缺 Authorization 才补凭证（已配则原样） */
static std::string refine_headers(const char* inner_headers, const std::string& api_key) {
    if (!inner_headers) return "{}";
    boost::system::error_code ec;
    bj::value v = bj::parse(inner_headers, ec);
    if (ec || !v.is_object()) return inner_headers;
    auto& o = v.as_object();
    if (api_key.empty()) return bj::serialize(v);
    if (!o.contains("Authorization")) o["Authorization"] = "Bearer " + api_key;
    return bj::serialize(v);
}

/* ==================== 套壳：build_request / parse_feed ==================== */

static plugin_status_t build_request(plugin_t* self, const char* dialog_json,
                                     plugin_request_t* out) {
    auto* p = static_cast<plugin_plugin*>(self);
    if (!dialog_json || !out || !p->inner_api || !p->inner_inst) return PLUGIN_ERR;

    // 内层协议层构造初步请求
    plugin_request_t inner{};
    if (p->inner_api->build_request(p->inner_inst, dialog_json, &inner) != PLUGIN_OK)
        return PLUGIN_ERR;

    // 壳的唯一动作：兜底供应商默认配置（端点 / 模型 / 凭证），其余透传
    const std::string u = p->base_url + "/v1/messages";
    const std::string h = refine_headers(inner.headers, p->api_key);
    const std::string b = refine_body(inner.body, p->model);

    // 释放内层分配（走内层自己的 free），最终请求整体为本壳自有分配
    if (p->inner_api->free) {
        p->inner_api->free(p->inner_inst, const_cast<char*>(inner.url));
        p->inner_api->free(p->inner_inst, const_cast<char*>(inner.headers));
        p->inner_api->free(p->inner_inst, const_cast<char*>(inner.body));
    }

    char* url = new char[u.size() + 1];
    char* hdrs = new char[h.size() + 1];
    char* bstr = new char[b.size() + 1];
    memcpy(url, u.c_str(), u.size() + 1);
    memcpy(hdrs, h.c_str(), h.size() + 1);
    memcpy(bstr, b.c_str(), b.size() + 1);

    out->url = url;
    out->headers = hdrs;
    out->body = bstr;
    return PLUGIN_OK;
}

static plugin_status_t parse_feed(plugin_t* self, const char* chunk, plugin_event_sink_t sink,
                                  void* sink_ctx) {
    auto* p = static_cast<plugin_plugin*>(self);
    if (!p->inner_api || !p->inner_inst) return PLUGIN_ERR;
    return p->inner_api->parse_feed(p->inner_inst, chunk, sink, sink_ctx); // 纯透传
}

/* ==================== 生命周期 ==================== */

static plugin_status_t init(plugin_t* self, plugin_core_t* core) {
    auto* p = static_cast<plugin_plugin*>(self);
    p->base_url = core->api->get_config(core, "base_url");
    p->api_key = core->api->get_config(core, "api_key");
    p->model = core->api->get_config(core, "model");
    if (p->base_url.empty()) p->base_url = "https://api.deepseek.com/anthropic";
    if (p->model.empty()) p->model = "deepseek-v4-flash";

    if (!core->api->get_dependency) {
        core->api->log(core, PLUGIN_LOG_ERROR, "deepseek: core 缺 get_dependency（ABI 过旧）");
        return PLUGIN_ERR;
    }
    if (core->api->get_dependency(core, "v1-messages", &p->inner_api, &p->inner_inst) != PLUGIN_OK) {
        core->api->log(core, PLUGIN_LOG_ERROR, "deepseek: 缺少前置依赖插件 v1-messages");
        return PLUGIN_ERR;
    }
    if (!p->inner_api || !p->inner_api->build_request || !p->inner_api->parse_feed)
        return PLUGIN_ERR;
    return PLUGIN_OK;
}

static void destroy(plugin_t* self) {
    auto* p = static_cast<plugin_plugin*>(self);
    delete p;
}

static void plugin_free(plugin_t* self, void* ptr) {
    (void)self;
    delete[] static_cast<char*>(ptr);
}

static const plugin_api_t k_api = {
    .abi_version = PLUGIN_ABI_VERSION,
    .type = PLUGIN_TYPE_PROTOCOL,
    .name = "deepseek",
    .init = init,
    .destroy = destroy,
    .on_event = nullptr,
    .free = plugin_free,
    .execute_tool = nullptr,
    .decide = nullptr,
    .build_request = build_request,
    .parse_feed = parse_feed,
};

extern "C" PLUGIN_EXPORT plugin_t* plugin_create(const plugin_api_t** out_api) {
    auto* p = new plugin_plugin();
    if (!p) return nullptr;
    *out_api = &k_api;
    return p;
}
