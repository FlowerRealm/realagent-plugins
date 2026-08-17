/*
 * core-tools.c — 内置工具插件（read / edit / bash）
 *
 * read  : 读文件 → 内容回传
 * edit  : 改文件；+x-0（空文件追加）= 创建文件（write 语义，无独立 write 工具）
 * bash  : 执行 shell 命令（stdout 边跑边推 tool_output 帧，完整输出仍在结果里回传；可中断）
 *
 * 结果 JSON：{"status": <0|非0>, "messages": <string|array>}
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <realagent/agent_caps.h>

struct realugin_plugin {
    realugin_host_t* core; /* 交给 core 的结果内存要经 core->api->alloc（ADR-0012） */
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

static realagent_result_t do_read(const char* params_json) {
    char path[4096];
    realagent_result_t r = {.status = 1, .messages = "{\"error\":\"missing file_path\"}"};
    if (json_str_field(params_json, "file_path", path, sizeof(path)) != 0) return r;

    FILE* f = fopen(path, "r");
    if (!f) {
        static char err[4200];
        snprintf(err, sizeof(err), "{\"error\":\"cannot open: %s\"}", path);
        r.messages = err;
        return r;
    }
    static char buf[MAX_OUT + 1]; /* 结果随即被 own() 拷进 core 内存，此处不必堆分配 */
    size_t n = fread(buf, 1, MAX_OUT, f);
    fclose(f);
    buf[n] = '\0';
    // 截断标记
    if (n == MAX_OUT) {
        size_t l = strlen(buf);
        memcpy(buf + l - 3, "...", 3);
    }
    r.status = 0;
    r.messages = buf;
    return r;
}

/* ==================== edit（+x-0 = 创建） ==================== */

static realagent_result_t do_edit(const char* params_json) {
    char path[4096], old_s[32768], new_s[32768];
    realagent_result_t r = {.status = 1, .messages = "{\"error\":\"missing file_path\"}"};
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

#define LINE_MAX_LEN 4096

/* JSON 字符串转义（写进 "..." 之间的那一段）。返回写入长度。
 * 只管必须管的：引号、反斜杠、控制字符。非 ASCII 原样透传——那是命令的输出，
 * 不是我们编的，不该在这里被改写。 */
static size_t json_escape(const char* s, size_t n, char* out, size_t cap) {
    size_t w = 0;
    for (size_t i = 0; i < n && w + 8 < cap; ++i) {
        const unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  out[w++] = '\\'; out[w++] = '"';  break;
            case '\\': out[w++] = '\\'; out[w++] = '\\'; break;
            case '\n': out[w++] = '\\'; out[w++] = 'n';  break;
            case '\r': out[w++] = '\\'; out[w++] = 'r';  break;
            case '\t': out[w++] = '\\'; out[w++] = 't';  break;
            default:
                if (c < 0x20) w += (size_t)snprintf(out + w, cap - w, "\\u%04x", c);
                else out[w++] = (char)c;
        }
    }
    out[w] = '\0';
    return w;
}

/* tool_output 帧（PROTOCOL.md）：命令还在跑的时候就把 stdout 推出去。
 * 与 tool_result 不是二选一——完整输出照旧在结果里回传，这里推的是"现在长什么样"。
 * call_id 是认领凭据：客户端靠它把这些碎片挂到对应那次工具调用下面。 */
static void emit_output(realugin_plugin_t* p, const char* call_id, const char* text, size_t len) {
    realugin_host_t* core = ((struct realugin_plugin*)p)->core;
    if (!core) return;
    char esc_text[LINE_MAX_LEN * 6 + 8], esc_id[512];
    char payload[sizeof(esc_text) + sizeof(esc_id) + 64];
    json_escape(text, len, esc_text, sizeof(esc_text));
    json_escape(call_id ? call_id : "", strlen(call_id ? call_id : ""), esc_id, sizeof(esc_id));
    snprintf(payload, sizeof(payload),
             "{\"call_id\":\"%s\",\"stream\":\"stdout\",\"text\":\"%s\"}", esc_id, esc_text);
    core->api->emit(core, "tool_output", payload);
}

