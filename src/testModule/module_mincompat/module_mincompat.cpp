#include <iostream>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include "kernel_module_kit_umbrella.h"

using namespace skroot_env;

static void log_report(const std::string& line) {
    printf("[MinCompat] %s\n", line.c_str());
}

// 最小容器模块：验证「容器内部 解包→落盘→读回」一个最小 Magisk 模块，并顺带探 /data/adb 可见性。
// 全程只读/建目录/写文本，无删除、无 mount、无 install_module、无内核 hook。
int skroot_module_main(const char* root_key, const char* module_private_dir) {
    printf("[MinCompat] === entry called ===\n");
    printf("[MinCompat] root_key_len=%zu\n", root_key ? strlen(root_key) : 0);
    printf("[MinCompat] module_private_dir=%s\n", module_private_dir ? module_private_dir : "(null)");

    // 日志开关（可选，失败不致命）
    KModErr e = set_skroot_log_enabled(root_key, true);
    log_report("set_log_enabled => " + to_string(e));

    // 提权
    e = get_root(root_key);
    log_report("get_root => " + to_string(e));

    // 1) 造 /data/adb 骨架 + 模块目录
    std::string out;
    const char* MODROOT = "/data/adb/modules";
    const char* MODID   = "testmod";

    std::string mkdir_cmd = std::string("mkdir -p ") + MODROOT + "/" + MODID;
    e = run_root_cmd(root_key, mkdir_cmd.c_str(), out);
    log_report("mkdir => " + to_string(e) + " | out=[" + out + "]");

    // 2) 内置一个最小 Magisk 模块的 module.prop
    //    用单引号包裹 + heredoc 方式写入，避免转义地狱。内容严格符合 Magisk 契约。
    {
        std::string prop = "id=testmod\nname=MinCompat Test Module\nversion=1.0.0\nversionCode=1\n"
                           "author=oopnv70-lab\ndescription=minimal magisk module inside skroot container\n";
        std::string cmd = std::string("cat > ") + MODROOT + "/" + MODID + "/module.prop <<'EOF'\n" + prop + "EOF\n";
        e = run_root_cmd(root_key, cmd.c_str(), out);
        log_report("write module.prop => " + to_string(e) + " | out=[" + out + "]");
    }

    // 3) 内置一个无害的 service.sh
    {
        std::string sh = "#!/system/bin/sh\n# MinCompat test service.sh: harmless echo to log\n"
                         "echo '[MinCompat] service.sh ran' >> /data/local/tmp/mincompat_service.log\n";
        std::string cmd = std::string("cat > ") + MODROOT + "/" + MODID + "/service.sh <<'EOF'\n" + sh + "EOF\n"
                         "chmod 755 " + std::string(MODROOT) + "/" + MODID + "/service.sh\n";
        e = run_root_cmd(root_key, cmd.c_str(), out);
        log_report("write service.sh => " + to_string(e) + " | out=[" + out + "]");
    }

    // 4) 读回验证：ls 目录 + cat module.prop + cat service.sh
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
        std::string cmd = std::string("cat ") + MODROOT + "/" + MODID + "/service.sh";
        e = run_root_cmd(root_key, cmd.c_str(), out);
        log_report("cat service.sh => " + to_string(e) + " | out=[" + out + "]");
    }

    // 5) 探命门：以相册普通 shell 视角（非 root 的 su 语境）读同一个文件，观察挂载命名空间可见性。
    //    只读观察，不做任何修改。用 /system/bin/toybox 或直接 cat 由 root 降权不现实，
    //    这里退而求其次：记录当前进程 uid 与 /data/adb 是否可见（root 视角 vs 全局可见性）。
    {
        std::string cmd = std::string("id; ls -la ") + MODROOT + " 2>&1";
        e = run_root_cmd(root_key, cmd.c_str(), out);
        log_report("id+ls /data/adb/modules => " + to_string(e) + " | out=[" + out + "]");
    }

    printf("[MinCompat] === entry done ===\n");
    return 0;
}

// 名片（必填）。ID32 固定 32 位 hex 随机串，一次生成后不变。
SKROOT_MODULE_NAME("MinCompat")
SKROOT_MODULE_VERSION("0.1.0")
SKROOT_MODULE_DESC("minimal container: extract+land a tiny magisk module inside SKRoot")
SKROOT_MODULE_AUTHOR("oopnv70-lab")
SKROOT_MODULE_ID32("1234567890abcdef1234567890abcdef")
