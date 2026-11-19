#!/usr/bin/env python3
"""Exercise deepseek_web through its ext_run_v2 entrypoint."""

import argparse
import ctypes
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

HARNESS_ENV = (
    "HARNESS_TEXT_ADAPTER",
    "HARNESS_TEXT_PROVIDER",
    "HARNESS_TEXT_API_KEY",
    "HARNESS_TEXT_API_KEY_ENV",
    "HARNESS_VISION_API_KEY",
    "HARNESS_VISION_API_KEY_ENV",
    "HARNESS_HOST_MODE",
)
CLEARED_ENV = HARNESS_ENV + (
    "DEEPSEEK_API_KEY",
    "DEEPSEEK_API_BASE",
    "TEST_DEEPSEEK_KEY",
)


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
    lib.harness_run.argtypes = [ctypes.c_char_p,
                                ctypes.POINTER(ctypes.c_char_p)]
    lib.harness_run.restype = ctypes.c_int
    return lib


def run_raw(lib, text):
    output = ctypes.c_char_p()
    rc = lib.harness_run(text.encode(), ctypes.byref(output))
    body = output.value.decode() if output.value else ""
    ctypes.CDLL(None).free(output)
    return rc, body


def run(lib, args):
    return run_raw(lib, json.dumps(args))


def call(lib, args, env):
    old = dict(os.environ)
    try:
        for key in CLEARED_ENV:
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
    parser.add_argument("--lib", default="./deepseek_test.so")
    parser.add_argument("--query", default="DeepSeek API documentation")
    opts = parser.parse_args()

    lib = load_extension(opts.lib)

    if opts.real:
        os.environ["HARNESS_TEXT_ADAPTER"] = "deepseek"
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

    cases = [
        (
            "env fallback",
            {
                "DEEPSEEK_API_BASE": base,
                "HARNESS_TEXT_ADAPTER": "deepseek",
                "DEEPSEEK_API_KEY": "test-key",
            },
            "test-key",
        ),
        (
            "config api_key_env",
            {
                "DEEPSEEK_API_BASE": base,
                "HARNESS_TEXT_ADAPTER": "deepseek",
                "HARNESS_TEXT_API_KEY_ENV": "TEST_DEEPSEEK_KEY",
                "TEST_DEEPSEEK_KEY": "env-key",
            },
            "env-key",
        ),
        (
            "config inline api_key",
            {
                "DEEPSEEK_API_BASE": base,
                "HARNESS_TEXT_ADAPTER": "deepseek",
                "HARNESS_TEXT_API_KEY": "inline-key",
            },
            "inline-key",
        ),
        (
            "provider deepseek implies adapter",
            {
                "DEEPSEEK_API_BASE": base,
                "HARNESS_TEXT_PROVIDER": "deepseek",
                "HARNESS_TEXT_API_KEY": "inline-key",
            },
            "inline-key",
        ),
        (
            "vision key fallback",
            {
                "DEEPSEEK_API_BASE": base,
                "HARNESS_TEXT_ADAPTER": "deepseek",
                "HARNESS_VISION_API_KEY": "vision-key",
            },
            "vision-key",
        ),
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

    for mode in ("old-host", "missing-callback"):
        rc, text = call(lib, args, {"HARNESS_HOST_MODE": mode})
        print(f"[{mode}] rc={rc} response={text[:160]}")
        check(rc < 0 and "get_config_string support" in text,
              f"{mode}: expected an explicit host compatibility error")
        check(MockHandler.seen == {}, f"{mode}: must not make an HTTP request")

    rc, text = call(
        lib,
        {"action": "fetch", "url": "https://example.com/article"},
        {
            "DEEPSEEK_API_BASE": base,
            "HARNESS_TEXT_ADAPTER": "deepseek",
            "HARNESS_TEXT_API_KEY": "inline-key",
        },
    )
    print(f"[fetch] rc={rc} path={MockHandler.seen.get('path')}")
    check(rc == 0 and MockHandler.seen.get("path") == "/v1/tools/fetch",
          "fetch: wrong path")
    check(json.loads(MockHandler.seen.get("body", "{}")).get("url") ==
          "https://example.com/article", "fetch: wrong body")

    rc, text = call(
        lib,
        args,
        {
            "DEEPSEEK_API_BASE": base,
            "HARNESS_TEXT_ADAPTER": "openai-chat-compatible",
            "HARNESS_TEXT_API_KEY": "inline-key",
        },
    )
    print(f"[unsupported adapter] rc={rc} response={text[:160]}")
    check(rc == 0 and "当前模型不支持" in text,
          "unsupported adapter: expected a refusal")
    check(MockHandler.seen == {},
          "unsupported adapter: must not make an HTTP request")

    rc, text = call(
        lib,
        args,
        {
            "DEEPSEEK_API_BASE": base,
            "HARNESS_TEXT_ADAPTER": "deepseek",
        },
    )
    print(f"[missing key] rc={rc} response={text[:160]}")
    check(rc == 0 and "no DeepSeek API key found" in text,
          "missing key: expected a key error")
    check(MockHandler.seen == {}, "missing key: must not make an HTTP request")

    rc, text = call(lib, {"action": "bogus", "query": "x"}, {})
    print(f"[invalid action] rc={rc} response={text[:160]}")
    check(rc == 0 and "action must be" in text,
          "invalid action: expected a validation error")

    rc, text = call(lib, {"action": "fetch"}, {})
    print(f"[missing url] rc={rc} response={text[:160]}")
    check(rc == 0 and "fetch requires" in text,
          "missing url: expected a validation error")

    rc, text = call(lib, {"action": "search"}, {})
    print(f"[missing query] rc={rc} response={text[:160]}")
    check(rc == 0 and "search requires" in text,
          "missing query: expected a validation error")

    rc, text = run_raw(lib, "not json")
    print(f"[invalid json] rc={rc} response={text[:160]}")
    check(rc == 0 and "invalid JSON arguments" in text,
          "invalid json: expected a parse error")

    server.shutdown()
    print("all mock checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
