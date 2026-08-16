#include <iostream>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <string>
#include <ctime>
#include <cerrno>
#include <sys/stat.h>
#include "kernel_module_kit_umbrella.h"

using namespace skroot_env;

// 全局状态（入口 + WebUI 共用）
static std::string g_root_key;
static std::string g_private_dir;
static std::string g_last_upload_name;   // 上传落盘后的 zip 完整路径
static std::string g_last_upload_orig;   // 原始文件名（不含路径）
static std::string g_last_install_report; // 最近一次安装的完整报告

static void logi(const std::string& s) { printf("[SKZygiskCompat] %s\n", s.c_str()); }

// 通过 root 执行一条命令，返回 (err, out)；失败时 out 保底为 "<无输出>"
static KModErr rsh(const std::string& cmd, std::string& out) {
    out.clear();
    KModErr e = run_root_cmd(g_root_key.c_str(), cmd.c_str(), out);
    // 失败时也尽量带上 stderr：很多 shell 错误只在 stderr 里，2>&1 已并入用 out 收集
    if (is_failed(e) && out.empty()) {
        out = "<run_root_cmd 无输出>";
    }
    return e;
}
// 追加一条可读的 shell 诊断：执行后把 (err, stdout+stderr) 都写进报告
static void append_diag(const std::string& cmd, std::string& report) {
    std::string out;
    KModErr e = rsh(cmd, out);
    report += "    cmd: " + cmd + "\n";
    report += "    err: " + to_string(e) + "\n";
    if (!out.empty()) {
        size_t p = 0;
        while (p < out.size()) {
            size_t nl = out.find('\n', p);
            std::string line = (nl == std::string::npos) ? out.substr(p) : out.substr(p, nl - p);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            report += "    out: " + line + "\n";
            if (nl == std::string::npos) break;
            p = nl + 1;
        }
    }
}

// 把 shell 单引号包裹，避免注入/转义问题
static std::string sq(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    r += "'";
    return r;
}
// 去掉字符串末尾的 '/'(只留根路径"/"本身的斜杠)，用于拼接子路径避免出现 "//"
static std::string trim_trailing_slash(const std::string& s) {
    std::string r = s;
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    return r;
}
// 拼接路径：左侧去掉尾斜杠后加 '/' + 右侧
static std::string join_path(const std::string& base, const std::string& sub) {
    return trim_trailing_slash(base) + "/" + sub;
}
// ============ 通用模块 WebUI 代理：把任意模块的 webroot 当作可访问站点 ============
// base64 解码（自实现，二进制安全，把 root shell 读回的 base64 还原成原始字节）
static std::string b64_decode(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int rev[256];
    for (int i = 0; i < 256; i++) rev[i] = -1;
    for (int i = 0; i < 64; i++) rev[(unsigned char)tbl[i]] = i;
    std::string out;
    out.reserve(in.size() * 3 / 4 + 4);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c == '=') break;
        int d = rev[c];
        if (d < 0) continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out.push_back((char)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}
