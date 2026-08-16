#include <iostream>
#include <cstring>
#include <cstdio>
#include <string>
#include <ctime>
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

// 通过 root 执行一条命令，返回 (err, out)；失败时 out 为空字符串
static KModErr rsh(const std::string& cmd, std::string& out) {
    out.clear();
    return run_root_cmd(g_root_key.c_str(), cmd.c_str(), out);
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
        if (!is_ok(e)) { add("zip 存在性检查失败 err=" + to_string(e)); return false; }
        if (out.find("MISSING") != std::string::npos) { add("上传的 zip 不存在: " + zip_path); return false; }
        add("[0] zip 存在: " + zip_path);
    }

    // 1) 解出 module.prop 读 id（用 unzip -p 直接读，不动盘）
    std::string modid;
    {
        KModErr e = rsh("unzip -p " + sq(zip_path) + " module.prop 2>/dev/null | sed -n 's/^id=//p' | tr -d '\\r\\n'", out);
        if (!is_ok(e)) { add("读 module.prop 失败 err=" + to_string(e)); return false; }
        modid = out;
        if (modid.empty()) {
            // 兜底：用 zip 名（去后缀）当 id
            std::string base = g_last_upload_orig;
            size_t dot = base.find_last_of('.');
            if (dot != std::string::npos) base = base.substr(0, dot);
            modid = base;
            add("[1] 未读到 module.prop id，用 zip 名兜底: " + modid);
        } else {
            add("[1] module.prop id = " + modid);
        }
    }

    // 2) 落盘路径 /data/adb/modules/<id>
    std::string modroot = "/data/adb/modules/" + modid;
    {
        KModErr e = rsh("mkdir -p " + sq(modroot), out);
        if (!is_ok(e)) { add("mkdir 失败 err=" + to_string(e)); return false; }
        add("[2] 落盘目录: " + modroot);
    }

    // 3) 解压主体：排除 META-INF，其余全解到 modroot
    {
        KModErr e = rsh("cd / && unzip -o " + sq(zip_path) + " -x 'META-INF/*' -d " + sq(modroot) + " 2>&1", out);
        if (!is_ok(e)) { add("解压失败 err=" + to_string(e) + " out=" + out); return false; }
        add("[3] 解压完成（排除 META-INF）");
    }

    // 4) 设权限 + secontext（复刻 set_default_perm）
    {
        KModErr e = rsh("chmod -R a+rX " + sq(modroot) + "; "
                        "find " + sq(modroot) + " -type f -exec chmod 0644 {} \\; 2>/dev/null; "
                        "chmod 0755 " + sq(modroot) + "/service.sh " + sq(modroot) + "/post-fs-data.sh " + sq(modroot) + "/customize.sh 2>/dev/null; true", out);
        (void)e; // 权限失败不致命，继续
        add("[4] 权限已设置（目录 0755 / 文件 0644 / 脚本 0755）");
    }

    // 5) 执行脚本：先 customize.sh（若存在），再 service.sh（复刻生命周期）
    {
        KModErr e = rsh("if [ -f " + sq(modroot) + "/customize.sh ]; then "
                        "  echo '--- customize.sh ---'; sh " + sq(modroot) + "/customize.sh 2>&1; echo 'exit='$?; "
                        "else echo 'no customize.sh'; fi", out);
        add("[5a] customize.sh: " + (is_ok(e) ? out : ("err=" + to_string(e))));
    }
    {
        KModErr e = rsh("if [ -f " + sq(modroot) + "/service.sh ]; then "
                        "  echo '--- service.sh ---'; sh " + sq(modroot) + "/service.sh 2>&1; echo 'exit='$?; "
                        "else echo 'no service.sh'; fi", out);
        add("[5b] service.sh: " + (is_ok(e) ? out : ("err=" + to_string(e))));
    }

    // 6) 读回验证：列目录 + module.prop 内容
    {
        KModErr e = rsh("ls -la " + sq(modroot) + "/ 2>&1", out);
        add("[6] 落盘结果:\n" + (is_ok(e) ? out : ("err=" + to_string(e))));
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

    // 文件上传字段回调：把上传文件流式写盘
    static int field_found_cb(const char* key, const char* filename, char* path, size_t pathlen, void* user_data) {
        (void)user_data;
        if (filename && filename[0]) {
            std::string dir = g_private_dir + "/uploads";
            mkdir(dir.c_str(), 0755);
            std::string dst = dir + "/" + filename;
            g_last_upload_orig = filename;
            g_last_upload_name = dst;
            if (dst.size() + 1 <= pathlen) {
                snprintf(path, pathlen, "%s", dst.c_str());
                return MG_FORM_FIELD_STORAGE_STORE;
            }
        }
        return MG_FORM_FIELD_STORAGE_SKIP;
    }

    bool handlePost(CivetServer* server, struct mg_connection* conn, const std::string& path, const std::string& body) override {
        printf("[SKZygiskCompat] POST path=%s body_len=%zu\n", path.c_str(), body.size());

        // ---- 上传：把 zip 落盘到 uploads/ ----
        if (path == "/upload") {
            g_last_upload_name.clear();
            g_last_upload_orig.clear();
            struct mg_form_data_handler fdh;
            memset(&fdh, 0, sizeof(fdh));
            fdh.field_found = field_found_cb;
            fdh.field_get = nullptr;
            fdh.field_store = nullptr;
            fdh.user_data = nullptr;

            int n = mg_handle_form_request(conn, &fdh);
            printf("[SKZygiskCompat] upload handled=%d name=%s -> %s\n", n, g_last_upload_orig.c_str(), g_last_upload_name.c_str());

            std::string resp = "{\"ok\":true,\"fields\":" + std::to_string(n) +
                               ",\"name\":\"" + g_last_upload_orig + "\",\"path\":\"" + g_last_upload_name + "\"}";
            kernel_module::webui::send_json(conn, 200, resp);
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