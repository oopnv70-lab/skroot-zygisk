#include <iostream>
#include <cstring>
#include <cstdio>
#include <string>
#include <ctime>
#include <sys/stat.h>
#include "kernel_module_kit_umbrella.h"

// 上传文件落盘目录（相对 module_private_dir）
static std::string g_private_dir;
static std::string g_last_upload_name;
static long long g_last_upload_size = 0;

// SKRoot 模块入口（签名见官方 module_descriptor.h）
int skroot_module_main(const char* root_key, const char* module_private_dir) {
    printf("[SKZygiskCompat] entry called\n");
    printf("[SKZygiskCompat] root_key len=%zu\n", strlen(root_key));
    printf("[SKZygiskCompat] module_private_dir=%s\n", module_private_dir);

    bool key_ok = (strlen(root_key) == 48);
    printf("[SKZygiskCompat] root_key check: %s\n", key_ok ? "PASS(48)" : "WARN(not48)");

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

// WebUI HTTP 服务器回调（基于 civetweb）
class ZygiskCompatWebHandler : public kernel_module::WebUIHttpHandler {
public:
    void onPrepareCreate(const char* root_key, const char* module_private_dir, uint32_t port) override {
        g_private_dir = module_private_dir ? module_private_dir : "";
        printf("[SKZygiskCompat] WebUI root_key len=%zu\n", strlen(root_key));
        printf("[SKZygiskCompat] WebUI module_private_dir=%s\n", g_private_dir.c_str());
        printf("[SKZygiskCompat] WebUI port=%u\n", port);
    }

    bool handleGet(CivetServer* server, struct mg_connection* conn, const std::string& path, const std::string& query) override {
        printf("[SKZygiskCompat] GET path=%s query=%s\n", path.c_str(), query.c_str());
        return false; // 走 civetweb 默认静态文件服务
    }

    // 文件上传字段回调：让 civetweb 把上传文件流式写盘
    static int field_found_cb(const char* key, const char* filename, char* path, size_t pathlen, void* user_data) {
        (void)user_data;
        if (filename && filename[0]) {
            std::string dir = g_private_dir + "/uploads";
            mkdir(dir.c_str(), 0755);
            std::string dst = dir + "/" + filename;
            g_last_upload_name = filename;
            if (dst.size() + 1 <= pathlen) {
                snprintf(path, pathlen, "%s", dst.c_str());
                return MG_FORM_FIELD_STORAGE_STORE;
            }
        }
        return MG_FORM_FIELD_STORAGE_SKIP;
    }

    bool handlePost(CivetServer* server, struct mg_connection* conn, const std::string& path, const std::string& body) override {
        printf("[SKZygiskCompat] POST path=%s body_len=%zu\n", path.c_str(), body.size());

        if (path == "/upload") {
            g_last_upload_name.clear();
            g_last_upload_size = 0;
            struct mg_form_data_handler fdh;
            memset(&fdh, 0, sizeof(fdh));
            fdh.field_found = field_found_cb;
            fdh.field_get = nullptr;
            fdh.field_store = nullptr;
            fdh.user_data = nullptr;

            int n = mg_handle_form_request(conn, &fdh);
            printf("[SKZygiskCompat] upload fields handled=%d name=%s\n", n, g_last_upload_name.c_str());

            std::string resp = "{\"ok\":true,\"fields\":" + std::to_string(n) +
                               ",\"name\":\"" + g_last_upload_name + "\"}";
            kernel_module::webui::send_json(conn, 200, resp);
            return true;
        }

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
SKROOT_MODULE_VERSION("0.3.0")
SKROOT_MODULE_DESC("Magisk/Zygisk compatibility layer for SKRoot Pro (module picker)")
SKROOT_MODULE_AUTHOR("oopnv70-lab")
SKROOT_MODULE_ID32("Zg4c0mp4t1b3L4y3r5kR00tPr0202601")
SKROOT_MODULE_WEB_UI(ZygiskCompatWebHandler)