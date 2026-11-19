# deepseek-web

Morph 原生扩展（`type = "so"`），把 DeepSeek [Search / Fetch API](https://api-docs.deepseek.com/zh-cn/guides/search/) 注册成工具 `deepseek_web`。

## 安装

- `gcc`、`make`、`curl`、`tar`、`cmake` ≥ 3.20、libcurl。

```text
/ext install github:oxUnd/deepseek-web@main --yes
```
安装到 `~/.morph/exts/deepseek_web`。

另一种方法

```bash
make
mkdir -p ~/.morph/exts/deepseek-web
cp manifest.toml deepseek_web.so ~/.morph/exts/deepseek-web/
```
