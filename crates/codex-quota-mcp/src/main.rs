use quota_core::{get_quota_snapshot, read_state, state_path, summary_text, write_state};
use serde_json::{json, Value};
use std::io::{self, BufRead, Write};

const PROTOCOL_VERSION: &str = "2025-06-18";

fn main() {
    let stdin = io::stdin();
    for line in stdin.lock().lines() {
        let line = match line {
            Ok(line) => line,
            Err(error) => {
                eprintln!("failed to read MCP request: {error}");
                break;
            }
        };

        if line.trim().is_empty() {
            continue;
        }

        let request: Value = match serde_json::from_str(&line) {
            Ok(request) => request,
            Err(error) => {
                eprintln!("invalid MCP JSON-RPC message: {error}");
                continue;
            }
        };

        if let Some(response) = handle_request(request) {
            println!("{response}");
            let _ = io::stdout().flush();
        }
    }
}

fn handle_request(request: Value) -> Option<Value> {
    let id = request.get("id").cloned();
    let method = request.get("method").and_then(Value::as_str).unwrap_or_default();

    match method {
        "initialize" => respond(
            id,
            json!({
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "codex-quota", "version": env!("CARGO_PKG_VERSION")}
            }),
        ),
        "notifications/initialized" => None,
        "ping" => respond(id, json!({})),
        "tools/list" => respond(id, tools_list()),
        "tools/call" => respond(id, call_tool(request.get("params").cloned().unwrap_or_default())),
        _ if id.is_some() => Some(json!({
            "jsonrpc": "2.0",
            "id": id,
            "error": {"code": -32601, "message": format!("unknown method: {method}")}
        })),
        _ => None,
    }
}

fn respond(id: Option<Value>, result: Value) -> Option<Value> {
    Some(json!({
        "jsonrpc": "2.0",
        "id": id,
        "result": result
    }))
}

fn tools_list() -> Value {
    let empty_schema = json!({"type": "object", "properties": {}, "additionalProperties": false});
    json!({
        "tools": [
            {
                "name": "codex_quota_get",
                "description": "Return the current Codex quota snapshot as structured JSON. Reads the watcher cache when available.",
                "inputSchema": empty_schema
            },
            {
                "name": "codex_quota_summary",
                "description": "Return a Chinese summary of Codex five-hour quota, weekly quota, usage, remaining percentages, and reset times.",
                "inputSchema": empty_schema
            },
            {
                "name": "codex_quota_refresh",
                "description": "Refresh Codex quota from ChatGPT immediately, update the cache, and return a Chinese summary.",
                "inputSchema": empty_schema
            }
        ]
    })
}

fn call_tool(params: Value) -> Value {
    let Some(name) = params.get("name").and_then(Value::as_str) else {
        return tool_error("missing tool name");
    };

    match name {
        "codex_quota_get" => {
            let snapshot = cached_or_refresh();
            json!({
                "content": [{"type": "text", "text": serde_json::to_string_pretty(&snapshot).unwrap_or_default()}],
                "structuredContent": {"snapshot": snapshot},
                "isError": false
            })
        }
        "codex_quota_summary" => {
            let snapshot = cached_or_refresh();
            json!({
                "content": [{"type": "text", "text": summary_text(&snapshot)}],
                "structuredContent": {"snapshot": snapshot},
                "isError": false
            })
        }
        "codex_quota_refresh" => {
            let snapshot = get_quota_snapshot();
            let _ = write_state(state_path(), &snapshot);
            json!({
                "content": [{"type": "text", "text": summary_text(&snapshot)}],
                "structuredContent": {"snapshot": snapshot},
                "isError": false
            })
        }
        _ => tool_error(&format!("unknown tool: {name}")),
    }
}

fn cached_or_refresh() -> quota_core::QuotaSnapshot {
    match read_state(state_path()) {
        Ok(Some(snapshot)) => snapshot,
        _ => {
            let snapshot = get_quota_snapshot();
            let _ = write_state(state_path(), &snapshot);
            snapshot
        }
    }
}

fn tool_error(message: &str) -> Value {
    json!({
        "content": [{"type": "text", "text": message}],
        "isError": true
    })
}

