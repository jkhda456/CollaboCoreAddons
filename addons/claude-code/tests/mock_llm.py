"""A scripted model behind the host request API's wire protocol (COLLABO_BRIDGE=tcp:127.0.0.1:PORT).

POST <scenario> {"turns": [...]} to /__scenario to load a script; each model request takes the next
turn: {"text": "...", "thinking": "...", "tools": [{"name", "input"}], "delay": s, "slow": s-per-piece,
"usage": {"input_tokens": n}}. Both /v1/messages (Anthropic SSE) and /chat/completions (OpenAI SSE)
are answered. Requests are appended to requests.jsonl; /__requests returns them.
"""
import json, socketserver, sys, os, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.environ.get("CLAUDE_TEST_LOG", os.path.join(HERE, "llm-requests.jsonl"))
state = {"turns": [], "i": 0}
lock = threading.Lock()

def chunk(b): return f"{len(b):x}\n".encode() + b

def anthropic_events(turn, idx):
    ev = []
    usage = {"input_tokens": 100, **turn.get("usage", {})}
    ev.append({"type": "message_start", "message": {"usage": usage}})
    i = 0
    if turn.get("thinking"):
        ev.append({"type": "content_block_start", "index": i, "content_block": {"type": "thinking", "thinking": ""}})
        ev.append({"type": "content_block_delta", "index": i, "delta": {"type": "thinking_delta", "thinking": turn["thinking"]}})
        ev.append({"type": "content_block_delta", "index": i, "delta": {"type": "signature_delta", "signature": "sig"}})
        ev.append({"type": "content_block_stop", "index": i}); i += 1
    if turn.get("search"):
        ev.append({"type": "content_block_start", "index": i, "content_block": {"type": "server_tool_use", "id": "srvtoolu_1", "name": "web_search", "input": {}}})
        ev.append({"type": "content_block_delta", "index": i, "delta": {"type": "input_json_delta", "partial_json": json.dumps({"query": turn["search"]})}})
        ev.append({"type": "content_block_stop", "index": i}); i += 1
        ev.append({"type": "content_block_start", "index": i, "content_block": {"type": "web_search_tool_result", "tool_use_id": "srvtoolu_1",
                   "content": [{"type": "web_search_result", "url": "https://example.com", "title": "Example", "encrypted_content": "abc"}]}})
        ev.append({"type": "content_block_stop", "index": i}); i += 1
    if turn.get("text"):
        ev.append({"type": "content_block_start", "index": i, "content_block": {"type": "text", "text": ""}})
        text = turn["text"]
        for k in range(0, len(text), 7):
            ev.append({"type": "content_block_delta", "index": i, "delta": {"type": "text_delta", "text": text[k:k+7]}})
        ev.append({"type": "content_block_stop", "index": i}); i += 1
    for n, tool in enumerate(turn.get("tools", [])):
        ev.append({"type": "content_block_start", "index": i, "content_block": {"type": "tool_use", "id": f"tu_{idx}_{n}", "name": tool["name"], "input": {}}})
        raw = json.dumps(tool["input"])
        for k in range(0, len(raw), 10):
            ev.append({"type": "content_block_delta", "index": i, "delta": {"type": "input_json_delta", "partial_json": raw[k:k+10]}})
        ev.append({"type": "content_block_stop", "index": i}); i += 1
    ev.append({"type": "message_delta", "delta": {"stop_reason": "tool_use" if turn.get("tools") else "end_turn"}, "usage": {"output_tokens": 20}})
    ev.append({"type": "message_stop"})
    return [f"event: {e['type']}\ndata: {json.dumps(e)}\n\n" for e in ev]

def openai_events(turn, idx):
    ev = []
    if turn.get("text"):
        t = turn["text"]
        for k in range(0, len(t), 7):
            ev.append({"choices": [{"delta": {"content": t[k:k+7]}}]})
    for n, tool in enumerate(turn.get("tools", [])):
        ev.append({"choices": [{"delta": {"tool_calls": [{"index": n, "id": f"call_{idx}_{n}", "function": {"name": tool["name"], "arguments": json.dumps(tool["input"])}}]}}]})
    ev.append({"choices": [{"delta": {}, "finish_reason": "tool_calls" if turn.get("tools") else "stop"}], "usage": {"prompt_tokens": 100, "completion_tokens": 20}})
    return [f"data: {json.dumps(e)}\n\n" for e in ev] + ["data: [DONE]\n\n"]

