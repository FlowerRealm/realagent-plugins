/*
 * core-tools.c — 内置工具插件（read / edit / bash）
 *
 * read  : 读文件 → 内容回传
 * edit  : 改文件；+x-0（空文件追加）= 创建文件（write 语义，无独立 write 工具）
 * bash  : 执行 shell 命令（stdout 回传）
 *
 * 结果 JSON：{"status": <0|非0>, "messages": <string|array>}
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <plugin_api.h>

struct plugin_plugin {
    /* 无状态 */
};

/* ==================== 最小 JSON 字符串字段提取 ==================== */

/* 从 JSON 对象文本中提取字符串字段值。
 * 找到返回 0 并写入 out（含 '\0'）；未找到/非法返回 -1。
 * 处理 \" \\ \n \t \r 转义。够用即可——参数就几个字符串字段。 */
static int json_str_field(const char* json, const char* key, char* out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return -1;
    const size_t klen = strlen(key);
    const char* p = json;
    while ((p = strstr(p, "\""))) {
        ++p; /* 越过引号 */
        if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
            p += klen + 1;
            while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
            if (*p != ':') return -1;
            ++p;
            while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
            if (*p != '"') return -1; /* 只支持字符串值 */
            ++p;
            size_t n = 0;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    ++p;
                    switch (*p) {
                        case 'n': out[n] = '\n'; break;
                        case 't': out[n] = '\t'; break;
                        case 'r': out[n] = '\r'; break;
                        default:  out[n] = *p;   break;
                    }
                    ++p;
                } else {
                    out[n] = *p++;
                }
                if (++n >= out_size - 1) break;
            }
            out[n] = '\0';
            return 0;
        }
    }
    return -1;
}

/* ==================== read ==================== */

#define MAX_OUT 50000 /* 输出截断：50KB（对齐工具输出限制约定） */

static plugin_tool_result_t do_read(const char* params_json) {
    char path[4096];
    plugin_tool_result_t r = {.status = 1, .messages = "{\"error\":\"missing file_path\"}"};
    if (json_str_field(params_json, "file_path", path, sizeof(path)) != 0) return r;

    FILE* f = fopen(path, "r");
    if (!f) {
        static char err[4200];
        snprintf(err, sizeof(err), "{\"error\":\"cannot open: %s\"}", path);
        r.messages = err;
        return r;
    }
    char* buf = (char*)malloc(MAX_OUT + 1);
    if (!buf) { fclose(f); r.messages = "{\"error\":\"oom\"}"; return r; }
    size_t n = fread(buf, 1, MAX_OUT, f);
    fclose(f);
    buf[n] = '\0';
    // 截断标记
    if (n == MAX_OUT) {
        size_t l = strlen(buf);
        memcpy(buf + l - 3, "...", 3);
    }
    r.status = 0;
    r.messages = buf; /* 插件分配；core 约定：静态或插件管理 */
    return r;
}

/* ==================== edit（+x-0 = 创建） ==================== */

static plugin_tool_result_t do_edit(const char* params_json) {
    char path[4096], old_s[32768], new_s[32768];
    plugin_tool_result_t r = {.status = 1, .messages = "{\"error\":\"missing file_path\"}"};
    if (json_str_field(params_json, "file_path", path, sizeof(path)) != 0) return r;
    if (json_str_field(params_json, "new_string", new_s, sizeof(new_s)) != 0) {
        r.messages = "{\"error\":\"missing new_string\"}";
        return r;
    }
    const int has_old = json_str_field(params_json, "old_string", old_s, sizeof(old_s)) == 0;

    FILE* f = fopen(path, "r");
    if (!f) {
        // 文件不存在 → 创建（write 语义：edit +x-0）
        f = fopen(path, "w");
        if (!f) {
            static char err[4200];
            snprintf(err, sizeof(err), "{\"error\":\"cannot create: %s\"}", path);
            r.messages = err;
            return r;
        }
        fwrite(new_s, 1, strlen(new_s), f);
        fclose(f);
        r.status = 0;
        r.messages = "{\"ok\":\"created\"}";
        return r;
    }
    // 文件存在：读全文
    char* content = (char*)malloc(MAX_OUT + 1);
    size_t n = fread(content, 1, MAX_OUT, f);
    fclose(f);
    content[n] = '\0';

    // old_string 为空 → 追加到末尾
    if (!has_old || old_s[0] == '\0') {
        f = fopen(path, "a");
        if (!f) { free(content); r.messages = "{\"error\":\"cannot append\"}"; return r; }
        fwrite(new_s, 1, strlen(new_s), f);
        fclose(f);
        free(content);
        r.status = 0;
        r.messages = "{\"ok\":\"appended\"}";
        return r;
    }
    // 替换第一个 old_string
    char* hit = strstr(content, old_s);
    if (!hit) {
        static char err[4200];
        snprintf(err, sizeof(err), "{\"error\":\"old_string not found\"}");
        free(content);
        r.messages = err;
        return r;
    }
    const size_t before = (size_t)(hit - content);
    const size_t old_len = strlen(old_s), new_len = strlen(new_s);
    size_t total = before + new_len + (n - before - old_len);
    char* out = (char*)malloc(total + 1);
    memcpy(out, content, before);
    memcpy(out + before, new_s, new_len);
    memcpy(out + before + new_len, content + before + old_len, n - before - old_len);
    out[total] = '\0';
    f = fopen(path, "w");
    if (f) {
        fwrite(out, 1, total, f);
        fclose(f);
        r.status = 0;
        r.messages = "{\"ok\":\"edited\"}";
    } else {
        r.messages = "{\"error\":\"cannot write\"}";
    }
    free(content);
    free(out);
    return r;
}

