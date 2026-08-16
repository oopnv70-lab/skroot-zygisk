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

// ============ Magisk 脚本仿真 shim（shell prologue） ============
// Magisk 模块的 customize.sh 不能裸 sh 跑：它依赖 Magisk 在 source 脚本前注入的
// 一整套内置函数（ui_print/abort/grep_prop/set_perm 等）和一组环境变量
// （MODPATH/MODID/ZIPFILE/ARCH/API/BOOTMODE/OUTFD 等）。
//
// 本函数生成一段 shell prologue，由调用方用 heredoc 喂给 sh，再 source 目标脚本。
// prologue 语义对齐 Magisk 官方 util_functions.sh（关键函数签名/变量名一致），
// 但只实现足够让模块脚本跑过"业务逻辑"的最小闭环，不包含真正的挂载/sepolicy。
static std::string build_magisk_shim(const std::string& modroot,
                                     const std::string& modid,
                                     const std::string& zip_path) {
    std::string p;
    // --- 环境变量（对齐 Magisk install_module/setup 的注入） ---
    p += "MODPATH='" + modroot + "'\n";          // 模块落盘目录
    p += "MODID='" + modid + "'\n";              // 模块 id
    p += "ZIPFILE='" + zip_path + "'\n";         // 上传的 zip 绝对路径
    p += "MODNAME='" + modid + "'\n";            // 模块名（取 id 兜底）
    p += "MODAUTH=\n";                            // 认证信息（Magisk 默认空）
    p += "OUTFD=1\n";                             // ui_print 输出到 stdout
    p += "BOOTMODE=true\n";                       // 视为已启动模式（非 recovery）
    p += "TMPDIR=/dev/tmp\n";                     // 临时目录
    p += "MAGISKBIN=/data/adb/magisk\n";          // magisk 二进制目录
    p += "ARCH=arm64\n";                          // 探测不到的架构兜底（真机 arm64）
    p += "ABI=arm64-v8a\n";
    p += "IS64BIT=true\n";
    p += "API=34\n";                              // Android 14 兜底（实际应探测，暂兜底）
    p += "ASH_STANDALONE=1\n";                    // 独立 busybox shell
    p += "REPLACE=\n";                            // Magisk install_module 默认为空
    p += "REMOVE=\n";
    p += "\n";
    // --- ui_print：对齐 util_functions.sh（用 [ ] 而非 [[ ]]，保证 dash/mksh 兼容） ---
    p += "ui_print() {\n";
    p += "  if $BOOTMODE; then echo \"$1\"; else echo -e \"ui_print $1\\nui_print\" >> /proc/self/fd/$OUTFD; fi\n";
    p += "}\n";
    // --- grep_prop / grep_get_prop：对齐 util_functions.sh 第 43/51 行 ---
    p += "grep_prop() { local REGEX=\"s/^$1=//p\"; shift; local FILES=\"$@\";\n";
    p += "  [ -z \"$FILES\" ] && FILES='/system/build.prop';\n";
    p += "  cat $FILES 2>/dev/null | sed -n \"$REGEX\" 2>/dev/null | head -n 1; }\n";
    p += "grep_get_prop() { local result=$(grep_prop \"$@\");\n";
    p += "  if [ -z \"$result\" ]; then getprop \"$1\"; else echo \"$result\"; fi; }\n";
    // --- getvar：对齐 util_functions.sh 第 61 行 ---
    p += "getvar() { local VARNAME=$1; local VALUE=;\n";
    p += "  VALUE=$(grep_prop $VARNAME /sdk.prop /default.prop /system/build.prop 2>/dev/null);\n";
    p += "  [ -z \"$VALUE\" ] && VALUE=$(getprop $VARNAME 2>/dev/null);\n";
    p += "  [ -z \"$VALUE\" ] || eval \"$VARNAME=\\$VALUE\"; }\n";
    // --- abort：对齐 util_functions.sh 第 75 行（会清理 MODPATH + TMPDIR） ---
    p += "abort() { ui_print \"$1\"; [ -z \"$MODPATH\" ] || rm -rf \"$MODPATH\"; rm -rf \"$TMPDIR\" 2>/dev/null; exit 1; }\n";
    // --- api_level_arch_detect：对齐 util_functions.sh，动态探测而非硬编码 ---
    p += "api_level_arch_detect() {\n";
    p += "  API=$(grep_get_prop ro.build.version.sdk);\n";
    p += "  ABI=$(grep_get_prop ro.product.cpu.abi);\n";
    p += "  [ -z \"$API\" ] && API=34;\n";
    p += "  [ -z \"$ABI\" ] && ABI='arm64-v8a';\n";
    p += "  if [ \"$ABI\" = \"arm64-v8a\" ]; then ARCH=arm64; ABI32=armeabi-v7a; IS64BIT=true;\n";
    p += "  elif [ \"$ABI\" = \"x86_64\" ]; then ARCH=x64; ABI32=x86; IS64BIT=true;\n";
    p += "  elif [ \"$ABI\" = \"armeabi-v7a\" ]; then ARCH=arm; ABI32=armeabi-v7a; IS64BIT=false;\n";
    p += "  elif [ \"$ABI\" = \"x86\" ]; then ARCH=x86; ABI32=x86; IS64BIT=false;\n";
    p += "  elif [ \"$ABI\" = \"riscv64\" ]; then ARCH=riscv64; ABI32=riscv32; IS64BIT=true;\n";
    p += "  fi; }\n";
    // --- set_perm：对齐 util_functions.sh 第 604 行（target owner group mode [context]） ---
    p += "set_perm() {\n";
    p += "  chown $2:$3 \"$1\" 2>/dev/null || return 1\n";
    p += "  chmod $4 \"$1\" 2>/dev/null || return 1\n";
    p += "  local CON=$5\n";
    p += "  [ -z \"$CON\" ] && CON=u:object_r:system_file:s0\n";
    p += "  command -v chcon >/dev/null 2>&1 && chcon \"$CON\" \"$1\" 2>/dev/null\n";
    p += "  return 0\n";
    p += "}\n";
    p += "set_perm_recursive() {\n";
    p += "  find \"$1\" -type d 2>/dev/null | while read dir; do\n";
    p += "    set_perm \"$dir\" $2 $3 $4 $6\n";
    p += "  done\n";
    p += "  find \"$1\" -type f 2>/dev/null | while read file; do\n";
    p += "    set_perm \"$file\" $2 $3 $5 $6\n";
    p += "  done\n";
    p += "  return 0\n";
    p += "}\n";
    // --- mktouch：对齐 util_functions.sh 第 621 行 ---
    p += "mktouch() { mkdir -p \"${1%/*}\" 2>/dev/null; [ -z \"$2\" ] && touch \"$1\" || echo \"$2\" > \"$1\"; chmod 644 \"$1\" 2>/dev/null; }\n";
    p += "\n";
    return p;
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
    //    customize.sh 通过 Magisk shim 仿真环境 source 执行（而不是裸 sh），
    //    使 ui_print/abort/grep_prop/set_perm 等内置函数和 MODPATH/ZIPFILE 等变量可用。
    {
        add("[5a] 执行 customize.sh（shim 仿真环境）");
        std::string shim = build_magisk_shim(modroot, modid, zip_path);
        // 把 shim prologue 写到模块目录下的 .skroot_shim.sh，再 source 它 + customize.sh
        std::string shim_file = join_path(modroot, ".skroot_shim.sh");
        KModErr ew = rsh("cat > " + sq(shim_file) + " <<'SKROOT_SHIM_EOF'\n"
                        + shim + "SKROOT_SHIM_EOF\n"
                        "chmod 0644 " + sq(shim_file) + " 2>&1; echo rc=$?", out);
        if (!is_ok(ew) || out.find("rc=0") == std::string::npos) {
            add("  ✗ shim 落盘失败 err=" + to_string(ew) + " out=" + out);
        }
        KModErr e = rsh("if [ -f " + sq(modroot) + "/customize.sh ]; then "
                        "  echo '--- customize.sh 开始（shim）---'; "
                        "  sh -c '. " + sq(shim_file) + "; api_level_arch_detect; . " + sq(modroot) + "/customize.sh' 2>&1; echo '--- customize.sh 退出码='$?; "
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