// 通过 root 读取任意文件为二进制（base64 中转，避免 shell 对 \0/特殊字节的破坏）
// 成功返回 true，失败返回 false（文件不存在或读取失败）
static bool root_read_file_bin(const std::string& path, std::string& out) {
    out.clear();
    std::string b64;
    // base64 命令（toybox 提供）：-w0 不换行；文件不存在时 2>/dev/null 吞掉报错输出空串
    KModErr e = rsh("base64 -w0 " + sq(path) + " 2>/dev/null", b64);
    if (is_failed(e) && b64.empty()) return false;
    if (b64.empty()) {
        // 空文件合法（返回空内容但存在）；区分"不存在"与"空文件"需另判
        std::string chk;
        KModErr ce = rsh("test -f " + sq(path) + " && echo EXISTS || echo MISSING", chk);
        if (is_failed(ce) || chk.find("MISSING") != std::string::npos) return false;
        return true; // 空文件存在
    }
    out = b64_decode(b64);
    return true;
}
// 依据路径扩展名返回 Content-Type（供 send_bytes 使用）
static const char* mime_of(const std::string& path) {
    auto ends = [&](const char* ext) {
        size_t n = strlen(ext);
        return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
    };
    if (ends(".html") || ends(".htm")) return "text/html; charset=utf-8";
    if (ends(".js") || ends(".mjs")) return "application/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".json")) return "application/json; charset=utf-8";
    if (ends(".png")) return "image/png";
    if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
    if (ends(".gif")) return "image/gif";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".ico")) return "image/x-icon";
    if (ends(".woff")) return "font/woff";
    if (ends(".woff2")) return "font/woff2";
    if (ends(".ttf")) return "font/ttf";
    if (ends(".wasm")) return "application/wasm";
    if (ends(".map")) return "application/json";
    if (ends(".txt")) return "text/plain; charset=utf-8";
    if (ends(".webmanifest")) return "application/manifest+json";
    return "application/octet-stream";
}
// 校验模块 id：只允许安全字符（字母数字 _ - .），禁止路径穿越与 shell 注入
static bool safe_module_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (char c : id) {
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}
// 校验"目录名"（用于卸载/运行等按真实目录名操作的地方）：
//   只防路径穿越（禁止 '/' 与 '..'），其余字符都放行——目录名是文件系统能接受的
//   任意名字（可能含括号、空格、点等，如脏数据 id "skzygiskcompat(5)"）。禁止 \0。
static bool safe_dir_name(const std::string& s) {
    if (s.empty() || s.size() > 255) return false;
    if (s.find('/') != std::string::npos) return false;
    if (s.find("..") != std::string::npos) return false;
    if (s.find('\0') != std::string::npos) return false;
    return true;
}
// 从 query string 里提取指定 key 的值并做 URL decode。
// 前端用 encodeURIComponent 编码（如括号→%28），后端必须解码回原始目录名。
static std::string get_query_param_decoded(struct mg_connection* conn, const std::string& key) {
    std::string q = kernel_module::webui::get_request_query_string(conn); // 原始（未解码）query
    std::string needle = key + "=";
    size_t pos = q.find(needle);
    if (pos == std::string::npos) return "";
    std::string raw = q.substr(pos + needle.size());
    size_t amp = raw.find('&');
    if (amp != std::string::npos) raw = raw.substr(0, amp);
    // URL decode（URI 编码，'+' 不是空格，故 is_form_url_encoded=false）
    std::string decoded;
    CivetServer::urlDecode(raw, decoded, false);
    return decoded;
}
// 把 HTML 里的"根绝对路径"重写为模块子路径前缀 /module/<id>：
//   模块的 webroot 常用 /assets/xxx、/internal/xxx 这类根绝对路径引用自身资源
//   （KernelSU/APatch 的原生 WebUI 里模块界面挂在根路径，所以模块作者这么写）。
//   我们代理到 /module/<id>/ 子路径，必须把 src="..." 与 href="..." 中
//   以 '/' 开头、且非 '//'（协议相对）、非 http(s) 的引用前缀补上 /module/<id>。
static std::string rewrite_root_paths(const std::string& html, const std::string& id) {
    std::string out;
    out.reserve(html.size() + 64);
    const std::string prefix = "/module/" + id;
    size_t i = 0;
    const size_t n = html.size();
    // 依次扫描所有 "src=" 或 "href=" 后的 '/'，判断是否根路径
    const char* attrs[] = {"src=", "href=", "SRC=", "HREF="};
    while (i < n) {
        // 找下一个 attr（可能带引号）
        size_t best = std::string::npos;
        int bestlen = 0;
        for (const char* a : attrs) {
            size_t p = html.find(a, i);
            if (p != std::string::npos && (best == std::string::npos || p < best)) { best = p; bestlen = (int)strlen(a); }
        }
        if (best == std::string::npos) { out += html.substr(i); break; }
        // 拷贝 [i, best+attrlen) 原样
        out += html.substr(i, best - i);
        // attr 名
        out += html.substr(best, bestlen);
        size_t j = best + bestlen;
        // 跳过 attr 名后可能的空白与引号（引号也要原样输出）
        if (j < n && (html[j] == '"' || html[j] == '\'')) { out += html[j]; j++; }
        else {
            // 无引号：跳过空白
            while (j < n && (html[j] == ' ' || html[j] == '\t')) j++;
        }
        // 现在 j 指向属性值起始。判断是否为根绝对路径：'/xxx' 且第二个字符不是 '/'
        if (j < n && html[j] == '/' && (j + 1 >= n || html[j + 1] != '/')) {
            // 根绝对路径：插入前缀
            out += prefix;
        }
        // 继续扫描（不消费引号，从 j 继续，让下一轮正常处理剩余）
        i = j;
    }
    return out;
}

