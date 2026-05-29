use chrono::{DateTime, Local};
use quota_core::{get_quota_snapshot, state_path, write_state, QuotaSnapshot};
use serde::Serialize;
use serde_json::{json, Value};
use std::{
    collections::{HashMap, VecDeque},
    env,
    fs::{self, File},
    io::{self, BufRead, BufReader, Read, Write},
    net::{IpAddr, SocketAddr, TcpListener, TcpStream, UdpSocket},
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

const DEFAULT_BIND: &str = "0.0.0.0:8787";
const MAX_BODY_BYTES: usize = 64 * 1024;
const MAX_STATE_TASKS: usize = 8;
const MAX_TASK_HISTORY: usize = 16;
const QUOTA_CACHE_TTL: Duration = Duration::from_secs(60);
const RECONCILED_DONE_NOTIFY_WINDOW: Duration = Duration::from_secs(120);

#[derive(Clone, Debug, Default)]
struct BridgeState {
    task: Option<TaskEvent>,
    active_tasks: HashMap<String, TaskEvent>,
    active_order: VecDeque<String>,
    done_tasks: VecDeque<TaskEvent>,
    unmatched_stops: VecDeque<TaskEvent>,
    done_seq: u64,
    next_anonymous_task_id: u64,
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
    tracked_session_id: Option<String>,
    codex_home: PathBuf,
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
    active_task_count: usize,
    task: Option<TaskEvent>,
    active_tasks: Vec<TaskEvent>,
    done_seq: u64,
    last_done_task: Option<TaskEvent>,
    done_task_count: usize,
    unmatched_stop_count: usize,
    quota: QuotaSnapshot,
    bridge: BridgeInfo,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct BridgeInfo {
    service: &'static str,
    observed_at: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct DiscoveryInfo {
    service: &'static str,
    local_ip: String,
    state_url: String,
    health_url: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct TaskSnapshot {
    status: String,
    active_task_count: usize,
    task: Option<TaskEvent>,
    active_tasks: Vec<TaskEvent>,
    done_seq: u64,
    last_done_task: Option<TaskEvent>,
    done_task_count: usize,
    unmatched_stop_count: usize,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct TerminalTurn {
    kind: String,
    timestamp: Option<String>,
    message: Option<String>,
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
        tracked_session_id: env_text("CODEX_ORNAMENT_SESSION_ID"),
        codex_home: codex_home(),
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
        ("GET", "/discover") => write_json(&mut stream, 200, &discover_info(&config)?),
        ("GET", "/quota") => {
            let quota = cached_or_refresh_quota(&state);
            write_json(&mut stream, 200, &quota)
        }
        ("GET", "/state") => {
            let snapshot = task_snapshot(&state, &config);
            let response = OrnamentState {
                status: snapshot.status,
                active_task_count: snapshot.active_task_count,
                task: snapshot.task,
                active_tasks: snapshot.active_tasks,
                done_seq: snapshot.done_seq,
                last_done_task: snapshot.last_done_task,
                done_task_count: snapshot.done_task_count,
                unmatched_stop_count: snapshot.unmatched_stop_count,
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
                if event_in_scope(&event, &config) {
                    apply_task_event(&mut state, event.clone());
                }
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

fn task_snapshot(state: &Arc<Mutex<BridgeState>>, config: &BridgeConfig) -> TaskSnapshot {
    let Ok(mut state) = state.lock() else {
        return TaskSnapshot {
            status: "error".to_string(),
            active_task_count: 0,
            task: None,
            active_tasks: Vec::new(),
            done_seq: 0,
            last_done_task: None,
            done_task_count: 0,
            unmatched_stop_count: 0,
        };
    };

    recover_active_task_if_needed(&mut state, config);
    reconcile_active_tasks(&mut state, config);
    let active_task_count = state.active_tasks.len();
    let active_tasks = ordered_active_tasks(&state);
    let task = display_task(&state, &active_tasks);
    let status = display_status(active_task_count, task.as_ref());

    TaskSnapshot {
        status,
        active_task_count,
        task,
        active_tasks,
        done_seq: state.done_seq,
        last_done_task: state.done_tasks.back().cloned(),
        done_task_count: state.done_tasks.len(),
        unmatched_stop_count: state.unmatched_stops.len(),
    }
}

fn apply_task_event(state: &mut BridgeState, event: TaskEvent) {
    match event.status.as_str() {
        "running" => {
            let key = active_task_key_for_start(state, &event);
            insert_active_task(state, key, event.clone());
            state.task = Some(event);
        }
        "done" => {
            if finish_active_task(state, &event) {
                remember_done_task(state, event.clone());
                state.task = Some(event);
            } else if state.active_tasks.is_empty() {
                remember_done_task(state, event.clone());
                state.task = Some(event);
            } else if state.active_tasks.len() == 1 {
                remove_recent_active_task(state);
                remember_done_task(state, event.clone());
                remember_unmatched_stop(state, event.clone());
                state.task = Some(event);
            } else {
                remember_unmatched_stop(state, event);
            }
        }
        _ => {
            state.task = Some(event);
        }
    }
}

fn active_task_key_for_start(state: &mut BridgeState, event: &TaskEvent) -> String {
    if event.turn_id.is_some() {
        if let Some(key) = stable_task_key(event) {
            return key;
        }
    }

    state.next_anonymous_task_id += 1;
    match event.session_id.as_deref() {
        Some(session_id) => format!(
            "session:{session_id}:anonymous:{}",
            state.next_anonymous_task_id
        ),
        None => format!("anonymous:{}", state.next_anonymous_task_id),
    }
}

fn insert_active_task(state: &mut BridgeState, key: String, event: TaskEvent) {
    if !state.active_tasks.contains_key(&key) {
        state.active_order.push_back(key.clone());
    }
    state.active_tasks.insert(key, event);
}

fn finish_active_task(state: &mut BridgeState, event: &TaskEvent) -> bool {
    if stable_task_key(event)
        .and_then(|key| remove_active_task(state, &key))
        .is_some()
    {
        return true;
    }

    if event.turn_id.is_none() {
        if let Some(session_id) = event.session_id.as_deref() {
            return remove_recent_active_task_for_session(state, session_id).is_some();
        }
    }

    false
}

fn remove_active_task(state: &mut BridgeState, key: &str) -> Option<TaskEvent> {
    let removed = state.active_tasks.remove(key);
    if removed.is_some() {
        state.active_order.retain(|candidate| candidate != key);
    }
    removed
}

fn remove_recent_active_task(state: &mut BridgeState) -> Option<TaskEvent> {
    while let Some(key) = state.active_order.pop_back() {
        if let Some(event) = state.active_tasks.remove(&key) {
            return Some(event);
        }
    }

    let key = state.active_tasks.keys().next().cloned()?;
    state.active_tasks.remove(&key)
}

fn remove_recent_active_task_for_session(
    state: &mut BridgeState,
    session_id: &str,
) -> Option<TaskEvent> {
    let key = state.active_order.iter().rev().find_map(|key| {
        state.active_tasks.get(key).and_then(|event| {
            (event.session_id.as_deref() == Some(session_id)).then(|| key.clone())
        })
    })?;
    remove_active_task(state, &key)
}

fn remember_done_task(state: &mut BridgeState, event: TaskEvent) {
    state.done_seq = state.done_seq.saturating_add(1);
    state.done_tasks.push_back(event);
    trim_history(&mut state.done_tasks);
}

fn recover_active_task_if_needed(state: &mut BridgeState, config: &BridgeConfig) {
    if !state.active_tasks.is_empty() || state.task.is_some() {
        return;
    }

    let Some(session_id) = config.tracked_session_id.as_deref() else {
        return;
    };
    let Some(session_file) = find_session_file(&config.codex_home, session_id) else {
        return;
    };
    let Ok(Some(event)) = active_task_in_session_file(&session_file, session_id) else {
        return;
    };

    apply_task_event(state, event);
}

fn reconcile_active_tasks(state: &mut BridgeState, config: &BridgeConfig) {
    let active_keys = state.active_order.iter().cloned().collect::<Vec<_>>();
    for key in active_keys {
        let Some(event) = state.active_tasks.get(&key).cloned() else {
            continue;
        };

        if !event_in_scope(&event, config) {
            clear_active_task(state, &key);
            continue;
        }

        let Some(terminal) = terminal_turn_for_event(&config.codex_home, &event) else {
            continue;
        };

        clear_active_task(state, &key);
        if terminal.kind == "task_complete"
            && terminal
                .timestamp
                .as_deref()
                .map(|timestamp| timestamp_is_recent(timestamp, RECONCILED_DONE_NOTIFY_WINDOW))
                .unwrap_or(false)
        {
            let done_event = reconciled_done_event(&event, &terminal);
            remember_done_task(state, done_event.clone());
            state.task = Some(done_event);
        }
    }
}

fn clear_active_task(state: &mut BridgeState, key: &str) {
    if let Some(removed) = remove_active_task(state, key) {
        let task_matches_removed = state.task.as_ref() == Some(&removed);
        let task_matches_key =
            state.task.as_ref().and_then(stable_task_key).as_deref() == Some(key);
        if task_matches_removed || task_matches_key {
            state.task = None;
        }
    }
}

fn reconciled_done_event(start: &TaskEvent, terminal: &TerminalTurn) -> TaskEvent {
    TaskEvent {
        kind: terminal.kind.clone(),
        status: "done".to_string(),
        title: "Codex done".to_string(),
        message: terminal
            .message
            .as_deref()
            .map(|message| clip(message.trim(), 160))
            .filter(|message| !message.is_empty())
            .unwrap_or_else(|| "Codex done".to_string()),
        received_at: terminal.timestamp.clone().unwrap_or_else(now_local),
        session_id: start.session_id.clone(),
        turn_id: start.turn_id.clone(),
        cwd: start.cwd.clone(),
        model: start.model.clone(),
    }
}

fn event_in_scope(event: &TaskEvent, config: &BridgeConfig) -> bool {
    let Some(tracked_session_id) = config.tracked_session_id.as_deref() else {
        return true;
    };

    event
        .session_id
        .as_deref()
        .map(|session_id| session_id == tracked_session_id)
        .unwrap_or(false)
}

fn remember_unmatched_stop(state: &mut BridgeState, event: TaskEvent) {
    state.unmatched_stops.push_back(event);
    trim_history(&mut state.unmatched_stops);
}

fn trim_history(history: &mut VecDeque<TaskEvent>) {
    while history.len() > MAX_TASK_HISTORY {
        history.pop_front();
    }
}

fn ordered_active_tasks(state: &BridgeState) -> Vec<TaskEvent> {
    let mut tasks = Vec::new();
    for key in state.active_order.iter() {
        if let Some(event) = state.active_tasks.get(key) {
            tasks.push(event.clone());
            if tasks.len() >= MAX_STATE_TASKS {
                break;
            }
        }
    }
    tasks
}

fn display_task(state: &BridgeState, active_tasks: &[TaskEvent]) -> Option<TaskEvent> {
    if matches!(
        state.task.as_ref().map(|event| event.status.as_str()),
        Some("done" | "error" | "event")
    ) {
        return state.task.clone();
    }
    active_tasks.first().cloned().or_else(|| state.task.clone())
}

fn display_status(active_task_count: usize, task: Option<&TaskEvent>) -> String {
    if let Some(task) = task {
        if task.status == "done" || task.status == "error" || task.status == "event" {
            return task.status.clone();
        }
    }
    if active_task_count > 0 {
        "running".to_string()
    } else {
        task.map(|event| event.status.clone())
            .unwrap_or_else(|| "done".to_string())
    }
}

fn stable_task_key(event: &TaskEvent) -> Option<String> {
    match (event.session_id.as_deref(), event.turn_id.as_deref()) {
        (Some(session_id), Some(turn_id)) => Some(format!("session:{session_id}:turn:{turn_id}")),
        (Some(session_id), None) => Some(format!("session:{session_id}")),
        (None, Some(turn_id)) => Some(format!("turn:{turn_id}")),
        (None, None) => None,
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

fn discover_info(config: &BridgeConfig) -> io::Result<DiscoveryInfo> {
    let port = bind_port(&config.bind).unwrap_or(8787);
    let local_ip = local_lan_ip().unwrap_or_else(|| "127.0.0.1".to_string());
    Ok(DiscoveryInfo {
        service: "codex-ornament-bridge",
        local_ip: local_ip.clone(),
        state_url: format!("http://{local_ip}:{port}/state"),
        health_url: format!("http://{local_ip}:{port}/health"),
    })
}

fn bind_port(bind: &str) -> Option<u16> {
    bind.rsplit_once(':')
        .and_then(|(_, port)| port.parse::<u16>().ok())
}

fn local_lan_ip() -> Option<String> {
    let socket = UdpSocket::bind("0.0.0.0:0").ok()?;
    socket.connect("8.8.8.8:80").ok()?;
    let ip = socket.local_addr().ok()?.ip();
    match ip {
        IpAddr::V4(ip) if !ip.is_loopback() && !ip.is_unspecified() => Some(ip.to_string()),
        _ => None,
    }
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
        session_id: text_field(
            payload,
            &["session_id", "sessionId", "thread-id", "thread_id"],
        ),
        turn_id: text_field(payload, &["turn_id", "turnId", "turn-id"]),
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

fn terminal_turn_for_event(codex_home: &Path, event: &TaskEvent) -> Option<TerminalTurn> {
    let session_id = event.session_id.as_deref()?;
    let turn_id = event.turn_id.as_deref()?;
    let session_file = find_session_file(codex_home, session_id)?;
    terminal_turn_in_file(&session_file, turn_id).ok().flatten()
}

fn find_session_file(codex_home: &Path, session_id: &str) -> Option<PathBuf> {
    for root in [
        codex_home.join("sessions"),
        codex_home.join("archived_sessions"),
    ] {
        if let Some(path) = find_file_name_containing(&root, session_id) {
            return Some(path);
        }
    }
    None
}

fn find_file_name_containing(root: &Path, needle: &str) -> Option<PathBuf> {
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };

        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
                continue;
            }

            let Some(file_name) = path.file_name().and_then(|name| name.to_str()) else {
                continue;
            };
            if file_name.contains(needle) && file_name.ends_with(".jsonl") {
                return Some(path);
            }
        }
    }
    None
}

fn active_task_in_session_file(path: &Path, session_id: &str) -> io::Result<Option<TaskEvent>> {
    let file = File::open(path)?;
    let reader = BufReader::new(file);
    let mut active: Option<TaskEvent> = None;

    for line in reader.lines() {
        let line = line?;
        let Ok(record) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        let payload = &record["payload"];
        let Some(kind) = payload.get("type").and_then(Value::as_str) else {
            continue;
        };

        match kind {
            "task_started" => {
                let Some(turn_id) = payload.get("turn_id").and_then(Value::as_str) else {
                    continue;
                };
                active = Some(TaskEvent {
                    kind: "UserPromptSubmit".to_string(),
                    status: "running".to_string(),
                    title: "Codex running".to_string(),
                    message: "Codex running".to_string(),
                    received_at: record
                        .get("timestamp")
                        .and_then(Value::as_str)
                        .map(str::to_string)
                        .unwrap_or_else(now_local),
                    session_id: Some(session_id.to_string()),
                    turn_id: Some(turn_id.to_string()),
                    cwd: None,
                    model: None,
                });
            }
            "turn_context" => {
                if let Some(event) = active.as_mut() {
                    if payload.get("turn_id").and_then(Value::as_str) == event.turn_id.as_deref() {
                        event.cwd = text_field(payload, &["cwd"]).map(|value| clip(&value, 120));
                        event.model = text_field(payload, &["model"]);
                    }
                }
            }
            "task_complete" | "turn_aborted" => {
                if let Some(event) = active.as_ref() {
                    if payload.get("turn_id").and_then(Value::as_str) == event.turn_id.as_deref() {
                        active = None;
                    }
                }
            }
            _ => {}
        }
    }

    Ok(active)
}

fn terminal_turn_in_file(path: &Path, turn_id: &str) -> io::Result<Option<TerminalTurn>> {
    let file = File::open(path)?;
    let reader = BufReader::new(file);
    let mut terminal = None;

    for line in reader.lines() {
        let line = line?;
        let Ok(record) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        let payload = &record["payload"];
        let Some(kind) = payload.get("type").and_then(Value::as_str) else {
            continue;
        };
        if kind != "task_complete" && kind != "turn_aborted" {
            continue;
        }
        if payload.get("turn_id").and_then(Value::as_str) != Some(turn_id) {
            continue;
        }

        terminal = Some(TerminalTurn {
            kind: kind.to_string(),
            timestamp: record
                .get("timestamp")
                .and_then(Value::as_str)
                .map(str::to_string),
            message: terminal_message(payload),
        });
    }

    Ok(terminal)
}

fn terminal_message(payload: &Value) -> Option<String> {
    text_field(payload, &["last_agent_message", "message", "reason"])
        .map(|value| clip(value.trim(), 160))
        .filter(|value| !value.is_empty())
}

fn timestamp_is_recent(timestamp: &str, window: Duration) -> bool {
    let Ok(timestamp) = DateTime::parse_from_rfc3339(timestamp) else {
        return false;
    };
    let age = Local::now().signed_duration_since(timestamp.with_timezone(&Local));
    age.to_std().map(|age| age <= window).unwrap_or(false)
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
        "HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type, X-Codex-Ornament-Token, Access-Control-Request-Private-Network\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Private-Network: true\r\nConnection: close\r\n\r\n",
        body.len()
    )?;
    stream.write_all(body)
}

fn now_local() -> String {
    Local::now().to_rfc3339()
}

fn env_text(name: &str) -> Option<String> {
    env::var(name)
        .ok()
        .map(|value| value.trim().to_string())
        .filter(|value| !value.is_empty())
}

fn codex_home() -> PathBuf {
    env_text("CODEX_HOME")
        .map(PathBuf::from)
        .or_else(|| env_text("USERPROFILE").map(|home| PathBuf::from(home).join(".codex")))
        .unwrap_or_else(|| PathBuf::from(".codex"))
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
            tracked_session_id: None,
            codex_home: PathBuf::from(".codex-test"),
        }
    }

    fn scoped_test_config(session_id: &str, codex_home: impl Into<PathBuf>) -> BridgeConfig {
        BridgeConfig {
            bind: "127.0.0.1:8787".to_string(),
            token: None,
            tracked_session_id: Some(session_id.to_string()),
            codex_home: codex_home.into(),
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

    #[test]
    fn empty_state_reports_done_with_zero_active_tasks() {
        let state = Arc::new(Mutex::new(BridgeState::default()));

        let config = test_config(None);
        let snapshot = task_snapshot(&state, &config);

        assert_eq!(snapshot.status, "done");
        assert_eq!(snapshot.active_task_count, 0);
        assert!(snapshot.task.is_none());
    }

    #[test]
    fn tracks_multiple_running_tasks() {
        let mut state = BridgeState::default();
        let first = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));
        let second = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2",
            "turn_id": "turn-1"
        }));

        apply_task_event(&mut state, first);
        apply_task_event(&mut state, second);

        assert_eq!(state.active_tasks.len(), 2);
        assert_eq!(
            state
                .active_order
                .iter()
                .map(String::as_str)
                .collect::<Vec<_>>(),
            vec![
                "session:session-1:turn:turn-1",
                "session:session-2:turn:turn-1"
            ]
        );
    }

    #[test]
    fn queues_multiple_session_tasks_without_turn_id() {
        let mut state = BridgeState::default();
        let first = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "prompt": "first"
        }));
        let second = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "prompt": "second"
        }));

        apply_task_event(&mut state, first);
        apply_task_event(&mut state, second);

        assert_eq!(state.active_tasks.len(), 2);
        assert_eq!(
            state
                .active_order
                .iter()
                .map(String::as_str)
                .collect::<Vec<_>>(),
            vec![
                "session:session-1:anonymous:1",
                "session:session-1:anonymous:2"
            ]
        );
    }

    #[test]
    fn stop_removes_matching_running_task() {
        let mut state = BridgeState::default();
        let start = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));
        let stop = normalize_event(&json!({
            "hook_event_name": "Stop",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));

        apply_task_event(&mut state, start);
        apply_task_event(&mut state, stop);

        assert_eq!(state.active_tasks.len(), 0);
        assert_eq!(
            state.task.as_ref().map(|event| event.status.as_str()),
            Some("done")
        );
    }

    #[test]
    fn anonymous_stop_decrements_single_running_task() {
        let mut state = BridgeState::default();
        apply_task_event(
            &mut state,
            normalize_event(&json!({"hook_event_name": "UserPromptSubmit"})),
        );

        apply_task_event(
            &mut state,
            normalize_event(&json!({"hook_event_name": "Stop"})),
        );

        assert_eq!(state.active_tasks.len(), 0);
        assert_eq!(state.done_tasks.len(), 1);
        assert_eq!(state.unmatched_stops.len(), 1);
    }

    #[test]
    fn stop_with_no_active_task_reports_done_without_unmatched_count() {
        let mut state = BridgeState::default();

        apply_task_event(
            &mut state,
            normalize_event(&json!({"hook_event_name": "Stop"})),
        );

        assert_eq!(state.active_tasks.len(), 0);
        assert_eq!(state.done_tasks.len(), 1);
        assert_eq!(state.unmatched_stops.len(), 0);
        assert_eq!(
            state.task.as_ref().map(|event| event.status.as_str()),
            Some("done")
        );
    }

    #[test]
    fn session_stop_without_turn_id_removes_recent_session_task() {
        let mut state = BridgeState::default();
        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": "session-1",
                "prompt": "first"
            })),
        );
        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": "session-1",
                "prompt": "second"
            })),
        );

        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "Stop",
                "session_id": "session-1"
            })),
        );

        assert_eq!(state.active_tasks.len(), 1);
        assert_eq!(state.done_tasks.len(), 1);
        assert!(state
            .active_tasks
            .values()
            .any(|event| event.message == "first"));
    }

    #[test]
    fn unmatched_stop_does_not_guess_when_multiple_tasks_are_active() {
        let mut state = BridgeState::default();
        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": "session-1"
            })),
        );
        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": "session-2"
            })),
        );

        apply_task_event(
            &mut state,
            normalize_event(&json!({"hook_event_name": "Stop"})),
        );

        assert_eq!(state.active_tasks.len(), 2);
        assert_eq!(state.done_tasks.len(), 0);
        assert_eq!(state.unmatched_stops.len(), 1);
    }

    #[test]
    fn matching_stop_shows_done_even_when_other_tasks_are_still_active() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-2",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "session-1",
                    "turn_id": "turn-1",
                    "message": "first done"
                })),
            );
        }

        let config = test_config(None);
        let snapshot = task_snapshot(&state, &config);

        assert_eq!(snapshot.status, "done");
        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(
            snapshot.task.as_ref().map(|event| event.message.as_str()),
            Some("first done")
        );
        assert_eq!(snapshot.done_seq, 1);
        assert_eq!(
            snapshot
                .last_done_task
                .as_ref()
                .map(|event| event.message.as_str()),
            Some("first done")
        );
        assert_eq!(snapshot.done_task_count, 1);
    }

    #[test]
    fn snapshot_preserves_active_task_order() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-2"
                })),
            );
        }

        let config = test_config(None);
        let snapshot = task_snapshot(&state, &config);

        assert_eq!(snapshot.status, "running");
        assert_eq!(snapshot.active_tasks.len(), 2);
        assert_eq!(
            snapshot
                .active_tasks
                .iter()
                .filter_map(|event| event.session_id.as_deref())
                .collect::<Vec<_>>(),
            vec!["session-1", "session-2"]
        );
    }

    #[test]
    fn scoped_bridge_accepts_only_tracked_session_events() {
        let config = scoped_test_config("session-1", ".codex-test");
        let matching = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1"
        }));
        let other = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2"
        }));
        let missing = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit"
        }));

        assert!(event_in_scope(&matching, &config));
        assert!(!event_in_scope(&other, &config));
        assert!(!event_in_scope(&missing, &config));
    }

    #[test]
    fn unscoped_bridge_accepts_events_from_any_session() {
        let config = test_config(None);
        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2"
        }));

        assert!(event_in_scope(&event, &config));
    }

    #[test]
    fn terminal_turn_in_file_detects_completed_turn() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-terminal-turn-{}-complete.jsonl",
            std::process::id()
        ));
        fs::write(
            &path,
            concat!(
                "{\"timestamp\":\"2026-05-25T12:00:00+08:00\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"turn-1\",\"last_agent_message\":\"finished\"}}\n",
                "{\"timestamp\":\"2026-05-25T12:00:01+08:00\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"turn-2\",\"last_agent_message\":\"other\"}}\n"
            ),
        )
        .unwrap();

        let terminal = terminal_turn_in_file(&path, "turn-1").unwrap().unwrap();
        let _ = fs::remove_file(&path);

        assert_eq!(terminal.kind, "task_complete");
        assert_eq!(terminal.message.as_deref(), Some("finished"));
    }

    #[test]
    fn reconcile_clears_aborted_turn_from_session_log() {
        let codex_home =
            env::temp_dir().join(format!("codex-ornament-reconcile-{}", std::process::id()));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("25");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-25T13-36-13-session-1.jsonl"),
            "{\"timestamp\":\"2026-05-25T12:00:00+08:00\",\"payload\":{\"type\":\"turn_aborted\",\"turn_id\":\"turn-1\",\"reason\":\"interrupted\"}}\n",
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-1"
                })),
            );
        }

        let config = scoped_test_config("session-1", &codex_home);
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
    }

    #[test]
    fn recovers_active_turn_from_session_log_after_bridge_restart() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-recover-active-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("25");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-25T13-36-13-session-1.jsonl"),
            concat!(
                "{\"timestamp\":\"2026-05-25T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-25T12:00:01+08:00\",\"payload\":{\"type\":\"turn_context\",\"turn_id\":\"turn-1\",\"cwd\":\"D:\\\\Desktop\\\\codex\",\"model\":\"gpt-5.5\"}}\n"
            ),
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = scoped_test_config("session-1", &codex_home);
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.status, "running");
        let task = snapshot.task.unwrap();
        assert_eq!(task.turn_id.as_deref(), Some("turn-1"));
        assert_eq!(task.cwd.as_deref(), Some("D:\\Desktop\\codex"));
        assert_eq!(task.model.as_deref(), Some("gpt-5.5"));
    }

    #[test]
    fn does_not_recover_completed_turn_from_session_log() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-recover-complete-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("25");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-25T13-36-13-session-1.jsonl"),
            concat!(
                "{\"timestamp\":\"2026-05-25T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-25T12:00:02+08:00\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"turn-1\"}}\n"
            ),
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = scoped_test_config("session-1", &codex_home);
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
    }

    #[test]
    fn scope_reconcile_drops_stale_display_task() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-2"
                })),
            );
        }

        let config = scoped_test_config("session-1", ".codex-test");
        let snapshot = task_snapshot(&state, &config);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
        assert!(snapshot.task.is_none());
    }
}
