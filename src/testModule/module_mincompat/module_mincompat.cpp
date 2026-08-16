#include <iostream>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include "kernel_module_kit_umbrella.h"

using namespace skroot_env;

// 全局保存，供 WebUI 使用
static std::string g_root_key;
static std::string g_private_dir;

static void log_report(const std::string& line) {
    printf("[MinCompat] %s\n", line.c_str());
}

static const char* MODROOT = "/data/adb/modules";
static const char* MODID   = "testmod";

// 写入一个最小 Magisk 模块，并真正执行它的 service.sh（这就是「运行 Magisk 模块」的最小验证）。
// 全程只读/建目录/写文本/执行无害脚本；无删除、无 mount、无 install_module、无内核 hook。
static void run_container_probe(const char* root_key) {
    std::string out;
    KModErr e;

    // 1) 造 /data/adb 骨架 + 模块目录
    std::string mkdir_cmd = std::string("mkdir -p ") + MODROOT + "/" + MODID;
    e = run_root_cmd(root_key, mkdir_cmd.c_str(), out);
    log_report("mkdir => " + to_string(e) + " | out=[" + out + "]");

    // 2) 写入 module.prop（严格符合 Magisk 契约：id 正则 ^[a-zA-Z][a-zA-Z0-9._-]+$）
    {
        std::string prop = "id=testmod\nname=MinCompat Test Module\nversion=1.0.0\nversionCode=1\n"
                           "author=oopnv70-lab\ndescription=minimal magisk module inside skroot container\n";
        std::string cmd = std::string("cat > ") + MODROOT + "/" + MODID + "/module.prop <<'EOF'\n" + prop + "EOF\n";
        e = run_root_cmd(root_key, cmd.c_str(), out);
        log_report("write module.prop => " + to_string(e) + " | out=[" + out + "]");
    }

    // 3) 写入一个无害的 service.sh，并在末尾打一行标记到日志文件
    {
        std::string sh = "#!/system/bin/sh\n# MinCompat test service.sh: harmless\n"
                         "echo '[MinCompat] service.sh ran' >> /data/local/tmp/mincompat_service.log\n";
        std::string cmd = std::string("cat > ") + MODROOT + "/" + MODID + "/service.sh <<'EOF'\n" + sh + "EOF\n"
                         "chmod 755 " + std::string(MODROOT) + "/" + MODID + "/service.sh\n";
        e = run_root_cmd(root_key, cmd.c_str(), out);
        log_report("write service.sh => " + to_string(e) + " | out=[" + out + "]");
    }

    // 4) 真正执行 service.sh —— 这是「加载并运行一个 Magisk 模块脚本」的最小验证
    {
        std::string cmd = std::string(MODROOT) + "/" + MODID + "/service.sh; echo EXIT_CODE=$?";
        e = run_root_cmd(root_key, cmd.c_str(), out);
        log_report("run service.sh => " + to_string(e) + " | out=[" + out + "]");
    }

    // 5) 读回验证：目录列表 + module.prop + service.sh + service 日志
    {
        std::string cmd = std::string("ls -la ") + MODROOT + "/" + MODID + "/";
        e = run_root_cmd(root_key, cmd.c_str(), out);
        log_report("ls => " + to_string(e) + " | out=[" + out + "]");
    }
    {
        std::string cmd = std::string("cat ") + MODROOT + "/" + MODID + "/module.prop";
        e = run_root_cmd(root_key, cmd.c_str(), out);
        log_report("cat module.prop => " + to_string(e) + " | out=[" + out + "]");
    }
    {
        std::string cmd = std::string("cat /data/local/tmp/mincompat_service.log 2>&1");
        e = run_root_cmd(root_key, cmd.c_str(), out);
        log_report("cat service.log => " + to_string(e) + " | out=[" + out + "]");
    }

    // 6) 命门观察：当前进程 uid 与 /data/adb/modules 可见性
    {
        std::string cmd = std::string("id; ls -la ") + MODROOT + " 2>&1";
        e = run_root_cmd(root_key, cmd.c_str(), out);
        log_report("id+ls /data/adb/modules => " + to_string(e) + " | out=[" + out + "]");
    }
}

// skroot_module_main 入口 —— 这是本模块被 SKRoot 加载的唯一入口，必须尽快返回。
int skroot_module_main(const char* root_key, const char* module_private_dir) {
    g_root_key = root_key ? root_key : "";
    g_private_dir = module_private_dir ? module_private_dir : "";

    printf("[MinCompat] === entry called ===\n");
    printf("[MinCompat] root_key_len=%zu\n", root_key ? strlen(root_key) : 0);
    printf("[MinCompat] module_private_dir=%s\n", module_private_dir ? module_private_dir : "(null)");

    KModErr e = set_skroot_log_enabled(root_key, true);
    log_report("set_log_enabled => " + to_string(e));

    e = get_root(root_key);
    log_report("get_root => " + to_string(e));

    run_container_probe(root_key);

    printf("[MinCompat] === entry done ===\n");
    return 0;
}

// WebUI：浏览器可打开，查看执行结果日志
class MinCompatWebHandler : public kernel_module::WebUIHttpHandler {
public:
    void onPrepareCreate(const char* root_key, const char* module_private_dir, uint32_t port) override {
        g_root_key = root_key ? root_key : "";
        g_private_dir = module_private_dir ? module_private_dir : "";
        printf("[MinCompat] WebUI port=%u dir=%s\n", port, g_private_dir.c_str());
    }

    bool handleGet(CivetServer* server, struct mg_connection* conn, const std::string& path, const std::string& query) override {
        (void)server; (void)conn; (void)query;
        printf("[MinCompat] GET %s\n", path.c_str());
        return false; // 交给 CivetWeb 默认静态文件服务，返回 webroot/index.html
    }

    bool handlePost(CivetServer* server, struct mg_connection* conn, const std::string& path, const std::string& body) override {
        (void)server; (void)body;
        printf("[MinCompat] POST %s\n", path.c_str());

        if (path == "/getLog" || path == "/getReport") {
            std::string log;
            KModErr e = read_skroot_log(g_root_key.c_str(), log);
            if (is_ok(e)) {
                kernel_module::webui::send_text(conn, 200, log);
            } else {
                std::string msg = "read_skroot_log failed: " + to_string(e);
                kernel_module::webui::send_text(conn, 500, msg);
            }
            return true;
        }

        kernel_module::webui::send_text(conn, 200, "{}");
        return true;
    }
};

SKROOT_MODULE_NAME("MinCompat")
SKROOT_MODULE_VERSION("0.1.0")
SKROOT_MODULE_DESC("minimal container: extract+run a tiny magisk module inside SKRoot")
SKROOT_MODULE_AUTHOR("oopnv70-lab")
SKROOT_MODULE_ID32("1234567890abcdef1234567890abcdef")
SKROOT_MODULE_WEB_UI(MinCompatWebHandler)