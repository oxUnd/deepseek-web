# deepseek-web

Morph 原生扩展（`type = "so"`），通过 `ext_run_v2` 把 DeepSeek
[Search / Fetch API](https://api-docs.deepseek.com/zh-cn/guides/search/)
注册成工具 `deepseek_web`。

## 要求

- Morph 提供 ext SDK 1.1 的 `get_config_string` 宿主接口。v0.5.0 发布版
  不支持；仅有最初的 `ext_run_v2` 接口也不够。
- C11 编译器（Clang/GCC）、make、curl、tar、CMake ≥ 3.20，以及
  libcurl ≥ 7.85（支持异步 DNS）。

## 构建与安装

```bash
make
make test
make install
```

插件自带 Morph 导出的公开 ext SDK，可以独立编译，不需要 Morph 源码。
首次构建下载 nothing 的固定提交
`fe8cd34e7bf0b401f553eb908794dbb5f2c6c7a3`，校验归档的 SHA-256 后构建
启用 HTTP 的静态 nbase。依赖缓存在 `.deps/`，后续构建复用缓存。

源码统一使用 `<nb/xxx.h>` 公共头文件，不手写 nbase 类型或 `extern` 声明。
编译时优先使用该提交的头文件，并链接同一份源码生成的静态库，避免声明与
实现的版本错配。nbase/cJSON 保留在插件内部；运行时不需要 Morph 导出 nbase
函数，也不需要额外安装 nbase 动态库。libcurl 仍是系统链接依赖。

在支持上述 ext SDK 的 Morph 内也可以执行：

```text
/ext install github:oxUnd/deepseek-web@main --yes
```

安装构建完成后会移除源码包里的依赖缓存和目标文件。安装后重启 Morph 加载插件。

本地 `make install` 默认安装到 `~/.morph/exts/deepseek_web`；自定义目录使用
`make install EXT_INSTALL_DIR=/absolute/path/to/exts/deepseek_web`。
`make clean` 清理插件构建产物，`make distclean` 同时清理依赖缓存。

## 依赖与 SDK

Makefile 固定 nothing 提交及下载归档 SHA-256。需要镜像时可设置 `NBASE_URL`，
镜像内容仍须通过相同校验。更新依赖时同步更新提交和校验值，重新构建并测试。

`sdk/manifest.json` 固定公开 ext SDK 1.1.0、ABI 1，以及随包头文件的来源和
SHA-256。更新 SDK 时使用 `morph-ext-development` skill 内的
`scripts/scaffold.py sdk` 选择确切版本。可用 `MORPH_SDK=/path/to/exported/include`
替换随包声明；插件不访问 Morph 私有配置结构。

编译的 nbase include 路径排在 SDK 前面，因此 `<nb/http.h>`、`<nb/time.h>`
以及 SDK 间接使用的 nbase 声明均来自同一固定提交。

HTTP 请求使用 `nb_http_*`，保留 60 秒总超时、15 秒连接超时和 8 MiB 响应上限。
请求期间每次 poll 最多等待 50 ms，并检查宿主取消状态和调用 deadline。
最多跟随 3 次同源重定向；跨源重定向作为 HTTP 错误返回。

## API key

插件通过宿主的 `get_config_string` 读取当前调用生效的
`model.text.api_key` / `model.text.api_key_env`，必要时回退到 `model.vision`，
最后回退到环境变量 `DEEPSEEK_API_KEY`。当前文本模型必须使用 DeepSeek
adapter，否则插件会在发起请求前直接拒绝。

API 地址默认是 `https://api.deepseek.com`，可通过 `DEEPSEEK_API_BASE` 覆盖。

## 测试

```bash
make test
```

测试库复用生产插件的目标文件及固定版本的 nbase，通过测试配置宿主调用
`ext_run_v2`。本地 mock 服务覆盖配置读取、授权头、Search / Fetch、旧 ext SDK
宿主、HTTP 错误、空响应、大小限制、重定向、非法密钥，以及请求前和请求中的
取消与 deadline。

对真实 API 跑一次（需要有效密钥）：

```bash
python3 test_deepseek_web.py --lib ./deepseek_test.so --real
```
