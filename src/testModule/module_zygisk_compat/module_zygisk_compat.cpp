#include <iostream>
#include <cstring>
#include <cstdio>
#include <string>
#include <ctime>
#include "kernel_module_kit_umbrella.h"

// SKRoot 模块入口（签名见官方 module_descriptor.h）
int skroot_module_main(const char* root_key, const char* module_private_dir) {
    printf("[SKZygiskCompat] entry called\n");
    printf("[SKZygiskCompat] root_key len=%zu\n", strlen(root_key));
    printf("[SKZygiskCompat] module_private_dir=%s\n", module_private_dir);

    // 自检1：官方 README 声明 root_key 为 48 位随机字符串
    bool key_ok = (strlen(root_key) == 48);
    printf("[SKZygiskCompat] root_key check: %s\n", key_ok ? "PASS(48)" : "WARN(not48)");

    // 落盘证据：状态文件写入模块私有目录（证明入口真被调用）
    std::string status_path = std::string(module_private_dir) + "/skzygiskcompat_status.txt";
    bool file_ok = false;
    FILE* f = fopen(status_path.c_str(), "w");
    if (f) {
        fprintf(f, "entry_called=1\n");
        fprintf(f, "root_key_len=%zu\n", strlen(root_key));
        fprintf(f, "module_private_dir=%s\n", module_private_dir);
        fprintf(f, "unix_time=%ld\n", (long)time(nullptr));
        fclose(f);
        file_ok = true;
    }
    printf("[SKZygiskCompat] status file: %s\n", file_ok ? "written" : "FAILED");
    printf("[SKZygiskCompat] done\n");
    return (key_ok && file_ok) ? 0 : 1;
}

// WebUI HTTP 服务器回调（基于 civetweb，写法照官方 module_web_ui_example）
class ZygiskCompatWebHandler : public kernel_module::WebUIHttpHandler {
public:
    void onPrepareCreate(const char* root_key, const char* module_private_dir, uint32_t port) override {
        printf("[SKZygiskCompat] WebUI root_key len=%zu\n", strlen(root_key));
        printf("[SKZygiskCompat] WebUI module_private_dir=%s\n", module_private_dir);
        printf("[SKZygiskCompat] WebUI port=%u\n", port);
    }

    bool handleGet(CivetServer* server, struct mg_connection* conn, const std::string& path, const std::string& query) override {
        printf("[SKZygiskCompat] GET path=%s query=%s\n", path.c_str(), query.c_str());
        return false; // 返回 false 走 civetweb 默认静态文件服务（webroot/）
    }

    bool handlePost(CivetServer* server, struct mg_connection* conn, const std::string& path, const std::string& body) override {
        printf("[SKZygiskCompat] POST path=%s body=%s\n", path.c_str(), body.c_str());

        std::string resp;
        if (path == "/getStatus") {
            resp = std::string("{\"entry\":\"ok\",\"pid\":") + std::to_string(getpid()) +
                   ",\"uid\":" + std::to_string(getuid()) + "}";
        } else if (path == "/getTime") {
            char buf[64];
            snprintf(buf, sizeof(buf), "%ld", (long)time(nullptr));
            resp = buf;
        } else {
            resp = "{}";
        }

        kernel_module::webui::send_json(conn, 200, resp);
        return true;
    }
};

// SKRoot 模块名片（字段说明见 module_descriptor.h）
SKROOT_MODULE_NAME("SKZygiskCompat")
SKROOT_MODULE_VERSION("0.2.0")
SKROOT_MODULE_DESC("Magisk/Zygisk compatibility layer for SKRoot Pro (WebUI build)")
SKROOT_MODULE_AUTHOR("oopnv70-lab")
SKROOT_MODULE_ID32("Zg4c0mp4t1b3L4y3r5kR00tPr0202601")
SKROOT_MODULE_WEB_UI(ZygiskCompatWebHandler)