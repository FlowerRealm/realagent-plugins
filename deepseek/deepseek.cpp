/*
 * deepseek.cpp — DeepSeek 供应商壳（type = protocol）
 *
 * 套壳（ADR-0004 嵌套链的外层）：包住 v1-messages 协议层。壳做三件事——
 *   1. 声明供应商身份：deps 声明包住 v1-messages，core 据此解析协议链入口；
 *   2. 兜底供应商默认配置：端点 / 模型，凭证（api_key）；
 *   3. 计价（ADR-0009）：模型数据表是本壳自己的数据（自读自解析），拦下内层解析出的
 *      token 用量，按本次模型的单价算出钱，只向上报 status_update {"cost"}。
 *      **token 到此为止不再上传**——core 不认识 token，也不该认识。
 *
 * 无任何供应商特殊逻辑：不做模型映射、不重写请求结构。协议层留空处填默认
 * （url 用供应商端点、model 用供应商默认、缺 Authorization 补凭证），其余原样透传。
 *
 * 模型数据表路径由 core 给（get_config("models_path")）：用户接管版存在就是它，
 * 否则是包内出厂版。表里有什么字段是本壳的事，core 只收 list_models 报上去的
 * name/owned_by/context——单价不报。
 */
#include <plugin_api.h>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

namespace bj = boost::json;

struct plugin_plugin {
    std::string base_url;      // init：config 或 DeepSeek 默认
    std::string api_key;
    std::string model;

    /* 模型数据表（本壳自有）：单价按模型名索引，键名随供应商，本壳不解释 */
    std::unordered_map<std::string, bj::object> pricing;
    std::string models_json;   // 报给 core 的清单（去掉单价），list_models 返回它
    std::string cur_model;     // 本次调用生效的模型名（build_request 记，算钱时查表用）

    const plugin_api_t* inner_api = nullptr; // 内层 v1-messages（get_dependency 注入）
    plugin_t* inner_inst = nullptr;
};

/* ==================== 工具函数 ==================== */

/* 内层请求 → 最终请求：model 留空才填供应商默认（无映射），其余不动。
 * 顺手记下本次生效的模型名——算钱要按它查单价。 */
