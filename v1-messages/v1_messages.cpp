/*
 * v1_messages.cpp — /v1/messages 协议层插件（type = protocol）
 *
 * /v1/messages 是一种协议：Anthropic、DeepSeek、OpenRouter 等多家公司都实现同一端点。
 * 本插件不认识任何供应商——请求构造与 SSE 解析（含 thinking 块）都是协议固有内容，
 * 供应商差异（端点/模型/凭证默认值）由外层套壳插件（provider 壳）负责。
 *
 * 成对（ADR-0004）：
 *   build_request : 抽象对话（dialog_json）→ /v1/messages 请求体
 *   parse_feed    : SSE 响应 → 事件（thinking_start / thinking_update / thinking_stop /
 *                   message_update / tool_use / usage / stop）
 *
 * 配置（core 注入）：base_url / api_key / model，均不设供应商默认值。
 */
#include <plugin_api.h>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

namespace bj = boost::json;

struct plugin_plugin {
    std::string base_url;    // init 时从配置读入（可为空：供应商壳兜底）
    std::string api_key;
    std::string model;

    std::string buf;         // SSE 缓冲（未完整事件块）
    std::string block_type;  // 当前 content block 类型（text / thinking / tool_use）
    std::string tool_id;
    std::string tool_name;
    std::string tool_input;  // 累积 partial_json
    std::string thinking_sig;  // 当前 thinking 块的 signature（回传历史用）

    // 本条 message 的 token 计数（绝对值，message_start 清零后逐帧合并）
    int64_t u_input = 0;
    int64_t u_output = 0;
    int64_t u_cache_read = 0;
    int64_t u_cache_write = 0;
};

/* ==================== 工具函数 ==================== */

static std::string json_str(bj::value v) {
    return bj::serialize(v);
}

/* 取 usage 对象里的整数字段，缺失/类型不符按 0 —— 各家实现给的字段并不齐全 */
static int64_t usage_num(const bj::object& u, const char* key) {
    if (!u.contains(key)) return 0;
    const bj::value& v = u.at(key);
    return v.is_int64() ? v.as_int64() : (v.is_uint64() ? (int64_t)v.as_uint64() : 0);
}

/* 发一个 usage 事件：数值是"本条 message 到此为止的累计值"（绝对值，非增量）。
 * 绝对值语义让 core 覆盖写即可，丢帧不会造成永久偏差；累加只在 core 跨 turn 做一次。
 *
 * 各家在不同帧里给不同字段（input 在 message_start，output 在 message_delta），
 * 因此计数落在插件状态上合并后整体发出——下游永远收到完整一组，不必猜"0 是真的 0
 * 还是这帧没给"。全零不发（无 usage 信息的端点保持静默）。 */
