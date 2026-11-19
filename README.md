# deepseek-web

> **Morph 开发**（Developed by Morph）

Morph 原生扩展（`type = "so"`），把 DeepSeek 的
[Search / Fetch API](https://api-docs.deepseek.com/zh-cn/guides/search/)
注册成 Morph 工具 `deepseek_web`。

模型可以在对话中自动调用它搜索网页或抓取单个页面，结果以 JSON 观察返回。

> 这不是 DeepSeek 对话接口里的服务端工具 `deepseek_web_search`。
> 本扩展由 Morph 模型发起工具调用，扩展在本地执行 HTTP 请求，调用的是
> 独立的 Search / Fetch API，按成功请求计费。

## 功能

| 能力 | 说明 |
| --- | --- |
| `search` | 按 query 搜索网页，返回结构化结果 |
| `fetch` | 抓取单个网页，返回标题与 Markdown 正文 |
| 自动调用 | 模型根据工具描述和用户问题决定何时调用 |
| 配置读取 | 自己定位 Morph 当前配置，读取 DeepSeek API key |
| Adapter 门控 | 只有文本模型使用 DeepSeek adapter 时才允许请求 |

## 工作流程

```text
用户问题
  -> Morph 模型决定调用 deepseek_web
  -> ext_run(args_json, result_json)
  -> 定位当前 config，检查 [model.text].adapter
  -> 读取 api_key / api_key_env
  -> POST /v1/tools/search 或 /v1/tools/fetch
  -> 返回 JSON，作为工具观察回到模型
```

## 模型限制

扩展只对 DeepSeek 文本模型生效。请求前会检查 `[model.text]`：

| 配置 | 结果 |
| --- | --- |
| `adapter = "deepseek"` | 允许 |
| 未写 `adapter`，但 `provider = "deepseek"` | 允许（Morph 会推断出 DeepSeek adapter） |
| 其他 adapter | 直接返回不支持 |
| 配置无法读取，或找不到 `[model.text]` | 直接返回不支持/无法确认 |

不满足条件时不会使用 API key，也不会发起 HTTP 请求。返回示例：

```json
{
  "error": "当前模型不支持 deepseek_web：需要 [model.text].adapter = \"deepseek\"（当前 adapter=openai-chat-compatible）"
}
```

## 快速开始

### 依赖

- C 编译器与 `make`
- libcurl（macOS 使用系统 SDK；Linux 需要 libcurl 开发包）
- Morph 0.5.0（当前验证版本）

### 构建

```bash
cd deepseek-web
make
```

生成的产物是 `deepseek_web.so`。`cJSON` 已随扩展目录提供，不需要额外安装。

### 安装

```bash
mkdir -p ~/.morph/exts/deepseek-web
cp manifest.toml deepseek_web.so ~/.morph/exts/deepseek-web/
```

### 重启并确认

扩展在 Morph 启动时加载。安装后先退出当前会话，再重新启动：

```bash
# 在 Morph 内
/quit

# shell
morph
```

确认：

```text
/ext list
```

应能看到 `deepseek_web`。`morph --version` 的 `Exts` 数量也应增加。

## 使用

直接用自然语言：

```text
用 deepseek_web 搜一下 "DeepSeek V4.1 release notes"，返回 5 条
用 deepseek_web 抓取 https://api-docs.deepseek.com/zh-cn/guides/search.md
```

模型也可以根据问题自动选择调用。工具名是 `deepseek_web`。

### 参数

| 参数 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `action` | string | 是 | `search` 或 `fetch` |
| `query` | string | `search` 必填 | 搜索词，1–128 个 Unicode 字符 |
| `url` | string | `fetch` 必填 | HTTP/HTTPS 网页地址，最长 8192 |
| `count` | integer | 否 | 返回条数 1–50，默认 10 |
| `need_content` | boolean | 否 | 是否在搜索结果里返回网页正文，默认 false |
| `ori_question` | string | 否 | 原始用户问题，帮助理解搜索意图 |
| `localization` | string | 否 | 用于优先返回本地化结果的 IP 地址 |

### 搜索示例

```json
{
  "action": "search",
  "query": "DeepSeek V4.1 release notes",
  "count": 5,
  "need_content": true
}
```

### 抓取示例

```json
{
  "action": "fetch",
  "url": "https://api-docs.deepseek.com/zh-cn/guides/search.md"
}
```

成功时扩展直接返回 DeepSeek API 的原始 JSON；失败时返回：

```json
{"error": "..."}
```

## 配置与密钥

### 为什么扩展要自己读配置

Morph 的原生扩展 ABI 是：

```c
int ext_run(const char *args_json, char **result_json);
```

**它不会把运行时 `ctx` 传给扩展。** 因此扩展自己定位当前配置，而不是从
`ctx->config` 读取。这是当前 ABI 下的等价方案；如果要严格从 `ctx` 取，
需要给 Morph 增加扩展上下文 ABI。

### 配置文件定位顺序

1. `MORPH_CONFIG` 环境变量
2. `MORPH_CONFIG_PATH` 环境变量
3. 宿主进程的 `-c` / `--config` 参数
   （macOS 通过 `_NSGetArgv`，Linux 通过 `/proc/self/cmdline`）
4. 默认 `~/.morph/config.toml`

### API key 读取顺序

1. `[model.text].api_key`
2. `[model.text].api_key_env` 指向的环境变量
3. `[model.vision].api_key`
4. `[model.vision].api_key_env` 指向的环境变量
5. `DEEPSEEK_API_KEY`

注意：

- 如果 `api_key_env` 指向的变量没有设置，会直接报错，不会静默改用其他 key。
- 配置无法读取时，即使设置了 `DEEPSEEK_API_KEY`，也会先返回“无法确认 adapter”。
- `api_key_env` 指向的变量必须在启动 Morph 之前导出；扩展读取的是 Morph 进程环境。
- 扩展不会把 key 写入日志，也不会把 key 放进返回值。

### 示例配置

```toml
[model.text]
provider = "deepseek"
adapter = "deepseek"
model = "deepseek-v4-flash"
api_base = "https://api.deepseek.com/v1"
api_key_env = "DEEPSEEK_API_KEY"
extra_body_json = '''{"thinking":{"type":"enabled"}}'''
```

```bash
export DEEPSEEK_API_KEY="..."
morph
```

### 自定义 API 地址

测试或代理场景可以设置：

```bash
export DEEPSEEK_API_BASE="http://127.0.0.1:8765"
```

默认值是 `https://api.deepseek.com`。这里应只填 host，**不要带 `/v1`**；
扩展会自己拼接 `/v1/tools/search` 或 `/v1/tools/fetch`。

## 计费与限制

- 调用的是独立 Search / Fetch API，按成功请求计费（5 元 / 1000 次，
  以 DeepSeek 官方计费说明为准）。
- `count` 只控制返回条数，不影响单次请求价格。
- 失败、超时或无结果通常不计费；以官方说明为准。
- 官方限速参考：Search 100 QPS，Fetch 25 QPS（账号级，可能变化）。
- 扩展自身限制：连接超时 15 秒，总超时 60 秒，最多 3 次重定向，
  响应上限 8 MiB。

## 安全

- API key 只用于 `Authorization: Bearer ...` 请求头。
- key 中的 CR/LF 会被拒绝，避免 HTTP 头注入。
- 请求失败时只返回状态码和响应正文，不返回 key。
- 扩展声明 `sandbox_capabilities = ["network"]`，只申请联网能力。
- `manifest.toml` 限定 `fronts = ["cli"]`；其他 front 需要自行调整。

## 测试

本地 mock 测试会启动一个临时 HTTP 服务，不需要真实 key，也不产生费用：

```bash
python3 test_deepseek_web.py
```

覆盖：

- `DEEPSEEK_API_KEY` 环境变量回退
- 配置 `api_key_env`
- 配置内联 `api_key`
- 只写 `provider = "deepseek"` 的隐式 adapter
- `fetch` 请求体与路径
- 非 DeepSeek adapter：返回“不支持”，且 mock 服务没有收到请求
- 缺少 key、缺少配置的错误路径

真实调用一次 Search API：

```bash
python3 test_deepseek_web.py --real
```

这会使用当前配置和真实 key，并产生一次 Search API 费用。

## 故障排查

| 返回/现象 | 原因 | 处理 |
| --- | --- | --- |
| `当前模型不支持 deepseek_web` | `[model.text].adapter` 不是 `deepseek` | 改用 DeepSeek adapter，或把 `adapter` 设为 `"deepseek"` |
| `无法读取当前 Morph 配置` | 默认配置不存在，或 `-c` 路径无法读取 | 设置 `MORPH_CONFIG`，或确认启动参数 |
| `config uses api_key_env=... but ... not set` | 配置引用的环境变量未导出 | 在启动 Morph 前 `export` 对应变量 |
| `no DeepSeek API key found` | 配置没有 `api_key`/`api_key_env`，也没有 `DEEPSEEK_API_KEY` | 补 key |
| `DeepSeek API returned HTTP 402` | 余额不足 | 充值 |
| `DeepSeek API returned HTTP 429` | 超过 QPS | 退避后重试 |
| `DeepSeek request failed: ...` | 网络、DNS、TLS 或超时 | 检查网络与代理 |
| `/ext list` 看不到扩展 | manifest 或 `.so` 未加载 | 检查文件名、权限、manifest 和 Morph 启动日志 |

## 目录结构

```text
deepseek-web/
├── README.md
├── manifest.toml          # 工具名、schema、权限、入口
├── Makefile               # 构建 deepseek_web.so
├── deepseek_web.c         # 扩展实现
├── cJSON.c / cJSON.h      # 内置 JSON 解析
└── test_deepseek_web.py   # mock 与真实调用测试
```

安装后：

```text
~/.morph/exts/deepseek-web/
├── manifest.toml
└── deepseek_web.so
```

## 参考

- [DeepSeek 网页搜索与抓取](https://api-docs.deepseek.com/zh-cn/guides/search/)
- [DeepSeek Search API](https://api-docs.deepseek.com/zh-cn/api/search)
- [DeepSeek Fetch API](https://api-docs.deepseek.com/zh-cn/api/fetch)
- Morph 扩展手册：安装包或源码中的 `docs/morph.txt`，`*morph-exts*` 章节