/* 在跑的 bash 子进程组（0 = 手上没有）。中止请求从事件循环线程进来（tool.interrupt），
 * 读循环在 agent 线程——锁护住"登记"与"取用"不交错，否则会朝一个已经回收的 pid 开枪。
 * 顺序执行（ADR-0002）保证同时至多一个，一个数字就够，不需要表。 */
static pthread_mutex_t g_bash_mtx = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_bash_pgid = 0;
static volatile sig_atomic_t g_bash_killed = 0; /* 本次调用挨过刀：收尾时要盯着它死透 */

static realagent_result_t do_bash(realugin_plugin_t* p, const char* call_id, const char* params_json) {
    char cmd[8192];
    realagent_result_t r = {.status = 1, .messages = "{\"error\":\"missing command\"}"};
    if (json_str_field(params_json, "command", cmd, sizeof(cmd)) != 0) return r;

    int fds[2];
    if (pipe(fds) != 0) { r.messages = "{\"error\":\"pipe failed\"}"; return r; }

    /* 不用 popen：拿不到 pid 就没法中止。fork 前后都在锁里，中止请求要么在开工之前
     * 到（那时它什么都不做，core 那边会拒掉这次调用），要么排在登记之后——
     * 不存在"子进程已经跑起来但没人记得它"的缝。 */
    pthread_mutex_lock(&g_bash_mtx);
    const pid_t pid = fork();
    if (pid == 0) {
        /* 子进程：以下都是 async-signal-safe 的，多线程 fork 后只能用这些 */
        setpgid(0, 0); /* 自成进程组：中止时一枪打掉命令拉起的整棵子孙树，不留孤儿 */
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    if (pid > 0) {
        setpgid(pid, pid); /* 父子各设一遍：谁先跑到都算数，不必猜调度 */
        g_bash_pgid = pid;
        g_bash_killed = 0;
    }
    pthread_mutex_unlock(&g_bash_mtx);

    close(fds[1]);
    if (pid < 0) { close(fds[0]); r.messages = "{\"error\":\"fork failed\"}"; return r; }

    FILE* f = fdopen(fds[0], "r");
    if (!f) { close(fds[0]); r.messages = "{\"error\":\"fdopen failed\"}"; return r; }

    static char buf[MAX_OUT + 1]; /* 同上：own() 会拷走，旧实现这块 malloc 是纯泄漏 */
    char line[LINE_MAX_LEN];
    size_t n = 0;
    int truncated = 0;
    /* 阻塞读就够了：中止不是让这里醒过来，是把它等的那个东西杀掉——
     * 子进程一死管道就到 EOF，循环自己会退。为此专门去 poll 一个标志位是白费力气。 */
    while (fgets(line, sizeof(line), f)) {
        const size_t len = strlen(line);
        if (n + len <= MAX_OUT) {
            memcpy(buf + n, line, len);
            n += len;
            emit_output(p, call_id, line, len);
        } else {
            truncated = 1; /* 超限后只吞不推：管道还得读干净，中途撒手等于给命令一个 SIGPIPE */
        }
    }
    buf[n] = '\0';
    if (truncated && n >= 3) memcpy(buf + n - 3, "...", 3);
    fclose(f);

    pthread_mutex_lock(&g_bash_mtx);
    g_bash_pgid = 0; /* 摘牌：读到 EOF 就没什么可中止的了 */
    const int killed = g_bash_killed;
    pthread_mutex_unlock(&g_bash_mtx);

    int st = 0, reaped = 0;
    if (killed) {
        /* SIGTERM 递出去了，没人保证它一定死。给一秒，还赖着就 SIGKILL 整组——
         * 用户按了中止，后台不许留下任何东西。 */
        for (int i = 0; i < 100 && !reaped; ++i) {
            if (waitpid(pid, &st, WNOHANG) > 0) reaped = 1;
            else usleep(10000);
        }
        if (!reaped) kill(-pid, SIGKILL);
    }
    if (!reaped) waitpid(pid, &st, 0);

    const int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    r.status = rc == 0 ? 0 : rc; /* 非零退出码视为错误 */
    r.messages = buf;
    return r;
}

/* tool.interrupt：从另一条线进来，只捅一下就走（plugin_api.h 的线程契约）。
 * 收尾（reap、拼结果）留在 do_bash 那条线上——在这儿等子进程死会把事件循环一起卡住。
 * 手上没有在跑的就什么都不做：「下一次调用该不该拒」是 core 的账，记在插件里
 * 只会变成一个迟早过期的标志位。 */
static void interrupt_tool(realugin_plugin_t* p, const char* call_id) {
    (void)p;
    (void)call_id; /* 顺序执行（ADR-0002）：在跑的至多一个，不必按 id 找 */
    pthread_mutex_lock(&g_bash_mtx);
    if (g_bash_pgid > 0) {
        /* 打的是进程组，不是单个进程。第一次 SIGTERM，给命令一个自己收尾的机会；
         * 再来一次就 SIGKILL——捂着 TERM 不撒手、还攥着 stdout 的进程会把读循环
         * 一起吊死，用户再按一次就不该再有商量。 */
        kill(-g_bash_pgid, g_bash_killed ? SIGKILL : SIGTERM);
        g_bash_killed = 1;
    }
    pthread_mutex_unlock(&g_bash_mtx);
}

/* ==================== 插件接口 ==================== */

static const realagent_tool_t k_tools[] = {
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

/* 结果是本次调用现造的 → 转移：经 core->api->alloc 分配，core 读完释放（ADR-0012）。
 * 各 do_* 内部用 malloc/字面量两种来源，此处统一拷成 core 的内存——
 * 于是"谁分配谁释放"这条老账在边界上只剩一种答案，bash 那块输出也不会再泄漏。 */
static realagent_result_t own(realugin_plugin_t* p, realagent_result_t r) {
    realugin_host_t* core = ((struct realugin_plugin*)p)->core;
    const size_t n = r.messages ? strlen(r.messages) : 0;
    char* buf = (char*)core->api->alloc(core, n + 1);
    if (buf) {
        memcpy(buf, r.messages ? r.messages : "", n);
        buf[n] = '\0';
    }
    r.messages = buf;
    return r;
}

static realagent_result_t execute_tool(realugin_plugin_t* p, const char* call_id,
                                    const char* tool_name, const char* params_json) {
    if (strcmp(tool_name, "read") == 0) return own(p, do_read(params_json));
    if (strcmp(tool_name, "edit") == 0) return own(p, do_edit(params_json));
    if (strcmp(tool_name, "bash") == 0) return own(p, do_bash(p, call_id, params_json));
    realagent_result_t r = {.status = 1, .messages = "{\"error\":\"unknown tool\"}"};
    return own(p, r);
}

/* 交出工具清单：借阅静态表，寿命 = 本容器在位时长（ADR-0012）。
 * core 不留副本，每次要用时来问一遍——零分配、零序列化。 */
static size_t tool_list(realugin_plugin_t* p, const realagent_tool_t** out) {
    (void)p;
    *out = k_tools;
    return sizeof(k_tools) / sizeof(k_tools[0]);
}

static realugin_status_t init(realugin_plugin_t* p, realugin_host_t* core) {
    ((struct realugin_plugin*)p)->core = core;
    return REALUGIN_OK;
}

static void destroy(realugin_plugin_t* p) { free(p); }

/* 能力表：交清单、按名执行、中止在跑的那次。各一个函数（ADR-0012）。
 * 中止是可选项——不报这个键的容器，core 只好等它自己跑完。 */
static const realugin_capability_t k_caps[] = {
    {REALAGENT_CAP_TOOL_LIST,      (realugin_fn_t)tool_list},
    {REALAGENT_CAP_TOOL_EXECUTE,   (realugin_fn_t)execute_tool},
    {REALAGENT_CAP_TOOL_INTERRUPT, (realugin_fn_t)interrupt_tool},
};

static size_t capabilities(realugin_plugin_t* p, const realugin_capability_t** out) {
    (void)p;
    *out = k_caps; /* 借阅：静态表，寿命 = 本容器在位时长 */
    return sizeof(k_caps) / sizeof(k_caps[0]);
}

static const realugin_plugin_api_t k_api = {
    .abi_version = REALUGIN_ABI_VERSION,
    .name = "core-tools",
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
