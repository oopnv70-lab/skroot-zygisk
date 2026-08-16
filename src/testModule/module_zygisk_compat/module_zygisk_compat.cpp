#include <iostream>
#include <cstring>
#include <cstdio>
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
            append_diag("ls -la " + sq(zip_path) + " 2>&1; echo '--- 上级目录 ---'; ls -la " + sq(g_private_dir) + "/uploads 2>&1", report);
            return false;
        }
        if (out.find("MISSING") != std::string::npos) {
            add("  ✗ 上传的 zip 不存在: " + zip_path);
            append_diag("ls -la " + sq(g_private_dir) + "/uploads 2>&1", report);
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
        append_diag("which unzip 2>&1 || echo 'unzip 不存在'; unzip -v 2>&1 | head -1", report);
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
            append_diag("which unzip 2>&1; unzip -v 2>&1 | head -2; echo '--- zip 完整性 ---'; unzip -t " + sq(zip_path) + " 2>&1 | tail -5", report);
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
    {
        add("[5a] 执行 customize.sh");
        KModErr e = rsh("if [ -f " + sq(modroot) + "/customize.sh ]; then "
                        "  echo '--- customize.sh 开始 ---'; sh " + sq(modroot) + "/customize.sh 2>&1; echo '--- customize.sh 退出码='$?; "
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
        return false; // 走 civetweb 默认静态文件服务，返回 webroot/index.html
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
            bool ok = write_upload(g_private_dir + "/uploads", filename, body, err);
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