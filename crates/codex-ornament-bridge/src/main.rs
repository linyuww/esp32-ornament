use chrono::Local;
use quota_core::{get_quota_snapshot, state_path, write_state, QuotaSnapshot};
use serde::Serialize;
use serde_json::{json, Value};
use std::{
    collections::HashMap,
    env,
    io::{self, BufRead, BufReader, Read, Write},
    net::{IpAddr, SocketAddr, TcpListener, TcpStream},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

const DEFAULT_BIND: &str = "0.0.0.0:8787";
const MAX_BODY_BYTES: usize = 64 * 1024;
const QUOTA_CACHE_TTL: Duration = Duration::from_secs(60);

#[derive(Clone, Debug, Default)]
struct BridgeState {
    task: Option<TaskEvent>,
    quota: Option<CachedQuota>,
}

#[derive(Clone, Debug)]
struct CachedQuota {
    snapshot: QuotaSnapshot,
    fetched_at: Instant,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct BridgeConfig {
    bind: String,
    token: Option<String>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct HttpRequest {
    method: String,
    path: String,
    headers: HashMap<String, String>,
    body: Vec<u8>,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct TaskEvent {
    kind: String,
    status: String,
    title: String,
    message: String,
    received_at: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    session_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    turn_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    cwd: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    model: Option<String>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct OrnamentState {
    status: String,
    task: Option<TaskEvent>,
    quota: QuotaSnapshot,
    bridge: BridgeInfo,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct BridgeInfo {
    service: &'static str,
    observed_at: String,
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

fn run() -> io::Result<()> {
    let config = BridgeConfig {
        bind: env::var("CODEX_ORNAMENT_BIND").unwrap_or_else(|_| DEFAULT_BIND.to_string()),
        token: env::var("CODEX_ORNAMENT_TOKEN")
            .ok()
            .filter(|value| !value.trim().is_empty()),
    };
    let listener = TcpListener::bind(&config.bind)?;
    let state = Arc::new(Mutex::new(BridgeState::default()));

    eprintln!("codex ornament bridge listening on http://{}", config.bind);
    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                let state = Arc::clone(&state);
                let config = config.clone();
                std::thread::spawn(move || {
                    if let Err(error) = handle_connection(stream, state, config) {
                        eprintln!("request failed: {error}");
                    }
                });
            }
            Err(error) => eprintln!("connection failed: {error}"),
        }
    }

    Ok(())
}

fn handle_connection(
    mut stream: TcpStream,
    state: Arc<Mutex<BridgeState>>,
    config: BridgeConfig,
) -> io::Result<()> {
    let peer = stream.peer_addr().ok();
    let request = read_request(&mut stream)?;

    match (request.method.as_str(), request.path.as_str()) {
        ("OPTIONS", _) => write_response(&mut stream, 204, "text/plain; charset=utf-8", b""),
        ("GET", "/health") => {
            write_response(&mut stream, 200, "text/plain; charset=utf-8", b"ok\n")
        }
        ("GET", "/quota") => {
            let quota = cached_or_refresh_quota(&state);
            write_json(&mut stream, 200, &quota)
        }
        ("GET", "/state") => {
            let task = state.lock().ok().and_then(|state| state.task.clone());
            let response = OrnamentState {
                status: task
                    .as_ref()
                    .map(|event| event.status.clone())
                    .unwrap_or_else(|| "idle".to_string()),
                task,
                quota: cached_or_refresh_quota(&state),
                bridge: BridgeInfo {
                    service: "codex-ornament-bridge",
                    observed_at: now_local(),
                },
            };
            write_json(&mut stream, 200, &response)
        }
        ("POST", "/hook/codex") | ("POST", "/event") => {
            if !post_allowed(peer, &request, &config) {
                return write_json(
                    &mut stream,
                    403,
                    &json!({"ok": false, "error": "forbidden"}),
                );
            }

            let payload = serde_json::from_slice::<Value>(&request.body).unwrap_or_else(|_| {
                json!({
                    "hook_event_name": "InvalidJson",
                    "raw": String::from_utf8_lossy(&request.body).to_string()
                })
            });
            let event = normalize_event(&payload);
            if let Ok(mut state) = state.lock() {
                state.task = Some(event.clone());
            }
            write_json(&mut stream, 200, &json!({"ok": true, "event": event}))
        }
        _ => write_json(
            &mut stream,
            404,
            &json!({"ok": false, "error": "not found"}),
        ),
    }
}

fn read_request(stream: &mut TcpStream) -> io::Result<HttpRequest> {
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    let mut reader = BufReader::new(stream.try_clone()?);
    let mut request_line = String::new();
    reader.read_line(&mut request_line)?;
    let mut parts = request_line.split_whitespace();
    let method = parts.next().unwrap_or_default().to_string();
    let raw_path = parts.next().unwrap_or("/").to_string();
    let path = raw_path
        .split_once('?')
        .map(|(path, _)| path.to_string())
        .unwrap_or(raw_path);

    let mut headers = HashMap::new();
    loop {
        let mut line = String::new();
        reader.read_line(&mut line)?;
        let trimmed = line.trim_end_matches(['\r', '\n']);
        if trimmed.is_empty() {
            break;
        }
        if let Some((name, value)) = trimmed.split_once(':') {
            headers.insert(name.trim().to_ascii_lowercase(), value.trim().to_string());
        }
    }

    let content_length = headers
        .get("content-length")
        .and_then(|value| value.parse::<usize>().ok())
        .unwrap_or(0)
        .min(MAX_BODY_BYTES);
    let mut body = vec![0; content_length];
    if content_length > 0 {
        reader.read_exact(&mut body)?;
    }

    Ok(HttpRequest {
        method,
        path,
        headers,
        body,
    })
}

fn post_allowed(peer: Option<SocketAddr>, request: &HttpRequest, config: &BridgeConfig) -> bool {
    if peer.map(|addr| is_loopback(addr.ip())).unwrap_or(false) {
        return true;
    }

    let Some(expected) = config.token.as_deref() else {
        return false;
    };
    request
        .headers
        .get("x-codex-ornament-token")
        .map(|actual| actual == expected)
        .unwrap_or(false)
}

fn is_loopback(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(ip) => ip.is_loopback(),
        IpAddr::V6(ip) => ip.is_loopback(),
    }
}

fn normalize_event(payload: &Value) -> TaskEvent {
    let kind = text_field(payload, &["hook_event_name", "type"])
        .unwrap_or_else(|| "codex-event".to_string());
    let status = match kind.as_str() {
        "UserPromptSubmit" => "running",
        "Stop" | "agent-turn-complete" => "done",
        "InvalidJson" => "error",
        _ => "event",
    }
    .to_string();
    let title = match status.as_str() {
        "running" => "Codex running",
        "done" => "Codex done",
        "error" => "Codex hook error",
        _ => "Codex event",
    }
    .to_string();
    let message = event_message(payload).unwrap_or_else(|| kind.clone());

    TaskEvent {
        kind,
        status,
        title,
        message,
        received_at: now_local(),
        session_id: text_field(payload, &["session_id", "thread-id"]),
        turn_id: text_field(payload, &["turn_id", "turn-id"]),
        cwd: text_field(payload, &["cwd"]).map(|value| clip(&value, 120)),
        model: text_field(payload, &["model"]),
    }
}

fn event_message(payload: &Value) -> Option<String> {
    text_field(
        payload,
        &[
            "last_assistant_message",
            "last-assistant-message",
            "prompt",
            "message",
        ],
    )
    .or_else(|| {
        payload
            .get("input-messages")
            .and_then(Value::as_array)
            .and_then(|messages| messages.last())
            .and_then(Value::as_str)
            .map(str::to_string)
    })
    .map(|value| clip(value.trim(), 160))
    .filter(|value| !value.is_empty())
}

fn text_field(payload: &Value, keys: &[&str]) -> Option<String> {
    keys.iter()
        .filter_map(|key| payload.get(*key))
        .find_map(Value::as_str)
        .map(str::to_string)
        .filter(|value| !value.is_empty())
}

fn cached_or_refresh_quota(state: &Arc<Mutex<BridgeState>>) -> QuotaSnapshot {
    if let Ok(state) = state.lock() {
        if let Some(cache) = state.quota.as_ref() {
            if cache.fetched_at.elapsed() < QUOTA_CACHE_TTL {
                return cache.snapshot.clone();
            }
        }
    }

    let snapshot = get_quota_snapshot();
    let _ = write_state(state_path(), &snapshot);
    if let Ok(mut state) = state.lock() {
        state.quota = Some(CachedQuota {
            snapshot: snapshot.clone(),
            fetched_at: Instant::now(),
        });
    }
    snapshot
}

fn write_json<T: Serialize>(stream: &mut TcpStream, status: u16, value: &T) -> io::Result<()> {
    let body = serde_json::to_vec(value)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;
    write_response(stream, status, "application/json; charset=utf-8", &body)
}

fn write_response(
    stream: &mut TcpStream,
    status: u16,
    content_type: &str,
    body: &[u8],
) -> io::Result<()> {
    let reason = match status {
        200 => "OK",
        204 => "No Content",
        403 => "Forbidden",
        404 => "Not Found",
        _ => "OK",
    };
    write!(
        stream,
        "HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type, X-Codex-Ornament-Token\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nConnection: close\r\n\r\n",
        body.len()
    )?;
    stream.write_all(body)
}

fn now_local() -> String {
    Local::now().to_rfc3339()
}

fn clip(value: &str, max_chars: usize) -> String {
    let mut output = String::new();
    for (index, ch) in value.chars().enumerate() {
        if index >= max_chars {
            output.push_str("...");
            break;
        }
        output.push(ch);
    }
    output
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_config(token: Option<&str>) -> BridgeConfig {
        BridgeConfig {
            bind: "127.0.0.1:8787".to_string(),
            token: token.map(str::to_string),
        }
    }

    #[test]
    fn maps_lifecycle_start_to_running() {
        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "prompt": "build the firmware"
        }));

        assert_eq!(event.kind, "UserPromptSubmit");
        assert_eq!(event.status, "running");
        assert_eq!(event.title, "Codex running");
        assert_eq!(event.message, "build the firmware");
    }

    #[test]
    fn maps_stop_to_done() {
        let event = normalize_event(&json!({
            "hook_event_name": "Stop",
            "session_id": "session-1",
            "cwd": "D:\\Desktop\\codex"
        }));

        assert_eq!(event.status, "done");
        assert_eq!(event.session_id.as_deref(), Some("session-1"));
        assert_eq!(event.cwd.as_deref(), Some("D:\\Desktop\\codex"));
    }

    #[test]
    fn maps_notify_event_to_done() {
        let event = normalize_event(&json!({
            "type": "agent-turn-complete",
            "last-assistant-message": "finished"
        }));

        assert_eq!(event.kind, "agent-turn-complete");
        assert_eq!(event.status, "done");
        assert_eq!(event.message, "finished");
    }

    #[test]
    fn clips_long_messages() {
        let input = "a".repeat(180);
        let clipped = clip(&input, 160);

        assert_eq!(clipped.chars().count(), 163);
        assert!(clipped.ends_with("..."));
    }

    #[test]
    fn allows_loopback_post_without_token() {
        let request = HttpRequest::default();
        let peer = "127.0.0.1:50000".parse::<SocketAddr>().ok();

        assert!(post_allowed(peer, &request, &test_config(None)));
    }

    #[test]
    fn rejects_lan_post_without_token() {
        let request = HttpRequest::default();
        let peer = "192.168.1.20:50000".parse::<SocketAddr>().ok();

        assert!(!post_allowed(peer, &request, &test_config(None)));
    }

    #[test]
    fn allows_lan_post_with_matching_token() {
        let mut request = HttpRequest::default();
        request.headers.insert(
            "x-codex-ornament-token".to_string(),
            "secret-token".to_string(),
        );
        let peer = "192.168.1.20:50000".parse::<SocketAddr>().ok();

        assert!(post_allowed(
            peer,
            &request,
            &test_config(Some("secret-token"))
        ));
    }
}
