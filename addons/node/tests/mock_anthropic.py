#!/usr/bin/env python3
"""A scripted Anthropic Messages API for the tests: `mock_anthropic.py PORT`.

The answer follows the last user message:
  run COMMAND           a Bash tool call with COMMAND; its result comes back as the answer
  tool NAME {json}      a call of tool NAME with that input; likewise
  tools [[NAME, {json}], ...]   those calls one after another, then the last result as the answer
  anything else         "Hello from the mock model! You said: ..."
Streams when the request asks for it (Claude Code does). GET /__requests lists the requests so
far (method, path, the x-api-key and anthropic-version headers, the last user text)."""
import json
import re
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

requests_seen = []
lock = threading.Lock()
counter = [0]


def next_id():
    with lock:
        counter[0] += 1
        return counter[0]


def events_text(model, text):
    mid = f"msg_mock_{next_id()}"
    yield "message_start", {"message": {"id": mid, "type": "message", "role": "assistant", "model": model,
                                        "content": [], "stop_reason": None, "stop_sequence": None,
                                        "usage": {"input_tokens": 10, "output_tokens": 1,
                                                  "cache_creation_input_tokens": 0, "cache_read_input_tokens": 0}}}
    yield "content_block_start", {"index": 0, "content_block": {"type": "text", "text": ""}}
    for i in range(0, len(text), 8):
        yield "content_block_delta", {"index": 0, "delta": {"type": "text_delta", "text": text[i:i + 8]}}
    yield "content_block_stop", {"index": 0}
    yield "message_delta", {"delta": {"stop_reason": "end_turn", "stop_sequence": None}, "usage": {"output_tokens": 12}}
    yield "message_stop", {}


def events_tool(model, name, tool_input):
    n = next_id()
    payload = json.dumps(tool_input)
    yield "message_start", {"message": {"id": f"msg_mock_{n}", "type": "message", "role": "assistant", "model": model,
                                        "content": [], "stop_reason": None, "stop_sequence": None,
                                        "usage": {"input_tokens": 10, "output_tokens": 1}}}
    yield "content_block_start", {"index": 0, "content_block": {"type": "tool_use", "id": f"toolu_mock_{n}",
                                                                "name": name, "input": {}}}
    yield "content_block_delta", {"index": 0, "delta": {"type": "input_json_delta", "partial_json": payload[:10]}}
    yield "content_block_delta", {"index": 0, "delta": {"type": "input_json_delta", "partial_json": payload[10:]}}
    yield "content_block_stop", {"index": 0}
    yield "message_delta", {"delta": {"stop_reason": "tool_use", "stop_sequence": None}, "usage": {"output_tokens": 20}}
    yield "message_stop", {}


def result_text(content):
    if isinstance(content, list):
        return "".join(part.get("text", "") for part in content if isinstance(part, dict))
    return str(content)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send_json(self, status, obj):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/__requests":
            with lock:
                return self.send_json(200, requests_seen)
        self.send_json(404, {"type": "error", "error": {"type": "not_found_error", "message": "mock: not found"}})

    def do_POST(self):
        length = int(self.headers.get("content-length") or 0)
        raw = self.rfile.read(length) if length else b""
        try:
            body = json.loads(raw or b"{}")
        except ValueError:
            body = {}
        messages = body.get("messages") or []
        last = messages[-1] if messages else {}
        content = last.get("content")
        parts = content if isinstance(content, list) else [{"type": "text", "text": str(content or "")}]
        tool_result = next((p for p in parts if p.get("type") == "tool_result"), None)
        text = " ".join(p.get("text", "") for p in parts
                        if p.get("type") == "text" and not p.get("text", "").startswith("<system-reminder>")).strip()
        with lock:
            requests_seen.append({"method": "POST", "path": self.path, "x-api-key": self.headers.get("x-api-key"),
                                  "anthropic-version": self.headers.get("anthropic-version"), "text": text,
                                  "tool_result": result_text(tool_result.get("content")) if tool_result else None})
        if self.path.startswith("/v1/messages/count_tokens"):
            return self.send_json(200, {"input_tokens": 42})
        if not self.path.startswith("/v1/messages"):
            return self.send_json(404, {"type": "error", "error": {"type": "not_found_error", "message": "mock: not found"}})
        model = body.get("model", "mock-model")
        if not body.get("stream"):
            return self.send_json(200, {"id": f"msg_mock_{next_id()}", "type": "message", "role": "assistant",
                                        "model": model, "content": [{"type": "text", "text": "mock title"}],
                                        "stop_reason": "end_turn", "usage": {"input_tokens": 5, "output_tokens": 2}})
        first = next((m for m in messages if m.get("role") == "user"), {})
        first_parts = first.get("content") if isinstance(first.get("content"), list) else \
            [{"type": "text", "text": str(first.get("content") or "")}]
        prompt = " ".join(p.get("text", "") for p in first_parts if p.get("type") == "text"
                          and not p.get("text", "").startswith("<system-reminder>")).strip()
        done = sum(1 for m in messages if m.get("role") == "user" and isinstance(m.get("content"), list)
                   for p in m["content"] if p.get("type") == "tool_result")
        steps = json.loads(prompt[6:]) if prompt.startswith("tools [") else None
        if steps and done < len(steps):
            events = events_tool(model, steps[done][0], steps[done][1])
        elif tool_result:
            events = events_text(model, "The command printed: " + json.dumps(result_text(tool_result.get("content")))[:400])
        elif m := re.match(r"^tool (\w+) (.*)$", text, re.S):
            events = events_tool(model, m.group(1), json.loads(m.group(2)))
        elif m := re.match(r"^run (.*)$", text, re.S):
            events = events_tool(model, "Bash", {"command": m.group(1), "description": "Run the requested command"})
        else:
            events = events_text(model, "Hello from the mock model! You said: " + text[:80])
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("cache-control", "no-cache")
        self.send_header("request-id", "req_mock")
        self.send_header("connection", "close")
        self.end_headers()
        for name, data in events:
            self.wfile.write(f"event: {name}\ndata: {json.dumps({'type': name, **data})}\n\n".encode())
            self.wfile.flush()
        self.close_connection = True


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18094
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"mock Anthropic API on 127.0.0.1:{server.server_address[1]}", flush=True)
    server.serve_forever()