/* ==================== bash ==================== */

static plugin_tool_result_t do_bash(const char* params_json) {
    char cmd[8192];
    plugin_tool_result_t r = {.status = 1, .messages = "{\"error\":\"missing command\"}"};
    if (json_str_field(params_json, "command", cmd, sizeof(cmd)) != 0) return r;

    FILE* p = popen(cmd, "r");
    if (!p) { r.messages = "{\"error\":\"popen failed\"}"; return r; }
    char* buf = (char*)malloc(MAX_OUT + 1);
    size_t n = fread(buf, 1, MAX_OUT, p);
    int rc = pclose(p);
    buf[n] = '\0';
    if (n == MAX_OUT) {
        size_t l = strlen(buf);
        memcpy(buf + l - 3, "...", 3);
    }
    r.status = rc == 0 ? 0 : rc; /* 非零退出码视为错误 */
    r.messages = buf;
    return r;
}

/* ==================== 插件接口 ==================== */

static const plugin_tool_t k_tools[] = {
    {"read", "读文件", "读取指定文件的内容。路径不存在或不可读时返回错误。",
     "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"文件路径\"}},\"required\":[\"file_path\"]}",
     0},
    {"edit", "编辑文件", "修改文件内容。目标文件不存在时创建新文件（old_string 可为空表示创建/追加）；"
     "old_string 为空时把 new_string 追加到文件末尾；否则替换第一个匹配的 old_string。",
     "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\",\"description\":\"被替换的原文；为空=创建或追加\"},\"new_string\":{\"type\":\"string\"}},\"required\":[\"file_path\",\"new_string\"]}",
     1},
    {"bash", "执行命令", "在 shell 中执行命令并返回标准输出。危险操作需用户确认。",
     "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}",
     1},
};

static plugin_tool_result_t execute_tool(plugin_t* p, const char* call_id,
                                         const char* tool_name, const char* params_json) {
    (void)p;
    (void)call_id;
    if (strcmp(tool_name, "read") == 0) return do_read(params_json);
    if (strcmp(tool_name, "edit") == 0) return do_edit(params_json);
    if (strcmp(tool_name, "bash") == 0) return do_bash(params_json);
    plugin_tool_result_t r = {.status = 1, .messages = "{\"error\":\"unknown tool\"}"};
    return r;
}

static plugin_status_t init(plugin_t* p, plugin_core_t* core) {
    for (size_t i = 0; i < sizeof(k_tools) / sizeof(k_tools[0]); ++i) {
        if (core->api->register_tool(core, &k_tools[i]) != PLUGIN_OK) {
            core->api->log(core, PLUGIN_LOG_ERROR, "core-tools: register_tool 失败");
            return PLUGIN_ERR;
        }
    }
    return PLUGIN_OK;
}

static void destroy(plugin_t* p) { free(p); }
static void plugin_free(plugin_t* p, void* ptr) {
    (void)p;
    free(ptr);
}

static const plugin_api_t k_api = {
    .abi_version = PLUGIN_ABI_VERSION,
    .type = PLUGIN_TYPE_TOOL,
    .name = "core-tools",
    .init = init,
    .destroy = destroy,
    .on_event = NULL,
    .free = plugin_free,
    .execute_tool = execute_tool,
    .decide = NULL,
    .build_request = NULL,
    .parse_feed = NULL,
};

PLUGIN_EXPORT plugin_t* plugin_create(const plugin_api_t** out_api) {
    plugin_t* p = (plugin_t*)malloc(sizeof(struct plugin_plugin));
    if (!p) return NULL;
    *out_api = &k_api;
    return p;
}