static std::string refine_body(plugin_plugin* p, const char* inner_body) {
    if (!inner_body) return "{}";
    boost::system::error_code ec;
    bj::value v = bj::parse(inner_body, ec);
    if (ec || !v.is_object()) return inner_body; // 解析失败原样透传
    auto& o = v.as_object();
    if (!o.contains("model") || bj::value_to<std::string>(o.at("model")).empty())
        o["model"] = p->model;
    p->cur_model = bj::value_to<std::string>(o.at("model"));
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
    const std::string b = refine_body(p, inner.body);

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

/* 算钱：token 用量 × 本次模型的单价，同名键点积 / 1M。
 * 键名两边同源（都是本供应商的口径），本壳不认识具体是哪些键，也不需要认识。
 * 表里没这个模型 / 没这个键 → 该维度不计，不猜、不兜底。 */
static double price(plugin_plugin* p, const char* usage_json) {
    const auto it = p->pricing.find(p->cur_model);
    if (it == p->pricing.end() || !usage_json) return 0;
    boost::system::error_code ec;
    const bj::value v = bj::parse(usage_json, ec);
    if (ec || !v.is_object()) return 0;
    double total = 0;
    for (const auto& [k, tokens] : v.as_object()) {
        const auto* unit = it->second.if_contains(k);
        if (!unit || !unit->is_number() || !tokens.is_number()) continue;
        total += tokens.to_number<double>() * unit->to_number<double>() / 1e6;
    }
    return total;
}

/* 事件透传夹层：usage 拦下换成钱，其余原样往上走 */
struct SinkCtx {
    plugin_plugin* p;
    plugin_event_sink_t out;
    void* out_ctx;
};

static void shell_sink(void* ctx, const char* type, const char* payload) {
    auto* s = static_cast<SinkCtx*>(ctx);
    if (type && std::strcmp(type, "usage") == 0) {
        // token 到此为止：只把钱报上去（ADR-0009）。算不出钱就什么都不报——
        // 没数据就是没数据，不发 0
        const double cost = price(s->p, payload);
        if (cost > 0) {
            bj::object o;
            o["cost"] = cost;
            s->out(s->out_ctx, "status_update", bj::serialize(o).c_str());
        }
        return;
    }
    s->out(s->out_ctx, type, payload);
}

static plugin_status_t parse_feed(plugin_t* self, const char* chunk, plugin_event_sink_t sink,
                                  void* sink_ctx) {
    auto* p = static_cast<plugin_plugin*>(self);
    if (!p->inner_api || !p->inner_inst) return PLUGIN_ERR;
    SinkCtx ctx{p, sink, sink_ctx};
    return p->inner_api->parse_feed(p->inner_inst, chunk, shell_sink, &ctx);
}

/* 模型清单：只报 core 用得着的三个字段，单价留在壳里 */
static const char* list_models(plugin_t* self) {
    auto* p = static_cast<plugin_plugin*>(self);
    return p->models_json.empty() ? nullptr : p->models_json.c_str();
}

/* ==================== 生命周期 ==================== */

/* 读模型数据表：路径由 core 给（用户接管版优先，否则包内出厂版）。
 * 严格解析——文件在但读不动/字段缺，就是加载失败，不跳过坏条目、不补默认值。 */
static bool load_models(plugin_plugin* p, plugin_core_t* core, const char* path) {
    std::ifstream f(path);
    if (!f) return true; // 没有表：不算钱、不报清单。表本来就是可选的
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    boost::system::error_code ec;
    const bj::value v = bj::parse(text, ec);
    if (ec || !v.is_array()) {
        core->api->log(core, PLUGIN_LOG_ERROR, "deepseek: 模型数据表不是 JSON 数组");
        return false;
    }
    bj::array pub; // 报给 core 的清单：去掉单价
    for (const auto& item : v.as_array()) {
        if (!item.is_object()) {
            core->api->log(core, PLUGIN_LOG_ERROR, "deepseek: 模型数据表条目不是对象");
            return false;
        }
        const auto& o = item.as_object();
        const auto* name = o.if_contains("name");
        const auto* owned_by = o.if_contains("owned_by");
        const auto* context = o.if_contains("context");
        const auto* pricing = o.if_contains("pricing");
        if (!name || !name->is_string() || !owned_by || !owned_by->is_string() || !context ||
            !context->is_int64() || !pricing || !pricing->is_object()) {
            core->api->log(core, PLUGIN_LOG_ERROR,
                           "deepseek: 模型数据表条目缺字段（name/owned_by/context/pricing）");
            return false;
        }
        p->pricing[bj::value_to<std::string>(*name)] = pricing->as_object();
        bj::object e;
        e["name"] = *name;
        e["owned_by"] = *owned_by;
        e["context"] = *context;
        pub.push_back(std::move(e));
    }
    p->models_json = bj::serialize(pub);
    return true;
}

static plugin_status_t init(plugin_t* self, plugin_core_t* core) {
    auto* p = static_cast<plugin_plugin*>(self);
    p->base_url = core->api->get_config(core, "base_url");
    p->api_key = core->api->get_config(core, "api_key");
    p->model = core->api->get_config(core, "model");
    if (p->base_url.empty()) p->base_url = "https://api.deepseek.com/anthropic";
    if (p->model.empty()) p->model = "deepseek-v4-flash";

    if (!load_models(p, core, core->api->get_config(core, "models_path"))) return PLUGIN_ERR;

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
    .list_models = list_models,
};

extern "C" PLUGIN_EXPORT plugin_t* plugin_create(const plugin_api_t** out_api) {
    auto* p = new plugin_plugin();
    if (!p) return nullptr;
    *out_api = &k_api;
    return p;
}
