#!/usr/bin/env python3
"""Exercise deepseek_web through its ext_run_v2 entrypoint."""

import argparse
import ctypes
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HARNESS_ENV = (
    "HARNESS_TEXT_ADAPTER",
    "HARNESS_TEXT_PROVIDER",
    "HARNESS_TEXT_API_KEY",
    "HARNESS_TEXT_API_KEY_ENV",
    "HARNESS_VISION_API_KEY",
    "HARNESS_VISION_API_KEY_ENV",
    "HARNESS_HOST_MODE",
    "HARNESS_CANCEL_AFTER_MS",
    "HARNESS_DEADLINE_MS",
)
CLEARED_ENV = HARNESS_ENV + (
    "DEEPSEEK_API_KEY",
    "DEEPSEEK_API_BASE",
    "TEST_DEEPSEEK_KEY",
)


class MockHandler(BaseHTTPRequestHandler):
    seen = {}
    mode = "normal"
    cross_origin_base = ""

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        MockHandler.seen = {
            "path": self.path,
            "authorization": self.headers.get("Authorization"),
            "content_type": self.headers.get("Content-Type"),
            "body": body.decode("utf-8"),
        }
        mode = MockHandler.mode
        if mode == "slow":
            time.sleep(1)
        if mode == "redirect" and self.path != "/redirected":
            self.send_response(307)
            self.send_header("Location", "/redirected")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if mode == "cross-origin":
            self.send_response(307)
            self.send_header("Location", f"{self.cross_origin_base}/other")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
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
        if mode == "empty":
            payload = b""
        elif mode == "oversized":
            payload = b"x" * (8 * 1024 * 1024 + 1)
        elif mode == "http-error":
            payload = b'{"error":"rate limited"}'
        self.send_response(429 if mode == "http-error" else 200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass  # The client may cancel or enforce its response-size limit.

    def log_message(self, *_args):
        pass


class RedirectTarget(BaseHTTPRequestHandler):
    requests = 0

    def do_POST(self):
        RedirectTarget.requests += 1
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *_args):
        pass


def load_extension(path):
    lib = ctypes.CDLL(path)
    lib.harness_run.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p)]
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

    server = ThreadingHTTPServer(("127.0.0.1", 0), MockHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    redirect_target = ThreadingHTTPServer(("127.0.0.1", 0), RedirectTarget)
    redirect_thread = threading.Thread(target=redirect_target.serve_forever, daemon=True)
    redirect_thread.start()
    MockHandler.cross_origin_base = f"http://127.0.0.1:{redirect_target.server_port}"
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
    check(rc == 0 and "deepseek_web requires the DeepSeek adapter" in text,
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

    request_env = {
        "DEEPSEEK_API_BASE": base,
        "HARNESS_TEXT_ADAPTER": "deepseek",
        "HARNESS_TEXT_API_KEY": "inline-key",
    }
    for mode, expected in (
        ("http-error", "HTTP 429: {"),
        ("empty", "empty response"),
        ("oversized", "8 MiB limit"),
        ("cross-origin", "HTTP 307"),
    ):
        MockHandler.mode = mode
        rc, text = call(lib, args, request_env)
        print(f"[{mode}] rc={rc} response={text[:160]}")
        check(rc == 0 and expected in json.loads(text).get("error", ""),
              f"{mode}: expected {expected!r}")
    check(RedirectTarget.requests == 0, "cross-origin redirect: must not forward the request")

    rc, text = call(lib, args, {**request_env, "DEEPSEEK_API_BASE": "file:///unused"})
    check(rc == 0 and "DeepSeek request failed" in json.loads(text).get("error", ""),
          "invalid HTTP URL: expected a transport error")
    check(MockHandler.seen == {}, "invalid HTTP URL: must not send HTTP")
    print("[invalid HTTP URL] passed")

    MockHandler.mode = "redirect"
    rc, text = call(lib, args, request_env)
    check(rc == 0 and "Mock result" in text and
          MockHandler.seen.get("path") == "/redirected" and
          MockHandler.seen.get("authorization") == "Bearer inline-key",
          "same-origin redirect: expected an authenticated redirected POST")
    print("[same-origin redirect] passed")

    MockHandler.mode = "normal"
    for key in ("bad\r\nX-Injected: yes", "x" * 4096):
        rc, text = call(lib, args, {**request_env, "HARNESS_TEXT_API_KEY": key})
        check(rc == 0 and "API key" in json.loads(text).get("error", ""),
              "invalid API key: expected a validation error")
        check(MockHandler.seen == {}, "invalid API key: must not send HTTP")
    print("[invalid API keys] passed")

    for field, value, status, mode in (
        ("HARNESS_CANCEL_AFTER_MS", "0", -14, "normal"),
        ("HARNESS_DEADLINE_MS", "-1", -7, "normal"),
        ("HARNESS_CANCEL_AFTER_MS", "120", -14, "slow"),
        ("HARNESS_DEADLINE_MS", "120", -7, "slow"),
    ):
        MockHandler.mode = mode
        started = time.monotonic()
        rc, text = call(lib, args, {**request_env, field: value})
        elapsed = time.monotonic() - started
        check(rc == status and not text, f"{field}/{mode}: rc={rc}, {text}")
        check(elapsed < 0.8, f"{field}/{mode}: cancellation took {elapsed:.2f}s")
        if mode == "normal":
            check(MockHandler.seen == {}, f"{field}: must not send HTTP")
        print(f"[{field}/{mode}] rc={rc} elapsed={elapsed:.3f}s")

    server.shutdown()
    server.server_close()
    thread.join()
    redirect_target.shutdown()
    redirect_target.server_close()
    redirect_thread.join()
    print("all mock checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