// ============ 安装一个 Magisk 模块 zip（复刻 Magisk install_module 的 6 步） ============
// 输入：zip_path = 已上传落盘的模块 zip 绝对路径
// 输出：report 收集中文报告，返回 true=成功，false=失败
static bool do_install_magisk_module(const std::string& zip_path, std::string& report) {
    report.clear();
    auto add = [&](const std::string& line) { report += line + "\n"; };

    std::string out;

    // 0) 确认 zip 存在
    {
        KModErr e = rsh("test -f " + sq(zip_path) + " && echo EXISTS || echo MISSING", out);
        add("[0] zip 存在性检查");
        if (!is_ok(e)) {
            add("  ✗ rsh 调用失败 err=" + to_string(e));
            append_diag("ls -la " + sq(zip_path) + " 2>&1; echo '--- 上级目录 ---'; ls -la " + sq(join_path(g_private_dir, "uploads")) + " 2>&1", report);
            return false;
        }
        if (out.find("MISSING") != std::string::npos) {
            add("  ✗ 上传的 zip 不存在: " + zip_path);
            append_diag("ls -la " + sq(join_path(g_private_dir, "uploads")) + " 2>&1", report);
            return false;
        }
        add("  ✓ zip 存在: " + zip_path);
        append_diag("ls -la " + sq(zip_path) + " 2>&1", report);
    }
    // 1) 解出 module.prop 读 id（用 unzip -p 直接读，不动盘）
    std::string modid;
    {
        // 预检 unzip 是否存在，失败时能看到具体原因
        add("[1] 读取 module.prop id");
        append_diag("which unzip 2>&1 || echo 'unzip 不存在'", report);
        KModErr e = rsh("unzip -p " + sq(zip_path) + " module.prop 2>&1 | sed -n 's/^id=//p' | tr -d '\\r\\n'", out);
        if (!is_ok(e)) {
            add("  ✗ 读 module.prop 失败 err=" + to_string(e) + " out=" + out);
            append_diag("unzip -t " + sq(zip_path) + " 2>&1", report);
            return false;
        }
        modid = out;
        if (modid.empty()) {
            std::string base = g_last_upload_orig;
            size_t dot = base.find_last_of('.');
            if (dot != std::string::npos) base = base.substr(0, dot);
            modid = base;
            add("  ⚠ 未读到 module.prop id，用 zip 名兜底: " + modid);
            // 诊断：zip 里到底有没有 module.prop、内容长啥样
            append_diag("unzip -l " + sq(zip_path) + " 2>&1 | head -30", report);
        } else {
            add("  ✓ module.prop id = " + modid);
        }
    }
    // 2) 落盘路径 /data/adb/modules/<id>
    std::string modroot = "/data/adb/modules/" + modid;
    {
        add("[2] 创建落盘目录 " + modroot);
        KModErr e = rsh("mkdir -p " + sq(modroot) + " 2>&1; echo rc=$?", out);
        if (!is_ok(e) || out.find("rc=0") == std::string::npos) {
            add("  ✗ mkdir 失败 err=" + to_string(e) + " out=" + out);
            append_diag("ls -ld /data 2>&1; ls -ld /data/adb 2>&1; ls -ld /data/adb/modules 2>&1", report);
            return false;
        }
        add("  ✓ 落盘目录: " + modroot);
    }

    // 3) 解压主体：排除 META-INF，其余全解到 modroot
    {
        add("[3] 解压主体（排除 META-INF）");
        KModErr e = rsh("cd / && unzip -o " + sq(zip_path) + " -x 'META-INF/*' -d " + sq(modroot) + " 2>&1", out);
        if (!is_ok(e) || out.find("cannot find") != std::string::npos || out.find("error") != std::string::npos) {
            add("  ✗ 解压失败 err=" + to_string(e) + " out=" + out);
            append_diag("which unzip 2>&1; echo '--- zip 完整性 ---'; unzip -t " + sq(zip_path) + " 2>&1 | tail -5", report);
            return false;
        }
        add("  ✓ 解压完成");
    }
    // 4) 设权限 + secontext（复刻 set_default_perm）
    {
        add("[4] 设置权限");
        KModErr e = rsh("chmod -R a+rX " + sq(modroot) + " 2>&1; "
                        "find " + sq(modroot) + " -type f -exec chmod 0644 {} \\; 2>&1; "
                        "chmod 0755 " + sq(modroot) + "/service.sh " + sq(modroot) + "/post-fs-data.sh " + sq(modroot) + "/customize.sh 2>&1; true", out);
        if (!is_ok(e)) {
            add("  ✗ chmod 执行失败 err=" + to_string(e));
            append_diag("ls -laR " + sq(modroot) + " 2>&1 | head -40", report);
            return false;
        }
        add("  ✓ chmod 已执行");
        // 读回实际权限位验证（不再无条件声称成功）
        {
            std::string perm;
            KModErr e2 = rsh("find " + sq(modroot) + " -maxdepth 2 -type f -printf '%m %p\\n' 2>&1 | head -40", perm);
            if (is_ok(e2)) {
                add("  --- 实际权限位 ---");
                if (perm.empty()) {
                    add("  （无文件）");
                } else {
                    add("  " + perm);
                }
            } else {
                add("  ⚠ 读回权限失败 err=" + to_string(e2));
            }
        }
    }
    // 5) 执行脚本：先 customize.sh（若存在），再 service.sh（复刻生命周期）
    //    customize.sh 通过 source Magisk 官方 util_functions.sh 获得全套函数，
    //    并注入身份变量（MAGISK_VER_CODE 等）让脚本走进正确的 root 实现分支。
    {
        add("[5a] 执行 customize.sh（source 官方 util_functions.sh）");
        std::string util = join_path(g_private_dir, "webroot") + "/util_functions.sh";
        // 检查 util_functions.sh 是否随 webroot 解压到位
        {
            std::string chk;
            KModErr ec = rsh("test -f " + sq(util) + " && echo EXISTS || echo MISSING", chk);
            if (!is_ok(ec) || chk.find("MISSING") != std::string::npos) {
                add("  ✗ util_functions.sh 缺失: " + util);
                append_diag("ls -la " + sq(join_path(g_private_dir, "webroot")) + " 2>&1", report);
            } else {
                add("  ✓ util_functions.sh 就位: " + util);
            }
        }
        // 在同一个 shell 里：source util_functions.sh → 设身份变量 → 探测架构 → source customize.sh
        // 身份：伪装成 Magisk。MAGISK_VER_CODE 需同时满足：
        //   - Zygisk-Next 的 MIN_MAGISK_VERSION=26402（Magisk 26.4）
        //   - LSPosed 的 check_magisk_version 阈值 26403（其提示文案误写为 v27+）
        // 取 28101（Magisk v28.1）以覆盖更高要求的新模块，避免被各家版本门槛卡住。
        KModErr e = rsh("if [ -f " + sq(modroot) + "/customize.sh ]; then "
                        "  echo '--- customize.sh 开始（官方 util_functions.sh）---'; "
                        "  sh -c '. " + sq(util) + "; "
                        "OUTFD=1; BOOTMODE=true; TMPDIR=/dev/tmp; MAGISKBIN=/data/adb/magisk; "
                        "MAGISK_VER_CODE=28101; "
                        "ZIPFILE=" + sq(zip_path) + "; MODPATH=" + sq(modroot) + "; MODID=" + sq(modid) + "; "
                        "api_level_arch_detect; "
                        ". " + sq(modroot) + "/customize.sh' 2>&1; echo '--- customize.sh 退出码='$?; "
                        "else echo '（无 customize.sh）'; fi", out);
        add("  " + (is_ok(e) ? out : ("err=" + to_string(e) + " out=" + out)));
    }
    {
        add("[5b] 执行 service.sh");
        KModErr e = rsh("if [ -f " + sq(modroot) + "/service.sh ]; then "
                        "  echo '--- service.sh 开始 ---'; sh " + sq(modroot) + "/service.sh 2>&1; echo '--- service.sh 退出码='$?; "
                        "else echo '（无 service.sh）'; fi", out);
        add("  " + (is_ok(e) ? out : ("err=" + to_string(e) + " out=" + out)));
    }
    // 6) 读回验证：列目录 + module.prop 内容
    {
        add("[6] 落盘结果");
        KModErr e = rsh("ls -la " + sq(modroot) + "/ 2>&1", out);
        add(is_ok(e) ? out : ("err=" + to_string(e) + " out=" + out));
        // 额外：读 module.prop 内容，确认身份落盘正确
        {
            std::string prop;
            KModErr e2 = rsh("cat " + sq(modroot) + "/module.prop 2>&1", prop);
            add("  --- module.prop 内容 ---");
            add(is_ok(e2) ? prop : ("（读不到: " + to_string(e2) + "）"));
        }
    }
    return true;
}