static void merge_usage(plugin_plugin* p, const bj::object& u, plugin_event_sink_t sink,
                        void* sink_ctx) {
    if (usage_num(u, "input_tokens") > 0) p->u_input = usage_num(u, "input_tokens");
    if (usage_num(u, "output_tokens") > 0) p->u_output = usage_num(u, "output_tokens");
    if (usage_num(u, "cache_read_input_tokens") > 0)
        p->u_cache_read = usage_num(u, "cache_read_input_tokens");
    if (usage_num(u, "cache_creation_input_tokens") > 0)
        p->u_cache_write = usage_num(u, "cache_creation_input_tokens");
    if (!sink) return;
    if (p->u_input == 0 && p->u_output == 0 && p->u_cache_read == 0 && p->u_cache_write == 0) return;
    bj::object ev;
    ev["input"] = p->u_input;
    ev["output"] = p->u_output;
    ev["cache_read"] = p->u_cache_read;
    ev["cache_write"] = p->u_cache_write;
    sink(sink_ctx, "usage", json_str(ev).c_str());
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

    // messages：抽象对话 → /v1/messages 格式（合并相邻同 role）
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
                    } else if (bt == "thinking") {
                        // thinking 块（协议固有内容）原样回传，带 signature（缺失时省略）
                        out_block["type"] = "thinking";
                        out_block["thinking"] = b.at("thinking");
                        if (b.contains("signature")) out_block["signature"] = b.at("signature");
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

    // tools（抽象 → input_schema）
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
    if (!p->api_key.empty()) hdrs_obj["Authorization"] = "Bearer " + p->api_key;
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

/* 解析一个 SSE 事件块（event/data 对），产出发给 sink。
 * 本函数按 /v1/messages 的帧结构直取字段（.at / as_object / value_to 均会抛），
 * 帧一旦不合规就抛异常——包装层负责兜住，见 handle_sse_event。 */
static void handle_sse_event_impl(plugin_plugin* p, const std::string& event_type,
                                  const std::string& data, plugin_event_sink_t sink,
                                  void* sink_ctx) {
    boost::system::error_code ec;
    bj::value v = bj::parse(data, ec);
    if (ec) return;
    const bj::object& o = v.as_object();
    const std::string t = bj::value_to<std::string>(o.at("type"));

    if (t == "message_start") {
        // 新 message：计数清零，再合并首帧 usage（input_tokens 在这里给）
        p->u_input = p->u_output = p->u_cache_read = p->u_cache_write = 0;
        if (o.contains("message") && o.at("message").is_object()) {
            const auto& msg = o.at("message").as_object();
            if (msg.contains("usage") && msg.at("usage").is_object())
                merge_usage(p, msg.at("usage").as_object(), sink, sink_ctx);
        }
    } else if (t == "content_block_start") {
        const auto& cb = o.at("content_block").as_object();
        const std::string cbt = bj::value_to<std::string>(cb.at("type"));
        p->block_type = cbt;
        if (cbt == "tool_use") {
            p->tool_id = bj::value_to<std::string>(cb.at("id"));
            p->tool_name = bj::value_to<std::string>(cb.at("name"));
            p->tool_input.clear();
        } else if (cbt == "thinking") {
            // 思考块开始：先发 signature（Anthropic 格式），再发起始文本（部分端点起始块带文本）
            if (cb.contains("signature"))
                p->thinking_sig = bj::value_to<std::string>(cb.at("signature"));
            if (sink) {
                bj::object ev;
                ev["signature"] = p->thinking_sig;
                sink(sink_ctx, "thinking_start", json_str(ev).c_str());
            }
            if (cb.contains("thinking")) {
                const std::string init = bj::value_to<std::string>(cb.at("thinking"));
                if (!init.empty() && sink) {
                    bj::object ev;
                    ev["delta"] = init;
                    sink(sink_ctx, "thinking_update", json_str(ev).c_str());
                }
            }
        }
    } else if (t == "content_block_delta") {
        const auto& delta = o.at("delta").as_object();
        const std::string dt = bj::value_to<std::string>(delta.at("type"));
        if (dt == "text_delta" && sink) {
            const std::string text = bj::value_to<std::string>(delta.at("text"));
            bj::object ev;
            ev["delta"] = text;
            sink(sink_ctx, "message_update", json_str(ev).c_str());
        } else if (dt == "thinking_delta" && p->block_type == "thinking" && sink) {
            const std::string text = bj::value_to<std::string>(delta.at("thinking"));
            bj::object ev;
            ev["delta"] = text;
            sink(sink_ctx, "thinking_update", json_str(ev).c_str());
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
        } else if (p->block_type == "thinking") {
            if (sink) sink(sink_ctx, "thinking_stop", "{}");
            p->block_type.clear();
            p->thinking_sig.clear();
        }
    } else if (t == "message_delta") {
        // output_tokens 在这里给终值：先发 usage 再发 stop，保证下游收工时数字已定
        if (o.contains("usage") && o.at("usage").is_object())
            merge_usage(p, o.at("usage").as_object(), sink, sink_ctx);
        if (sink) {
            // delta 缺失/非对象不是丢帧的理由：stop 照发，reason 退回默认值。
            // （守卫方式与上面的 usage 一致——同一个帧里两套写法迟早漏一个。）
            bj::object ev;
            ev["reason"] = bj::string("stop");
            if (o.contains("delta") && o.at("delta").is_object()) {
                const auto& d = o.at("delta").as_object();
                if (d.contains("stop_reason")) ev["reason"] = d.at("stop_reason");
            }
            sink(sink_ctx, "stop", json_str(ev).c_str());
        }
    }
    (void)event_type;
}

/* ABI 边界（ADR-0001）：异常绝不能穿出去。core 经 C 函数指针调 parse_feed，
 * 中间还隔着 libcurl 的 C 栈帧——异常穿过去是未定义行为，实测直接 terminate
 * 整个常驻服务。
 *
 * 但也绝不能吞掉：跳过畸形帧 = 正文/tool_use 悄悄消失，core 收到一个"成功但空"的
 * 回答，用户看到什么都没发生且没有任何报错。那比崩溃更糟。
 * 异常在此转成失败返回值，由 parse_feed 报 PLUGIN_ERR，core 中止本次调用。 */
static bool handle_sse_event(plugin_plugin* p, const std::string& event_type,
                             const std::string& data, plugin_event_sink_t sink, void* sink_ctx) {
    try {
        handle_sse_event_impl(p, event_type, data, sink, sink_ctx);
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[v1-messages] SSE 帧不合规: %s | data=%.200s\n", e.what(), data.c_str());
    } catch (...) {
        fprintf(stderr, "[v1-messages] SSE 帧不合规（未知异常）| data=%.200s\n", data.c_str());
    }
    return false;
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
            if (!data.empty() && !handle_sse_event(p, event_type, data, sink, sink_ctx))
                return PLUGIN_ERR; // 帧不合规：报错给 core 中止本次调用，绝不静默跳过
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
        if (!data.empty() && !handle_sse_event(p, event_type, data, sink, sink_ctx))
            return PLUGIN_ERR; // 同上
    }
    return PLUGIN_OK;
}

/* ==================== 生命周期 ==================== */

static plugin_status_t init(plugin_t* self, plugin_core_t* core) {
    auto* p = static_cast<plugin_plugin*>(self);
    p->api_key = core->api->get_config(core, "api_key");
    p->base_url = core->api->get_config(core, "base_url");
    p->model = core->api->get_config(core, "model");
    // 不设供应商默认值：端点/模型由外层 provider 壳兜底
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
    .name = "v1-messages",
    .init = init,
    .destroy = destroy,
    .on_event = nullptr,
    .free = plugin_free,
    .execute_tool = nullptr,
    .decide = nullptr,
    .build_request = build_request,
    .parse_feed = parse_feed,
    // 协议层供应商中立：不认识任何模型，也就没有清单可报（ADR-0009）
    .list_models = nullptr,
};

extern "C" PLUGIN_EXPORT plugin_t* plugin_create(const plugin_api_t** out_api) {
    auto* p = new plugin_plugin();
    if (!p) return nullptr;
    *out_api = &k_api;
    return p;
}
