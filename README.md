# deepseek-web

Morph 原生扩展（`type = "so"`），把 DeepSeek
[Search / Fetch API](https://api-docs.deepseek.com/zh-cn/guides/search/)
注册成工具 `deepseek_web`。

## 安装

依赖：C 编译器、`make`、`curl`、`tar`、`cmake` ≥ 3.20、libcurl。
nbase 由 Makefile 自动下载并静态链接，无需预装。

推荐用 Morph 自带安装器（仅支持 GitHub）：

```text
/ext install github:oxUnd/deepseek-web@main --yes
```

它会拉取仓库、执行 `make`，把 `manifest.toml` 与 `deepseek_web.so`
安装到 `~/.morph/exts/deepseek_web`。

手动安装：

```bash
make
mkdir -p ~/.morph/exts/deepseek-web
cp manifest.toml deepseek_web.so ~/.morph/exts/deepseek-web/
```

两种方式二选一，不要同时保留 `deepseek-web` 与 `deepseek_web` 两个目录。
安装后重启 Morph（`/quit` 再 `morph`），用 `/ext list` 确认
`deepseek_web` 已加载。卸载即删除对应目录。

API key 依次取自 `[model.text]` 的 `api_key` / `api_key_env`、
`[model.vision]` 的同名字段，最后回退 `DEEPSEEK_API_KEY`。

## 错误处理

失败时返回 `{"error": "..."}`，不会触发 HTTP 请求的错误不会计费。

| 返回 | 原因 | 处理 |
| --- | --- | --- |
| `当前模型不支持 deepseek_web` | `[model.text].adapter` 不是 `deepseek`，且 `provider` 也不是 `deepseek` | 改用 DeepSeek adapter，或显式设置 `adapter = "deepseek"` |
| `无法读取当前 Morph 配置（或解析失败）` | 默认配置不存在，或 `-c` / `MORPH_CONFIG` 指向的路径不可读 | 设置 `MORPH_CONFIG`，或修正启动参数 |
| `config uses api_key_env=... but that environment variable is not set` | `api_key_env` 引用的变量未导出 | 启动 Morph 前 `export` 该变量 |
| `no DeepSeek API key found` | 配置中没有 key，环境也没有 `DEEPSEEK_API_KEY` | 补 `api_key` / `api_key_env`，或导出 `DEEPSEEK_API_KEY` |
| `invalid JSON arguments` | 工具参数不是 JSON 对象 | 检查调用方生成的参数 |
| `action must be "search" or "fetch"` | `action` 取值非法 | 传 `search` 或 `fetch` |
| `search requires a non-empty query` | 缺少 `query` | 补充搜索词 |
| `fetch requires a non-empty url` | 缺少 `url` | 补充 HTTP/HTTPS 地址 |
| `API key contains a line break` / `API key is too long` | key 含 CR/LF 或超出长度 | 修正配置中的 key |
| `DeepSeek request failed: ...` | 网络、DNS、TLS 或超时 | 检查网络与代理后重试 |
| `DeepSeek API returned HTTP 4xx/5xx: ...` | 服务端返回非 200，正文最多带 2000 字节 | 按下方状态码处理 |
| `DeepSeek returned an empty response` | 服务端返回空正文 | 重试；持续出现则检查服务状态 |
| `DeepSeek response exceeds the 8 MiB limit` | 响应超过上限 | 缩小抓取范围或改抓其他页面 |

HTTP 状态码：

| 状态码 | 处理 |
| --- | --- |
| 400 / 401 | 请求或 key 有问题，修正后再试 |
| 402 | 余额不足，充值后重试 |
| 403 / 404 | Fetch 被安全策略拒绝或页面不可抓取，勿原样重试 |
| 408 / 429 / 5xx | 退避后重试 |

请求参数 `count` 会被夹到 1–50；连接超时 15 秒、总超时 60 秒、
最多 3 次重定向。
