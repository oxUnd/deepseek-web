#!/usr/bin/env python3
"""Exercise the deepseek_web extension through its public ext_run symbol."""

import argparse
import ctypes
import json
import os
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer


class MockHandler(BaseHTTPRequestHandler):
    seen = {}

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        MockHandler.seen = {
            "path": self.path,
            "authorization": self.headers.get("Authorization"),
            "content_type": self.headers.get("Content-Type"),
            "body": body.decode("utf-8"),
        }
        payload = json.dumps(
            {
                "results": [
                    {
                        "title": "Mock result",
                        "url": "https://example.com/mock",
                        "snippet": "mock snippet",
                    }
                ]
            }
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_args):
        pass


def load_extension(path):
    lib = ctypes.CDLL(path)
    lib.ext_run.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p)]
    lib.ext_run.restype = ctypes.c_int
    return lib


def run(lib, args):
    output = ctypes.c_char_p()
    rc = lib.ext_run(json.dumps(args).encode(), ctypes.byref(output))
    text = output.value.decode() if output.value else ""
    ctypes.CDLL(None).free(output)
    return rc, text


def call(lib, args, env):
    old = dict(os.environ)
    try:
        for key in ("DEEPSEEK_API_KEY", "DEEPSEEK_API_BASE", "MORPH_CONFIG",
                    "MORPH_CONFIG_PATH", "TEST_DEEPSEEK_KEY"):
            os.environ.pop(key, None)
        os.environ.update(env)
        MockHandler.seen = {}
        return run(lib, args)
    finally:
        os.environ.clear()
        os.environ.update(old)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--real", action="store_true", help="call the real API once")
    parser.add_argument("--lib", default="./deepseek_web.so")
    parser.add_argument("--query", default="DeepSeek API documentation")
    opts = parser.parse_args()

    lib = load_extension(opts.lib)

    if opts.real:
        os.environ.pop("DEEPSEEK_API_BASE", None)
        rc, text = run(
            lib,
            {
                "action": "search",
                "query": opts.query,
                "count": 1,
                "need_content": False,
            },
        )
        print("rc =", rc)
        print(text[:2000])
        return 0 if rc == 0 and '"error"' not in text else 1

    server = HTTPServer(("127.0.0.1", 0), MockHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    args = {
        "action": "search",
        "query": "hello world",
        "count": 3,
        "need_content": True,
        "ori_question": "what is hello world",
    }

    with tempfile.TemporaryDirectory() as tmp:
        env_cfg = os.path.join(tmp, "env.toml")
        with open(env_cfg, "w", encoding="utf-8") as f:
            f.write('[model.text]\nadapter = "deepseek"\n'
                    'api_key_env = "TEST_DEEPSEEK_KEY"\n')
        inline_cfg = os.path.join(tmp, "inline.toml")
        with open(inline_cfg, "w", encoding="utf-8") as f:
            f.write('[model.text]\nadapter = "deepseek"\napi_key = "inline-key"\n')
        provider_cfg = os.path.join(tmp, "provider.toml")
        with open(provider_cfg, "w", encoding="utf-8") as f:
            f.write('[model.text]\nprovider = "deepseek"\napi_key = "inline-key"\n')
        adapter_only_cfg = os.path.join(tmp, "adapter-only.toml")
        with open(adapter_only_cfg, "w", encoding="utf-8") as f:
            f.write('[model.text]\nadapter = "deepseek"\n')
        nokey_cfg = os.path.join(tmp, "nokey.toml")
        with open(nokey_cfg, "w", encoding="utf-8") as f:
            f.write('[model.text]\nadapter = "deepseek"\n')
        bad_cfg = os.path.join(tmp, "bad.toml")
        with open(bad_cfg, "w", encoding="utf-8") as f:
            f.write('[model.text]\nadapter = "openai-chat-compatible"\n'
                    'api_key = "inline-key"\n')

        cases = [
            ("env fallback", {"DEEPSEEK_API_BASE": base,
                              "MORPH_CONFIG": adapter_only_cfg,
                              "DEEPSEEK_API_KEY": "test-key"}, "test-key"),
            ("config api_key_env", {"DEEPSEEK_API_BASE": base,
                                    "MORPH_CONFIG": env_cfg,
                                    "TEST_DEEPSEEK_KEY": "env-key"}, "env-key"),
            ("config inline api_key", {"DEEPSEEK_API_BASE": base,
                                       "MORPH_CONFIG": inline_cfg}, "inline-key"),
            ("provider deepseek implies adapter", {
                "DEEPSEEK_API_BASE": base, "MORPH_CONFIG": provider_cfg,
            }, "inline-key"),
        ]
        for name, env, expected in cases:
            rc, text = call(lib, args, env)
            print(f"[{name}] rc={rc} auth={MockHandler.seen.get('authorization')}")
            check(rc == 0, f"{name}: rc={rc}")
            check(MockHandler.seen.get("path") == "/v1/tools/search",
                  f"{name}: wrong path")
            check(MockHandler.seen.get("authorization") == f"Bearer {expected}",
                  f"{name}: wrong authorization")
            check(json.loads(MockHandler.seen.get("body", "{}")).get("count") == 3,
                  f"{name}: wrong count")
            check("Mock result" in text, f"{name}: missing result")

        rc, text = call(
            lib,
            {"action": "fetch", "url": "https://example.com/article"},
            {"DEEPSEEK_API_BASE": base, "MORPH_CONFIG": inline_cfg},
        )
        print(f"[fetch] rc={rc} path={MockHandler.seen.get('path')}")
        check(rc == 0 and MockHandler.seen.get("path") == "/v1/tools/fetch",
              "fetch: wrong path")
        check(json.loads(MockHandler.seen.get("body", "{}")).get("url") ==
              "https://example.com/article", "fetch: wrong body")

        rc, text = call(
            lib,
            args,
            {"DEEPSEEK_API_BASE": base, "MORPH_CONFIG": bad_cfg},
        )
        print(f"[unsupported adapter] rc={rc} response={text[:160]}")
        check(rc == 0 and "当前模型不支持" in text,
              "unsupported adapter: expected a refusal")
        check(MockHandler.seen == {},
              "unsupported adapter: must not make an HTTP request")

        rc, text = call(
            lib,
            args,
            {"DEEPSEEK_API_BASE": base, "MORPH_CONFIG": nokey_cfg},
        )
        print(f"[missing key] rc={rc} response={text[:120]}")
        check(rc == 0 and "no DeepSeek API key found" in text,
              "missing key: expected a key error")

        rc, text = call(
            lib,
            args,
            {"DEEPSEEK_API_BASE": base,
             "MORPH_CONFIG": os.path.join(tmp, "missing.toml")},
        )
        print(f"[missing config] rc={rc} response={text[:120]}")
        check(rc == 0 and "无法读取当前 Morph 配置" in text,
              "missing config: expected a config error")

    server.shutdown()
    print("all mock checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