class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        line = self.rfile.readline().decode().rstrip("\n")
        if not line: return
        method, url = line.split(" ", 1)
        headers = {}
        while True:
            h = self.rfile.readline().decode().rstrip("\n")
            if not h: break
            k, v = h.split(":", 1); headers[k.strip().lower()] = v.strip()
        body = self.rfile.read(int(headers.get("content-length", 0)))
        req = json.loads(body) if body else None
        if url.endswith("/__scenario"):
            with lock: state["turns"] = req["turns"]; state["i"] = 0
            open(LOG, "w").close()
            self.wfile.write(b"200 OK\n\n" + chunk(b"ok") + b"0\nOK\n"); return
        if url.endswith("/mcp"):
            # A streamable-HTTP MCP server: a session id on initialize, SSE for tools/call.
            if req.get("id") is None:
                self.wfile.write(b"202 Accepted\n\n0\nOK\n"); return
            method = req["method"]
            session = headers.get("mcp-session-id", "")
            if method == "initialize":
                result = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}}, "serverInfo": {"name": "web-mcp"}}
                body = json.dumps({"jsonrpc": "2.0", "id": req["id"], "result": result}).encode()
                self.wfile.write(b"200 OK\ncontent-type: application/json\nmcp-session-id: sess-42\n\n" + chunk(body) + b"0\nOK\n"); return
            if session != "sess-42":
                self.wfile.write(b"400 Bad Request\n\n" + chunk(b"missing session") + b"0\nOK\n"); return
            if method == "tools/list":
                result = {"tools": [{"name": "lookup", "description": "Look up", "inputSchema": {"type": "object", "properties": {"q": {"type": "string"}}}}]}
                body = json.dumps({"jsonrpc": "2.0", "id": req["id"], "result": result}).encode()
                self.wfile.write(b"200 OK\ncontent-type: application/json\n\n" + chunk(body) + b"0\nOK\n"); return
            text = "looked up " + req["params"]["arguments"].get("q", "")
            events = f'event: message\ndata: {json.dumps({"jsonrpc": "2.0", "method": "notifications/progress", "params": {}})}\n\n' + \
                     f'event: message\ndata: {json.dumps({"jsonrpc": "2.0", "id": req["id"], "result": {"content": [{"type": "text", "text": text}]}})}\n\n'
            self.wfile.write(b"200 OK\ncontent-type: text/event-stream\n\n" + chunk(events.encode()) + b"0\nOK\n"); return
        if url.endswith("/models"):
            self.wfile.write(b"200 OK\ncontent-type: application/json\n\n" + chunk(json.dumps({"data": [{"id": "mock-model"}]}).encode()) + b"0\nOK\n"); return
        if "/messages" in url or "/chat/completions" in url:
            with open(LOG, "a") as f: f.write(json.dumps({"url": url, "headers": headers, "body": req}) + "\n")
            with lock:
                idx = state["i"]; state["i"] += 1
                turn = state["turns"][idx] if idx < len(state["turns"]) else {"text": "(no more scripted turns)"}
            time.sleep(turn.get("delay", 0))
            if not req.get("stream"):
                content = []
                if turn.get("text"): content.append({"type": "text", "text": turn["text"]})
                for n, tool in enumerate(turn.get("tools", [])):
                    content.append({"type": "tool_use", "id": f"tu_{idx}_{n}", "name": tool["name"], "input": tool["input"]})
                payload = json.dumps({"content": content, "stop_reason": "tool_use" if turn.get("tools") else "end_turn", "usage": {"input_tokens": 100, "output_tokens": 20}})
                self.wfile.write(b"200 OK\ncontent-type: application/json\n\n" + chunk(payload.encode()) + b"0\nOK\n"); return
            parts = anthropic_events(turn, idx) if "/messages" in url else openai_events(turn, idx)
            self.wfile.write(b"200 OK\ncontent-type: text/event-stream\n\n")
            try:
                for p in parts:
                    self.wfile.write(chunk(p.encode())); self.wfile.flush()
                    if turn.get("slow"): time.sleep(turn["slow"])
                self.wfile.write(b"0\nOK\n")
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        self.wfile.write(b"200 OK\ncontent-type: text/html\n\n" + chunk(b"<html><body><h1>Page</h1><p>fetched text</p></body></html>") + b"0\nOK\n")

class HttpHandler(socketserver.StreamRequestHandler):
    """The same model over plain HTTP/1.1 (what the engine's host side calls from the guest)."""
    def handle(self):
        line = self.rfile.readline().decode().rstrip("\r\n")
        if not line: return
        method, path, _ = line.split(" ", 2)
        headers = {}
        while True:
            h = self.rfile.readline().decode().rstrip("\r\n")
            if not h: break
            k, v = h.split(":", 1); headers[k.strip().lower()] = v.strip()
        body = self.rfile.read(int(headers.get("content-length", 0)))
        # Reuse the bridge handler by speaking its protocol to it in memory.
        import io
        request = f"{method} http://h{path}\n".encode() + b"".join(f"{k}: {v}\n".encode() for k, v in headers.items() if k != "host") + b"\n" + body
        out = io.BytesIO()
        fake = type("F", (), {})()
        h = Handler.__new__(Handler)
        h.rfile = io.BytesIO(request); h.wfile = _Tee(self.wfile)
        Handler.handle(h)

class _Tee:
    """Turns the bridge framing Handler writes into an HTTP/1.1 chunked response."""
    def __init__(self, w): self.w = w; self.buf = b""; self.head = False
    def flush(self): self.w.flush()
    def write(self, data):
        self.buf += data
        if not self.head:
            if b"\n\n" not in self.buf: return
            head, self.buf = self.buf.split(b"\n\n", 1)
            lines = head.decode().split("\n")
            status = lines[0]
            out = f"HTTP/1.1 {status}\r\n" + "".join(f"{l}\r\n" for l in lines[1:]) + "transfer-encoding: chunked\r\nconnection: close\r\n\r\n"
            self.w.write(out.encode()); self.head = True
        while True:
            if b"\n" not in self.buf: return
            size_line, rest = self.buf.split(b"\n", 1)
            if size_line in (b"OK", b""):
                self.buf = rest; continue
            n = int(size_line, 16)
            if n == 0:
                self.w.write(b"0\r\n\r\n"); self.w.flush(); self.buf = rest; continue
            if len(rest) < n: return
            self.w.write(f"{n:x}\r\n".encode() + rest[:n] + b"\r\n"); self.w.flush()
            self.buf = rest[n:]

if __name__ == "__main__":
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    socketserver.ThreadingTCPServer.daemon_threads = True
    if len(sys.argv) > 2 and sys.argv[2] == "http":
        with socketserver.ThreadingTCPServer(("127.0.0.1", int(sys.argv[1])), HttpHandler) as s:
            s.serve_forever()
    socketserver.ThreadingTCPServer.daemon_threads = True
    with socketserver.ThreadingTCPServer(("127.0.0.1", int(sys.argv[1])), Handler) as s:
        s.serve_forever()
