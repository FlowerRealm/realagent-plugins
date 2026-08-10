/*
 * deepseek_messages.cpp — Anthropic /v1/messages 协议插件（type = protocol）
 *
 * 成对（ADR-0004）：
 *   build_request : 抽象对话（dialog_json）→ Anthropic /v1/messages 请求体
 *   parse_feed    : SSE 响应 → 事件（message_update / tool_use / stop）
 *
 * 端点/模型可配（core 注入）：base_url / api_key / model
 */
#include <plugin_api.h>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <cstring>
#include <string>

namespace bj = boost::json;

struct plugin_plugin {
    std::string base_url;    // init 时从配置读入
    std::string api_key;
    std::string model;

    std::string buf;         // SSE 缓冲（未完整事件块）
    std::string block_type;  // 当前 content block 类型（text / tool_use）
    std::string tool_id;
    std::string tool_name;
    std::string tool_input;  // 累积 partial_json
};

/* ==================== 工具函数 ==================== */

static std::string json_str(bj::value v) {
    return bj::serialize(v);
}

/* ==================== build_request ==================== */

static plugin_status_t build_request(plugin_t* self, const char* dialog_json, plugin_request_t* out) {
    auto* p = static_cast<plugin_plugin*>(self);
    if (!dialog_json || !out) return PLUGIN_ERR;

    boost::system::error_code ec;
    bj::value dialog = bj::parse(dialog_json, ec);
    if (ec) return PLUGIN_ERR;

    const bj::object& d = dialog.as_object();
    const std::string model = d.contains("model") ? bj::value_to<std::string>(d.at("model"))
                                                  : p->model;
    const std::string system = d.contains("system") ? bj::value_to<std::string>(d.at("system")) : "";

    bj::object body;
    body["model"] = model;
    body["max_tokens"] = 4096;
    body["stream"] = true;

    // system（Anthropic 格式：字符串或 block 数组）
    if (!system.empty()) body["system"] = system;

    // messages：抽象对话 → Anthropic 格式（合并相邻同 role）
    bj::array msgs;
    if (d.contains("messages") && d.at("messages").is_array()) {
        for (const auto& m : d.at("messages").as_array()) {
            const bj::object& mo = m.as_object();
            const std::string role = bj::value_to<std::string>(mo.at("role"));
            bj::array blocks;
            if (mo.contains("content") && mo.at("content").is_array()) {
                for (const auto& blk : mo.at("content").as_array()) {
                    const bj::object& b = blk.as_object();
                    const std::string bt = bj::value_to<std::string>(b.at("type"));
                    bj::object out_block;
                    if (bt == "text") {
                        out_block["type"] = "text";
                        out_block["text"] = b.at("text");
                    } else if (bt == "tool_use") {
                        out_block["type"] = "tool_use";
                        out_block["id"] = b.at("id");
                        out_block["name"] = b.at("name");
                        out_block["input"] = b.contains("input") ? b.at("input") : bj::object{};
                    } else if (bt == "tool_result") {
                        out_block["type"] = "tool_result";
                        out_block["tool_use_id"] = b.at("tool_use_id");
                        out_block["content"] = b.at("content");
                        if (b.contains("is_error") && b.at("is_error").as_bool())
                            out_block["is_error"] = true;
                    }
                    blocks.push_back(out_block);
                }
            }
            // 合并相邻同 role：若上一条 message 同 role，并入其 content
            if (!msgs.empty() && msgs.back().as_object().at("role").as_string() == role) {
                auto& last = msgs.back().as_object();
                auto& last_blocks = last.at("content").as_array();
                for (auto& blk : blocks) last_blocks.push_back(blk);
            } else {
                bj::object mout;
                mout["role"] = role;
                mout["content"] = blocks;
                msgs.push_back(mout);
            }
        }
    }
    body["messages"] = msgs;

    // tools（抽象 → Anthropic input_schema）
    if (d.contains("tools") && d.at("tools").is_array()) {
        bj::array tools;
        for (const auto& t : d.at("tools").as_array()) {
            const bj::object& to = t.as_object();
            bj::object tool;
            tool["name"] = to.at("name");
            if (to.contains("description")) tool["description"] = to.at("description");
            tool["input_schema"] = to.contains("input_schema") ? to.at("input_schema") : bj::object{};
            tools.push_back(tool);
        }
        body["tools"] = tools;
        bj::object tc;
        tc["type"] = "auto";
        body["tool_choice"] = tc;
    }

    // —— 请求结果（插件 new char[] 分配；core 用完调 api->free 释放 = delete[]） ——
    const std::string u = p->base_url + "/v1/messages";
    bj::object hdrs_obj;
    hdrs_obj["Authorization"] = "Bearer " + p->api_key;
    hdrs_obj["Content-Type"] = "application/json";
    hdrs_obj["anthropic-version"] = "2023-06-01";
    const std::string h = bj::serialize(hdrs_obj);
    const std::string b = bj::serialize(body);

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

/* ==================== parse_feed（SSE 解析） ==================== */

/* 解析一个 SSE 事件块（event/data 对），产出发给 sink */
static void handle_sse_event(plugin_plugin* p, const std::string& event_type,
                             const std::string& data, plugin_event_sink_t sink, void* sink_ctx) {
    boost::system::error_code ec;
    bj::value v = bj::parse(data, ec);
    if (ec) return;
    const bj::object& o = v.as_object();
    const std::string t = bj::value_to<std::string>(o.at("type"));

    if (t == "content_block_start") {
        const auto& cb = o.at("content_block").as_object();
        const std::string cbt = bj::value_to<std::string>(cb.at("type"));
        p->block_type = cbt;
        if (cbt == "tool_use") {
            p->tool_id = bj::value_to<std::string>(cb.at("id"));
            p->tool_name = bj::value_to<std::string>(cb.at("name"));
            p->tool_input.clear();
        }
    } else if (t == "content_block_delta") {
        const auto& delta = o.at("delta").as_object();
        const std::string dt = bj::value_to<std::string>(delta.at("type"));
        if (dt == "text_delta" && sink) {
            const std::string text = bj::value_to<std::string>(delta.at("text"));
            bj::object ev;
            ev["delta"] = text;
            sink(sink_ctx, "message_update", json_str(ev).c_str());
        } else if (dt == "input_json_delta" && p->block_type == "tool_use") {
            p->tool_input += bj::value_to<std::string>(delta.at("partial_json"));
        }
    } else if (t == "content_block_stop") {
        if (p->block_type == "tool_use" && sink) {
            bj::object ev;
            ev["id"] = p->tool_id;
            ev["name"] = p->tool_name;
            boost::system::error_code iec;
            ev["input"] = bj::parse(p->tool_input, iec);
            if (iec) ev["input"] = bj::object{};
            sink(sink_ctx, "tool_use", json_str(ev).c_str());
            p->block_type.clear();
        }
    } else if (t == "message_delta") {
        if (sink) {
            const auto& d = o.at("delta").as_object();
            bj::object ev;
            ev["reason"] = d.contains("stop_reason") ? d.at("stop_reason")
                                                     : bj::string("stop");
            sink(sink_ctx, "stop", json_str(ev).c_str());
        }
    }
    (void)event_type;
}

static plugin_status_t parse_feed(plugin_t* self, const char* chunk, plugin_event_sink_t sink,
                                  void* sink_ctx) {
    auto* p = static_cast<plugin_plugin*>(self);
    if (!chunk) {
        // flush：剩余缓冲不再有完整事件，忽略
        return PLUGIN_OK;
    }
    p->buf += chunk;
    // 按空行切 SSE 事件块
    for (;;) {
        const size_t pos = p->buf.find("\n\n");
        if (pos == std::string::npos) {
            // 可能 \r\n 分隔
            const size_t pos2 = p->buf.find("\r\n\r\n");
            if (pos2 == std::string::npos) break;
            std::string block = p->buf.substr(0, pos2);
            p->buf.erase(0, pos2 + 4);
            std::string event_type, data;
            std::string::size_type i = 0;
            while (i < block.size()) {
                const auto nl = block.find('\n', i);
                const std::string line =
                    nl == std::string::npos ? block.substr(i) : block.substr(i, nl - i);
                i = nl == std::string::npos ? block.size() : nl + 1;
                if (line.rfind("event:", 0) == 0) event_type = line.substr(6);
                else if (line.rfind("data:", 0) == 0) data = line.substr(5);
            }
            if (!data.empty()) handle_sse_event(p, event_type, data, sink, sink_ctx);
            continue;
        }
        std::string block = p->buf.substr(0, pos);
        p->buf.erase(0, pos + 2);
        std::string event_type, data;
        std::string::size_type i = 0;
        while (i < block.size()) {
            const auto nl = block.find('\n', i);
            const std::string line =
                nl == std::string::npos ? block.substr(i) : block.substr(i, nl - i);
            i = nl == std::string::npos ? block.size() : nl + 1;
            if (line.rfind("event:", 0) == 0) event_type = line.substr(6);
            else if (line.rfind("data:", 0) == 0) data = line.substr(5);
        }
        if (!data.empty()) handle_sse_event(p, event_type, data, sink, sink_ctx);
    }
    return PLUGIN_OK;
}

/* ==================== 生命周期 ==================== */

static plugin_status_t init(plugin_t* self, plugin_core_t* core) {
    auto* p = static_cast<plugin_plugin*>(self);
    p->api_key = core->api->get_config(core, "api_key");
    p->base_url = core->api->get_config(core, "base_url");
    p->model = core->api->get_config(core, "model");
    if (p->base_url.empty()) p->base_url = "https://api.deepseek.com/anthropic";
    if (p->model.empty()) p->model = "deepseek-v4-flash";
    if (p->api_key.empty()) {
        core->api->log(core, PLUGIN_LOG_ERROR,
                       "deepseek-messages: 缺少 api_key（env ANTHROPIC_API_KEY 或 settings.json）");
        return PLUGIN_ERR;
    }
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
    .name = "deepseek-messages",
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