// skroot_module_main 入口 —— 尽早返回，耗时安装放到 WebUI 触发（浏览器点按钮时）
int skroot_module_main(const char* root_key, const char* module_private_dir) {
    g_root_key = root_key ? root_key : "";
    g_private_dir = module_private_dir ? module_private_dir : "";

    printf("[SKZygiskCompat] entry called\n");
    printf("[SKZygiskCompat] root_key len=%zu\n", strlen(root_key ? root_key : ""));
    printf("[SKZygiskCompat] module_private_dir=%s\n", module_private_dir ? module_private_dir : "");

    bool key_ok = (strlen(root_key ? root_key : "") == 48);
    printf("[SKZygiskCompat] root_key check: %s\n", key_ok ? "PASS(48)" : "WARN(not48)");

    KModErr e = set_skroot_log_enabled(root_key, true);
    printf("[SKZygiskCompat] set_log_enabled => %s\n", to_string(e).c_str());

    e = get_root(root_key);
    printf("[SKZygiskCompat] get_root => %s\n", to_string(e).c_str());

    printf("[SKZygiskCompat] done\n");
    return (key_ok && is_ok(e)) ? 0 : 1;
}

// WebUI HTTP 服务器回调（基于 civetweb）
class ZygiskCompatWebHandler : public kernel_module::WebUIHttpHandler {
public:
    void onPrepareCreate(const char* root_key, const char* module_private_dir, uint32_t port) override {
        g_root_key = root_key ? root_key : "";
        g_private_dir = module_private_dir ? module_private_dir : "";
        printf("[SKZygiskCompat] WebUI port=%u dir=%s\n", port, g_private_dir.c_str());
    }

