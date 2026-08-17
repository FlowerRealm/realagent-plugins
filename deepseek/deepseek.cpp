/*
 * deepseek.cpp — DeepSeek 供应商容器
 *
 * 在管线上提供两段（ADR-0012），外加一份模型清单：
 *   request.refine : 收一个粗请求，还一个能真发出去的请求——补端点、默认模型名、凭证
 *   usage.meter    : 收 token 用量，还一个钱数（USD）
 *   model.list     : 报模型清单（name/owned_by/context，**单价不报**）
 *
 * 它**不认识任何别的插件**：不知道粗请求是谁生成的，也不去调谁。装饰、接管、嵌套
 * 在这个模型里都不存在——收一个请求、还一个请求，就这么回事。
 *
 * 无任何供应商特殊逻辑：不做模型映射、不重写请求结构。只填空处（url 补端点、
 * body.model 空则填默认、headers 缺 Authorization 则补），其余原样透传。
 *
 * 模型数据表路径由 core 给（get_config("models_path")）：用户接管版存在就是它，
 * 否则是包内出厂版。表里有什么字段是本容器的事，core 只收报上去的三个字段。
 */
#include <realagent/agent_caps.h>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

namespace bj = boost::json;

struct realugin_plugin {
    realugin_host_t* core = nullptr; // init 存下：转移类内存要经 core->api->alloc
    std::string base_url;
    std::string api_key;
    std::string model;

    /* 模型数据表（本容器自有）：单价按模型名索引，键名随供应商，core 不解释 */
    std::unordered_map<std::string, bj::object> pricing;
    std::string models_json; // 报给 core 的清单（去掉单价），model.list 借阅它
    std::string cur_model;   // 本次生效的模型名（refine 时记下，计价时查表用）
};

