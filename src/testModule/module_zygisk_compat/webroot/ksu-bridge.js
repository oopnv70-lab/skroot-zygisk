// ksu-bridge.js
// 在 SKRoot 的浏览器环境里伪造 KernelSU 的 window.ksu 桥，
// 把 ksu.exec(cmd, options, callback) 转发到后端 /ksuExec 接口（root shell 执行）。
// 对齐 KernelSU 官方 js/index.js 的 exec 契约：
//   ksu.exec(command, JSON.stringify(options), callbackName)
//   回调签名：window[callbackName](errno, stdout, stderr)
(function () {
  // 若页面已自带原生 ksu（KernelSU/APatch 环境），则不覆盖，直接返回原生桥
  if (window.ksu && window.ksu.exec) {
    return;
  }

  let callbackCounter = 0;

  function getUniqueCallbackName(prefix) {
    return prefix + "_callback_" + Date.now() + "_" + (callbackCounter++);
  }

  // 真正的底层执行：POST 到 /ksuExec，body 是命令字符串
  async function rawExec(command, options) {
    const resp = await fetch('/ksuExec', { method: 'POST', body: command });
    const txt = await resp.text();
    try {
      return JSON.parse(txt); // { errno, stdout, stderr }
    } catch (e) {
      return { errno: -1, stdout: '', stderr: 'ksuExec 响应非 JSON: ' + txt };
    }
  }

  // 处理 options：KernelSU 的 exec options 是 { cwd, env }
  function applyOptions(command, options) {
    let final = command;
    let opts = {};
    if (options) {
      try { opts = typeof options === 'string' ? JSON.parse(options) : options; }
      catch (e) { opts = {}; }
    }
    if (opts.cwd) final = 'cd ' + JSON.stringify(opts.cwd) + ';' + final;
    if (opts.env) {
      for (const k in opts.env) final = 'export ' + k + '=' + opts.env[k] + ';' + final;
    }
    return final;
  }

  const ksu = {
    // 回调版本（Zygisk Next 等模块直接调用这个三参形式）
    exec(command, options, callbackFunc) {
      const final = applyOptions(command, options);
      rawExec(final, options).then((r) => {
        const cb = window[callbackFunc];
        if (typeof cb === 'function') {
          cb(r.errno, r.stdout, r.stderr);
        }
      }).catch((e) => {
        const cb = window[callbackFunc];
        if (typeof cb === 'function') {
          cb(-1, '', String(e));
        }
      });
    },
    // Promise 版本（官方 js/index.js 的封装形态）
    execPromisified(command, options) {
      return new Promise((resolve, reject) => {
        const callbackFuncName = getUniqueCallbackName('exec');
        window[callbackFuncName] = (errno, stdout, stderr) => {
          resolve({ errno, stdout, stderr });
          delete window[callbackFuncName];
        };
        try {
          ksu.exec(command, JSON.stringify(options || {}), callbackFuncName);
        } catch (e) {
          reject(e);
          delete window[callbackFuncName];
        }
      });
    },
    // 简单对象：toast 之类直接打 log，不做真 UI
    toast(msg) { console.log('[ksu.toast]', msg); },
    fullScreen(v) { console.log('[ksu.fullScreen]', v); },
    enableEdgeToEdge(v) { console.log('[ksu.enableEdgeToEdge]', v); },
    exit() { console.log('[ksu.exit]'); },
    moduleInfo() { return '{}'; },
    listPackages() { return '[]'; },
    getPackagesInfo() { return '[]'; },
  };

  // Zygisk Next 的 index.html 里有这样一段：
  //   if (window.apatch !== undefined && window.apatch.ksu === undefined) window.ksu = window.apatch
  // 这里无需处理 apatch，直接暴露 ksu 即可
  window.ksu = ksu;

  // 同时暴露 apatch（部分模块会引用 window.apatch，等价指向 ksu）
  if (!window.apatch) {
    window.apatch = ksu;
  }
})();