    bool handleGet(CivetServer* server, struct mg_connection* conn, const std::string& path, const std::string& query) override {
        printf("[SKZygiskCompat] GET path=%s query=%s\n", path.c_str(), query.c_str());
        // ---- 通用模块 WebUI 代理：/module/<id>/<路径> → /data/adb/modules/<id>/webroot/<路径> ----
        // 任何带 webroot/ 的 Magisk 模块，装进来后都能从模块列表点开自己的第二个界面。
        const std::string MP = "/module/";
        if (path.size() >= MP.size() && path.compare(0, MP.size(), MP) == 0) {
            std::string rest = path.substr(MP.size()); // id 之后的剩余路径（可能是 "zygisksu/index.html" 或 "zygisksu/assets/xxx.js"）
            // 拆出 <id> 和 <子路径>
            size_t slash = rest.find('/');
            std::string id = (slash == std::string::npos) ? rest : rest.substr(0, slash);
            std::string sub = (slash == std::string::npos) ? "" : rest.substr(slash + 1);
            // 若请求 /module/<id>（无子路径），默认跳 index.html
            if (sub.empty()) sub = "index.html";
            // 安全校验：id 必须安全；子路径禁止含 ".."（防 /data/adb/modules 外穿越）
            if (!safe_module_id(id) || sub.find("..") != std::string::npos) {
                kernel_module::webui::send_text(conn, 400, "bad module id / path");
                return true;
            }
            std::string file = "/data/adb/modules/" + id + "/webroot/" + sub;
            std::string content;
            if (!root_read_file_bin(file, content)) {
                kernel_module::webui::send_text(conn, 404, "module webui file not found: " + file);
                return true;
            }
            // 对 .html 注入 ksu 兼容桥（方案 A：不修改模块原文件，运行时改写响应）
            std::string out = content;
            const char* ctype = mime_of(sub);
            if (sub.size() >= 5 && (sub.compare(sub.size() - 5, 5, ".html") == 0 ||
                                    sub.compare(sub.size() - 4, 4, ".htm") == 0)) {
                // 先重写模块自身资源的根绝对路径（/assets、/internal 等）→ /module/<id>/assets 等
                out = rewrite_root_paths(out, id);
                // 再注入 ksu 兼容桥（根路径 /ksu-bridge.js，所有模块界面共享，不随模块子路径变化）
                const std::string script = "<script src=\"/ksu-bridge.js\"></script>";
                // 优先插到 <head> 之后（紧随其后，保证最先加载）；找不到则在文档开头注入
                size_t hp = out.find("<head>");
                if (hp != std::string::npos) {
                    out.insert(hp + 6, script);
                } else {
                    size_t hp2 = out.find("<head ");
                    if (hp2 != std::string::npos) {
                        size_t gt = out.find('>', hp2);
                        if (gt != std::string::npos) out.insert(gt + 1, script);
                        else out = script + out;
                    } else {
                        out = script + out;
                    }
                }
                ctype = "text/html; charset=utf-8";
            }
            kernel_module::webui::send_bytes(conn, 200, ctype, out.data(), out.size());
            return true;
        }
        // /ksu-bridge.js 走 civetweb 静态服务（webroot/ksu-bridge.js 已随本模块落盘，所有模块界面共享引用）
        return false; // 其余（含 / 首页、/ksu-bridge.js）走 civetweb 默认静态文件服务
    }
    // 重写基类的两参 handlePost：基类把 body 上限写死 256KB，会导致大模块 zip 直接 413。
    // 这里用更大的上限（64MB）读取，再转回四参版本交给子类业务逻辑。
    bool handlePost(CivetServer* server, struct mg_connection* conn) override {
        std::string path = kernel_module::webui::get_request_path(conn);
        std::string query = kernel_module::webui::get_request_query_string(conn);
        std::string body;
        auto st = kernel_module::webui::read_request_body(conn, body, 64u * 1024u * 1024u);
        if (st == kernel_module::webui::BodyReadStatus::TOO_LARGE) {
            printf("[SKZygiskCompat] POST 超过 64MB 上限，拒绝\n");
            return kernel_module::webui::send_text(conn, 413, "payload too large (>64MB)"), true;
        }
        if (st != kernel_module::webui::BodyReadStatus::OK) {
            printf("[SKZygiskCompat] POST body 读取失败 st=%d\n", (int)st);
            return kernel_module::webui::send_text(conn, 400, "bad request body"), true;
        }
        return handlePost(server, conn, path, body);
    }

