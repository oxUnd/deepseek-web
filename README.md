# deepseek-web

Morph 原生扩展（`type = "so"`），通过 `ext_run_v2` 把 DeepSeek
[Search / Fetch API](https://api-docs.deepseek.com/zh-cn/guides/search/)
注册成工具 `deepseek_web`。

## 要求

- Morph 支持 `ext_run_v2`（`a70d6dcb` 及之后；v0.5.0 发布版还没有这个入口）。
- 一份与运行中 Morph 匹配的源码 checkout，用于 `ext/context.h` 和配置结构布局。
- `gcc`、`make`、`curl`、`tar`、`cmake` ≥ 3.20、libcurl。

## 构建与安装

插件默认从 `../morph` 读取 Morph 头文件。插件与 Morph 源码并列时直接构建：

```bash
make
```

使用其他源码路径：

```bash
make MORPH_SRC=~/dev/AI/morph
```

安装到 `~/.morph/exts/deepseek_web`：

```text
MORPH_SRC=~/dev/AI/morph /ext install github:oxUnd/deepseek-web@main --yes
```

也可以手动复制：

```bash
make MORPH_SRC=~/dev/AI/morph
mkdir -p ~/.morph/exts/deepseek-web
cp manifest.toml deepseek_web.so ~/.morph/exts/deepseek-web/
```

## API key

插件从 `ext_context` 的配置快照读取 `[model.text]` 的 `api_key` /
`api_key_env`，必要时回退到 `[model.vision]`，最后回退到环境变量
`DEEPSEEK_API_KEY`。当前文本模型必须使用 DeepSeek adapter，否则插件会在
发起请求前直接拒绝。

## 测试

```bash
make test
```

测试使用本地 mock HTTP 服务覆盖配置读取、授权头、search/fetch 和错误分支。
对真实 API 跑一次：

```bash
python3 test_deepseek_web.py --lib ./deepseek_test.so --real
```
