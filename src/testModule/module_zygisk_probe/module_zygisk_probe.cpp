#include <iostream>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include "kernel_module_kit_umbrella.h"

using namespace skroot_env;
using kernel_module::to_string;

// 全局保存，供 WebUI 使用
static std::string g_root_key;
static std::string g_private_dir;

// 把一行结果同时打到 stdout 和 SKRoot 日志
static void log_report(const std::string& line) {
    printf("[SKZygiskProbe] %s\n", line.c_str());
}

// skroot_module_main 入口
int skroot_module_main(const char* root_key, const char* module_private_dir) {
    g_root_key = root_key ? root_key : "";
    g_private_dir = module_private_dir ? module_private_dir : "";

    printf("[SKZygiskProbe] === entry called ===\n");
    printf("[SKZygiskProbe] root_key_len=%zu\n", strlen(root_key));

    // 1. 开日志
    KModErr e = set_skroot_log_enabled(root_key, true);
    log_report("set_log_enabled => " + to_string(e));

    // 2. get_root
    e = get_root(root_key);
    log_report("get_root => " + to_string(e));

    // 3. 执行 id（验证 root 身份是否真实）
    std::string out;
    e = run_root_cmd(root_key, "id", out);
    log_report("run_root_cmd(id) => " + to_string(e) + " | out=[" + out + "]");

    // 4. uname
    e = run_root_cmd(root_key, "uname -a", out);
    log_report("run_root_cmd(uname -a) => " + to_string(e) + " | out=[" + out + "]");

    // 5. mount 里 data 相关（验证 /data 挂载现状）
    e = run_root_cmd(root_key, "mount | grep -i data", out);
    log_report("run_root_cmd(mount|grep data) => " + to_string(e) + " | out=[" + out + "]");

    // 6. 列出已安装模块
    std::vector<module_record> mods;
    e = get_all_modules_list(root_key, mods);
    log_report("get_all_modules_list => " + to_string(e) + " | count=" + std::to_string(mods.size()));
    for (auto& m : mods) {
        log_report("  module: id32=" + std::string(m.desc.id32) +
                   " name=" + std::string(m.desc.name) +
                   " ver=" + std::string(m.desc.version));
    }

    // 7. SKRoot 基础能力自测
    for (int i = 0; i <= 5; ++i) {
        std::string r;
        KModErr ee = test_skroot_basics(root_key, (BasicItem)i, r);
        log_report("test_basics[" + std::to_string(i) + "] => " + to_string(ee) + " | out=[" + r + "]");
    }

    printf("[SKZygiskProbe] === entry done ===\n");
    return 0;
}

// WebUI：提供 /getLog 端点，把 SKRoot 日志读出来显示
class ZygiskProbeWebHandler : public kernel_module::WebUIHttpHandler {
public:
    void onPrepareCreate(const char* root_key, const char* module_private_dir, uint32_t port) override {
        g_root_key = root_key ? root_key : "";
        g_private_dir = module_private_dir ? module_private_dir : "";
        printf("[SKZygiskProbe] WebUI port=%u dir=%s\n", port, g_private_dir.c_str());
    }

    bool handleGet(CivetServer* server, struct mg_connection* conn, const std::string& path, const std::string& query) override {
        (void)server; (void)conn; (void)query;
        printf("[SKZygiskProbe] GET %s\n", path.c_str());
        return false; // 默认静态文件服务
    }

    bool handlePost(CivetServer* server, struct mg_connection* conn, const std::string& path, const std::string& body) override {
        (void)server; (void)body;
        printf("[SKZygiskProbe] POST %s\n", path.c_str());

        if (path == "/getLog") {
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

        if (path == "/getReport") {
            std::string log;
            read_skroot_log(g_root_key.c_str(), log);
            kernel_module::webui::send_text(conn, 200, log);
            return true;
        }

        kernel_module::webui::send_text(conn, 200, "{}");
        return true;
    }
};

SKROOT_MODULE_NAME("SKZygiskProbe")
SKROOT_MODULE_VERSION("0.1.0")
SKROOT_MODULE_DESC("probe: verify root/cmd/module-list on SKRoot Pro")
SKROOT_MODULE_AUTHOR("oopnv70-lab")
SKROOT_MODULE_ID32("Pr0b3Zyg1skR00tEnv0000000000001x")
SKROOT_MODULE_WEB_UI(ZygiskProbeWebHandler)