    // 把原始请求体（纯文件字节）写盘到 uploads/
    static bool write_upload(const std::string& dir, const std::string& filename, const std::string& body, std::string& err) {
        if (body.empty()) { err = "body 为空"; return false; }
        err.clear();
        if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
            err = "mkdir 失败 errno=" + std::to_string(errno);
            return false;
        }
        std::string dst = dir + "/" + filename;
        FILE* fp = fopen(dst.c_str(), "wb");
        if (!fp) { err = "fopen 失败 errno=" + std::to_string(errno) + " path=" + dst; return false; }
        size_t w = fwrite(body.data(), 1, body.size(), fp);
        int fe = ferror(fp);
        fclose(fp);
        if (w != body.size() || fe) {
            err = "fwrite 不完整 写入=" + std::to_string(w) + " 期望=" + std::to_string(body.size());
            return false;
        }
        g_last_upload_name = dst;
        g_last_upload_orig = filename;
        return true;
    }
    bool handlePost(CivetServer* server, struct mg_connection* conn, const std::string& path, const std::string& body) override {
        printf("[SKZygiskCompat] POST path=%s body_len=%zu\n", path.c_str(), body.size());
        // ---- 上传：把原始 body（zip 字节）落盘到 uploads/ ----
        if (path == "/upload") {
            g_last_upload_name.clear();
            g_last_upload_orig.clear();
            // 文件名从 query 参数 ?name=xxx.zip 取；缺省用时间戳
            std::string q = kernel_module::webui::get_request_query_string(conn);
            std::string filename;
            const std::string key = "name=";
            size_t p = q.find(key);
            if (p != std::string::npos) {
                filename = q.substr(p + key.size());
                size_t amp = filename.find('&');
                if (amp != std::string::npos) filename = filename.substr(0, amp);
            }
            if (filename.empty()) {
                char ts[32];
                snprintf(ts, sizeof(ts), "upload_%lld.zip", (long long)time(nullptr));
                filename = ts;
            }
            std::string err;
            bool ok = write_upload(join_path(g_private_dir, "uploads"), filename, body, err);
            printf("[SKZygiskCompat] upload ok=%d name=%s path=%s err=%s body_len=%zu\n",
                   ok ? 1 : 0, filename.c_str(), g_last_upload_name.c_str(), err.c_str(), body.size());
            std::string resp;
            if (ok) {
                resp = "{\"ok\":true,\"name\":\"" + filename + "\",\"path\":\"" + g_last_upload_name + "\",\"size\":" + std::to_string(body.size()) + "}";
            } else {
                resp = "{\"ok\":false,\"error\":\"" + err + "\"}";
            }
            kernel_module::webui::send_json(conn, ok ? 200 : 500, resp);
            return true;
        }
// ---- 安装：把已上传的 zip 按 Magisk 契约装进 /data/adb/modules/<id>/ ----
        if (path == "/install") {
            if (g_last_upload_name.empty()) {
                kernel_module::webui::send_json(conn, 400, "{\"ok\":false,\"error\":\"请先上传 zip\"}");
                return true;
            }
            std::string report;
            bool ok = do_install_magisk_module(g_last_upload_name, report);
            g_last_install_report = report;

            // 简单 JSON 转义 report（仅处理反斜杠与双引号）
            std::string esc;
            for (char c : report) {
                if (c == '\\') esc += "\\\\";
                else if (c == '"') esc += "\\\"";
                else if (c == '\n') esc += "\\n";
                else esc += c;
            }
            std::string resp = "{\"ok\":" + std::string(ok ? "true" : "false") +
                               ",\"report\":\"" + esc + "\"}";
            kernel_module::webui::send_json(conn, 200, resp);
            return true;
        }

        // ---- 列表：扫描 /data/adb/modules/ 下已装模块 ----
        if (path == "/list") {
            std::string out;
            // 一条 shell 扫描：输出 id|name|version|author|description|hasWebui
            // hasWebui = webroot/index.html 是否存在（1/0），用于前端渲染"打开界面"按钮
            KModErr e = rsh(
                "for d in /data/adb/modules/*/; do [ -d \"$d\" ] || continue; "
                "id=$(basename \"$d\"); f=\"$d/module.prop\"; [ -f \"$f\" ] || continue; "
                "name=$(sed -n 's/^name=//p' \"$f\" | tr -d '\\r'); "
                "ver=$(sed -n 's/^version=//p' \"$f\" | tr -d '\\r'); "
                "auth=$(sed -n 's/^author=//p' \"$f\" | tr -d '\\r'); "
                "desc=$(sed -n 's/^description=//p' \"$f\" | tr -d '\\r'); "
                "if [ -f \"$d/webroot/index.html\" ]; then w=1; else w=0; fi; "
                "echo \"$id|$name|$ver|$auth|$desc|$w\"; done 2>&1", out);
            std::string resp;
            if (is_failed(e)) {
                resp = "{\"ok\":false,\"error\":\"" + to_string(e) + "\"}";
            } else {
                std::string arr = "[";
                size_t p = 0;
                bool first = true;
                while (p < out.size()) {
                    size_t nl = out.find('\n', p);
                    std::string line = (nl == std::string::npos) ? out.substr(p) : out.substr(p, nl - p);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    p = (nl == std::string::npos) ? out.size() : nl + 1;
                    if (line.empty()) continue;
                    // 拆前 4 段 id|name|version|author，剩余 = description|hasWebui
                    std::string f[4];
                    size_t pos = 0;
                    bool bad = false;
                    for (int i = 0; i < 4; i++) {
                        size_t bar = line.find('|', pos);
                        if (bar == std::string::npos) { bad = true; break; }
                        f[i] = line.substr(pos, bar - pos);
                        pos = bar + 1;
                    }
                    if (bad) continue;
                    std::string rest = line.substr(pos); // description|hasWebui
                    // 从最后一个 | 处拆：左边 description（可含 |），右边 hasWebui
                    std::string desc, has;
                    size_t lastbar = rest.rfind('|');
                    if (lastbar == std::string::npos) { desc = rest; has = "0"; }
                    else { desc = rest.substr(0, lastbar); has = rest.substr(lastbar + 1); }
                    auto jesc = [](const std::string& s) {
                        std::string r;
                        for (char c : s) {
                            if (c == '\\') r += "\\\\";
                            else if (c == '"') r += "\\\"";
                            else if (c == '\n') r += "\\n";
                            else if (c == '\r') r += "\\r";
                            else if (c == '\t') r += "\\t";
                            else r += c;
                        }
                        return r;
                    };
                    if (!first) arr += ",";
                    first = false;
                    arr += "{\"id\":\"" + jesc(f[0]) + "\",\"name\":\"" + jesc(f[1]) +
                           "\",\"version\":\"" + jesc(f[2]) + "\",\"author\":\"" + jesc(f[3]) +
                           "\",\"description\":\"" + jesc(desc) + "\",\"hasWebui\":" +
                           (has == "1" ? "true" : "false") + "}";
                }
                arr += "]";
                resp = "{\"ok\":true,\"modules\":" + arr + "}";
            }
            kernel_module::webui::send_json(conn, 200, resp);
            return true;
        }

        // ---- 运行 service.sh：执行 /data/adb/modules/<id>/service.sh ----
        if (path == "/runService") {
            std::string id = get_query_param_decoded(conn, "id");
            if (id.empty()) {
                kernel_module::webui::send_json(conn, 400, "{\"ok\":false,\"error\":\"缺少 id 参数\"}");
                return true;
            }
            if (!safe_dir_name(id)) {
                kernel_module::webui::send_json(conn, 400, "{\"ok\":false,\"error\":\"非法 id\"}");
                return true;
            }
            std::string modroot = "/data/adb/modules/" + id;
            std::string out;
            KModErr e = rsh("test -f " + sq(modroot) + "/service.sh 2>/dev/null && sh " +
                            sq(modroot + "/service.sh") + " 2>&1; echo \"SVC_EXIT=$?\"", out);
            bool ok = is_ok(e);
            std::string esc;
            for (char c : out) {
                if (c == '\\') esc += "\\\\";
                else if (c == '"') esc += "\\\"";
                else if (c == '\n') esc += "\\n";
                else esc += c;
            }
            std::string resp = "{\"ok\":" + std::string(ok ? "true" : "false") +
                               ",\"report\":\"" + esc + "\"}";
            kernel_module::webui::send_json(conn, 200, resp);
            return true;
        }

        // ---- 卸载：删除 /data/adb/modules/<id>（id 从 query 取，需 URL decode）----
        if (path == "/uninstall") {
            std::string id = get_query_param_decoded(conn, "id");
            if (id.empty()) {
                kernel_module::webui::send_json(conn, 400, "{\"ok\":false,\"error\":\"缺少 id 参数\"}");
                return true;
            }
            // 只防路径穿越（禁止 / 与 ..），其余字符（如括号、点）都放行——目录名就是文件系统名
            if (!safe_dir_name(id)) {
                kernel_module::webui::send_json(conn, 400, "{\"ok\":false,\"error\":\"非法 id\"}");
                return true;
            }
            std::string modroot = "/data/adb/modules/" + id;
            std::string out;
            KModErr e = rsh("rm -rf " + sq(modroot) + " 2>&1; test -d " + sq(modroot) +
                            " && echo STILL_EXISTS || echo REMOVED", out);
            bool ok = is_ok(e) && out.find("REMOVED") != std::string::npos;
            std::string resp = ok
                ? "{\"ok\":true,\"id\":\"" + id + "\"}"
                : "{\"ok\":false,\"error\":\"卸载失败: " + out + "\"}";
            kernel_module::webui::send_json(conn, ok ? 200 : 500, resp);
            return true;
        }

        // ---- ksu.exec 兼容桥：对齐 KernelSU window.ksu.exec(cmd, options, callback) ----
        // 前端会把命令 POST 到这里（JSON），我们执行并把 stdout/stderr/exit code 分开返回。
        if (path == "/ksuExec") {
            // body 是原始命令字符串（或 JSON {"cmd":"..."}），这里统一按"原样当 shell 命令"处理
            std::string cmd = body;
            // 若 body 是 JSON 包装，剥一层（前端可能传 {"cmd": "..."}）
            {
                std::string t = body;
                // 去掉首尾空白
                while (!t.empty() && (t.front() == ' ' || t.front() == '\n' || t.front() == '\r' || t.front() == '\t')) t.erase(t.begin());
                while (!t.empty() && (t.back() == ' ' || t.back() == '\n' || t.back() == '\r' || t.back() == '\t')) t.pop_back();
                if (t.size() >= 2 && t.front() == '{' && t.back() == '}') {
                    const std::string ck = "\"cmd\":\"";
                    size_t ckpos = t.find(ck);
                    if (ckpos != std::string::npos) {
                        size_t vstart = ckpos + ck.size();
                        size_t vend = t.find('"', vstart);
                        if (vend != std::string::npos) {
                            cmd = t.substr(vstart, vend - vstart);
                        }
                    }
                }
            }
            if (cmd.empty()) {
                kernel_module::webui::send_json(conn, 400, "{\"errno\":-1,\"stdout\":\"\",\"stderr\":\"empty command\"}");
                return true;
            }
            // 用临时文件分离 stdout / stderr / exit code
            std::string tmpdir = "/data/local/tmp/skzygisk_exec";
            std::string run_out;
            KModErr e = rsh("mkdir -p " + sq(tmpdir) + " 2>/dev/null; "
                            "sh -c " + sq(cmd) + " > " + sq(tmpdir) + "/out 2> " + sq(tmpdir) + "/err; "
                            "echo $? > " + sq(tmpdir) + "/code", run_out);
            // 分别读回 code / out / err
            std::string code_s, out_s, err_s;
            KModErr ec = rsh("cat " + sq(tmpdir) + "/code 2>/dev/null | tr -d '\\n\\r'", code_s);
            KModErr eo = rsh("cat " + sq(tmpdir) + "/out 2>/dev/null", out_s);
            KModErr ee = rsh("cat " + sq(tmpdir) + "/err 2>/dev/null", err_s);
            // 转义三个字段
            auto jesc = [](const std::string& s) {
                std::string r;
                for (char c : s) {
                    if (c == '\\') r += "\\\\";
                    else if (c == '"') r += "\\\"";
                    else if (c == '\n') r += "\\n";
                    else if (c == '\r') r += "\\r";
                    else if (c == '\t') r += "\\t";
                    else r += c;
                }
                return r;
            };
            int errno_i = 0;
            if (is_ok(ec)) {
                // 用 strtol 解析退出码（不抛异常，适配 NDK -fno-exceptions 环境）
                errno = 0;
                char* endp = nullptr;
                long v = strtol(code_s.c_str(), &endp, 10);
                if (errno == 0 && endp && endp != code_s.c_str() && *endp == '\0') {
                    errno_i = (int)v;
                } else {
                    errno_i = -1;
                }
            } else {
                errno_i = -1;
            }
            std::string resp = "{\"errno\":" + std::to_string(errno_i) +
                               ",\"stdout\":\"" + jesc(out_s) +
                               "\",\"stderr\":\"" + jesc(err_s) + "\"}";
            kernel_module::webui::send_json(conn, 200, resp);
            return true;
        }

        // ---- 状态 ----
        if (path == "/getStatus") {
            std::string resp = "{\"entry\":\"ok\",\"pid\":" + std::to_string(getpid()) +
                               ",\"uid\":" + std::to_string(getuid()) + "}";
            kernel_module::webui::send_json(conn, 200, resp);
            return true;
        }

        kernel_module::webui::send_text(conn, 200, "{}");
        return true;
    }
};

// SKRoot 模块名片
SKROOT_MODULE_NAME("SKZygiskCompat")
SKROOT_MODULE_VERSION("0.4.0")
SKROOT_MODULE_DESC("Magisk compatibility layer: upload+install+run magisk module inside SKRoot")
SKROOT_MODULE_AUTHOR("oopnv70-lab")
SKROOT_MODULE_ID32("Zg4c0mp4t1b3L4y3r5kR00tPr0202601")
SKROOT_MODULE_WEB_UI(ZygiskCompatWebHandler)