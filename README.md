# deepseek-web

Morph 原生扩展（`type = "so"`），通过 `ext_run_v2` 把 DeepSeek
[Search / Fetch API](https://api-docs.deepseek.com/zh-cn/guides/search/)
注册成工具 `deepseek_web`。

## 要求

- Morph 提供 ext SDK 1.1 的 `get_config_string` 宿主接口。v0.5.0 发布版
  不支持；仅有最初的 `ext_run_v2` 接口也不够。
- `gcc`、`make`、`curl`、`tar`、`cmake` ≥ 3.20、libcurl。

## 构建与安装

插件自带 Morph 导出的公开 SDK 头文件（`sdk/include/morph/`），可以独立编译，
不需要 Morph 源码、`MORPH_SRC` 或宿主内部配置结构。首次构建只下载 nbase。

在支持上述接口的 Morph 内执行：

```text
/ext install github:oxUnd/deepseek-web@main --yes
```

安装后重启 Morph 加载插件。

也可以在插件目录本地构建、测试和安装：

```bash
make
make test
make install
```

默认安装到 `~/.morph/exts/deepseek_web`，自定义目录使用
`make install EXT_INSTALL_DIR=/absolute/path/to/exts/deepseek_web`。

## SDK

`sdk/include/morph/ext.h` 和 `ext_types.h` 是 Morph `include/morph/` 的公开
头文件副本。它们声明 ABI，不参与编译 Morph，也不链接 Morph 私有库。
更新 SDK 时应从 Morph 导出产物同步，不能复制或猜测内部 `struct config` 布局。
可用 `MORPH_SDK=/path/to/exported/include` 替换随包头文件。

插件通过 `struct_size` 检查宿主是否提供配置接口；旧宿主会收到明确的升级提示。

## API key

插件通过宿主的 `get_config_string` 读取当前调用生效的
`model.text.api_key` / `model.text.api_key_env`，必要时回退到 `model.vision`，
最后回退到环境变量 `DEEPSEEK_API_KEY`。当前文本模型必须使用 DeepSeek
adapter，否则插件会在发起请求前直接拒绝。

## 测试

```bash
make test
```

测试使用本地 mock HTTP 服务覆盖配置读取、授权头、search/fetch、旧宿主兼容性
和错误分支。对真实 API 跑一次：

```bash
python3 test_deepseek_web.py --lib ./deepseek_test.so --real
```