/* 交给 core 的字符串经 core->api->alloc 分配、由 core 释放（转移，ADR-0012） */
static char* core_dup(realugin_plugin* p, const std::string& s) {
    char* out = static_cast<char*>(p->core->api->alloc(p->core, s.size() + 1));
    if (out) memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

/* ==================== request.refine ==================== */

/* 粗请求 → 精请求：只填空处。
 *   url     : 粗请求给的是路径（协议层不知道端点），拼上本供应商的 base_url
 *   body    : model 留空才填默认（不做映射：claude-* 原样透传）
 *   headers : 缺 Authorization 才补凭证
 * 顺手记下本次生效的模型名——计价要按它查单价。 */
static const char* refine(realugin_plugin_t* self, const char* request_json) {
    auto* p = static_cast<realugin_plugin*>(self);
    if (!request_json) return nullptr;
    boost::system::error_code ec;
    bj::value v = bj::parse(request_json, ec);
    if (ec || !v.is_object()) return nullptr;
    auto& req = v.as_object();

    // url：粗请求里是相对路径，前面补上端点
    const std::string path = req.contains("url") ? bj::value_to<std::string>(req.at("url")) : "";
    req["url"] = path.rfind("http", 0) == 0 ? path : p->base_url + path;

    if (req.contains("body") && req.at("body").is_object()) {
        auto& body = req.at("body").as_object();
        if (!body.contains("model") || bj::value_to<std::string>(body.at("model")).empty())
            body["model"] = p->model;
        p->cur_model = bj::value_to<std::string>(body.at("model"));
    }
    if (!p->api_key.empty() && req.contains("headers") && req.at("headers").is_object()) {
        auto& h = req.at("headers").as_object();
        if (!h.contains("Authorization")) h["Authorization"] = "Bearer " + p->api_key;
    }
    return core_dup(p, bj::serialize(v));
}

/* ==================== usage.meter ==================== */

/* 算钱：token 用量 × 本次模型的单价，同名键点积 / 1M。
 * 键名两边同源（都是本供应商的口径），本容器不认识具体是哪些键，也不需要认识。
 * 表里没这个模型 / 没这个键 → 该维度不计，不猜、不兜底。算不出返回 0（core 不发 cost）。 */
static double meter(realugin_plugin_t* self, const char* usage_json) {
    auto* p = static_cast<realugin_plugin*>(self);
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

/* ==================== model.list ==================== */

/* 借阅：指向本容器自有内存，寿命 = 容器在位时长，core 读完即用、不释放（ADR-0012） */
static const char* model_list(realugin_plugin_t* self) {
    auto* p = static_cast<realugin_plugin*>(self);
    return p->models_json.empty() ? nullptr : p->models_json.c_str();
}

/* ==================== 生命周期 ==================== */

/* 读模型数据表：路径由 core 给（用户接管版优先，否则包内出厂版）。
 * 严格解析——文件在但读不动/字段缺，就是加载失败，不跳过坏条目、不补默认值。 */
static bool load_models(realugin_plugin* p, realugin_host_t* core, const char* path) {
    std::ifstream f(path);
    if (!f) return true; // 没有表：不算钱、不报清单。表本来就是可选的
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    boost::system::error_code ec;
    const bj::value v = bj::parse(text, ec);
    if (ec || !v.is_array()) {
        core->api->log(core, REALUGIN_LOG_ERROR, "deepseek: 模型数据表不是 JSON 数组");
        return false;
    }
    bj::array pub; // 报给 core 的清单：去掉单价
    for (const auto& item : v.as_array()) {
        if (!item.is_object()) {
            core->api->log(core, REALUGIN_LOG_ERROR, "deepseek: 模型数据表条目不是对象");
            return false;
        }
        const auto& o = item.as_object();
        const auto* name = o.if_contains("name");
        const auto* owned_by = o.if_contains("owned_by");
        const auto* context = o.if_contains("context");
        const auto* pricing = o.if_contains("pricing");
        if (!name || !name->is_string() || !owned_by || !owned_by->is_string() || !context ||
            !context->is_int64() || !pricing || !pricing->is_object()) {
            core->api->log(core, REALUGIN_LOG_ERROR,
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

static realugin_status_t init(realugin_plugin_t* self, realugin_host_t* core) {
    auto* p = static_cast<realugin_plugin*>(self);
    p->core = core;
    p->base_url = core->api->get_config(core, "base_url");
    p->api_key = core->api->get_config(core, "api_key");
    p->model = core->api->get_config(core, "model");
    if (p->base_url.empty()) p->base_url = "https://api.deepseek.com/anthropic";
    if (p->model.empty()) p->model = "deepseek-v4-flash";

    if (!load_models(p, core, core->api->get_config(core, "models_path"))) return REALUGIN_ERR;

    // 看一眼现在有没有人能生成请求——没有的话本容器无从精修，早说比晚崩好。
    // 问的是能力，不是某个具体插件：谁生成的本容器不关心（ADR-0012）
    const char* const* names = nullptr;
    if (core->api->providers(core, REALAGENT_CAP_REQUEST_BUILD, &names) == 0) {
        core->api->log(core, REALUGIN_LOG_ERROR,
                       "deepseek: 没有任何容器能生成请求，本容器无从精修");
        return REALUGIN_ERR;
    }
    return REALUGIN_OK;
}

static void destroy(realugin_plugin_t* self) { delete static_cast<realugin_plugin*>(self); }

/* 能力表：三段，各一个函数。借阅静态表，寿命 = 容器在位时长 */
static const realugin_capability_t k_caps[] = {
    {REALAGENT_CAP_REQUEST_REFINE, (realugin_fn_t)refine},
    {REALAGENT_CAP_USAGE_METER,    (realugin_fn_t)meter},
    {REALAGENT_CAP_MODEL_LIST,     (realugin_fn_t)model_list},
};

static size_t capabilities(realugin_plugin_t* self, const realugin_capability_t** out) {
    (void)self;
    *out = k_caps;
    return sizeof(k_caps) / sizeof(k_caps[0]);
}

static const realugin_plugin_api_t k_api = {
    .abi_version = REALUGIN_ABI_VERSION,
    .name = "deepseek",
    .init = init,
    .destroy = destroy,
    .capabilities = capabilities,
};

extern "C" REALUGIN_EXPORT realugin_plugin_t* realugin_plugin_create(const realugin_plugin_api_t** out_api) {
    auto* p = new realugin_plugin();
    if (!p) return nullptr;
    *out_api = &k_api;
    return p;
}
