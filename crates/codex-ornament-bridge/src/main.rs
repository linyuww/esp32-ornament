use chrono::{DateTime, FixedOffset, Local};
use quota_core::{
    get_quota_snapshot, read_state, state_path, write_state, QuotaSnapshot, SnapshotStatus,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    collections::{HashMap, HashSet, VecDeque},
    env,
    fs::{self, File},
    io::{self, BufRead, BufReader, Read, Seek, SeekFrom, Write},
    net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, TcpStream, UdpSocket},
    path::{Path, PathBuf},
    sync::{
        mpsc::{self, Receiver, SyncSender, TrySendError},
        Arc, Mutex,
    },
    time::{Duration, Instant},
};

const DEFAULT_BIND: &str = "0.0.0.0:8787";
const MAX_BODY_BYTES: usize = 64 * 1024;
const MAX_STATE_TASKS: usize = 8;
const MAX_TASK_HISTORY: usize = 16;
const TASK_EVENT_QUEUE_CAPACITY: usize = 64;
const TASK_EVENT_ACK_TIMEOUT: Duration = Duration::from_millis(750);
const QUOTA_REFRESH_INTERVAL: Duration = Duration::from_secs(60);
const QUOTA_CACHE_TTL: Duration = QUOTA_REFRESH_INTERVAL;
const WEATHER_CACHE_TTL: Duration = Duration::from_secs(10 * 60);
const WEATHER_FETCH_TIMEOUT: Duration = Duration::from_secs(5);
const ACTIVE_RECOVERY_SCAN_TTL: Duration = Duration::from_secs(5);
const DEFAULT_WEATHER_LATITUDE: f64 = 39.99540087499999;
const DEFAULT_WEATHER_LONGITUDE: f64 = 116.34162524999999;
const DEFAULT_WEATHER_LABEL: &str = "HAIDIAN";
const DEFAULT_WEATHER_PROVIDER: WeatherProvider = WeatherProvider::Auto;
const RECONCILED_DONE_NOTIFY_WINDOW: Duration = Duration::from_secs(120);
const COMBINED_DONE_SOURCE_WINDOW: Duration = Duration::from_secs(5);
const RECOVER_ACTIVE_TASK_WINDOW: Duration = Duration::from_secs(12 * 60 * 60);
const RECOVER_ACTIVE_SESSION_SCAN_LIMIT: usize = 24;
const SESSION_TASK_SCAN_TAIL_BYTES: u64 = 2 * 1024 * 1024;
const ACTIVE_SESSION_FILE_MISSING_GRACE: Duration = Duration::from_secs(30);
const SESSION_FORK_CHAIN_LIMIT: usize = 8;
const DISCOVERY_MAGIC: &str = "codex-ornament-discover-v1";

type SharedBridgeState = Arc<Mutex<BridgeState>>;
type TaskEventSender = SyncSender<QueuedTaskEvent>;
type TaskEventReceiver = Receiver<QueuedTaskEvent>;

#[derive(Clone, Debug, Default)]
struct BridgeState {
    task: Option<TaskEvent>,
    active_tasks: HashMap<String, TaskEvent>,
    active_order: VecDeque<String>,
    done_tasks: VecDeque<TaskEvent>,
    unmatched_stops: VecDeque<TaskEvent>,
    done_seq: u64,
    codex_done_seq: u64,
    claude_done_seq: u64,
    next_anonymous_task_id: u64,
    quota: Option<CachedQuota>,
    quota_refreshing: bool,
    weather: Option<CachedWeather>,
    weather_refreshing: bool,
    active_recovery: Option<CachedActiveRecovery>,
}

#[derive(Clone, Debug)]
struct CachedQuota {
    snapshot: QuotaSnapshot,
    fetched_at: Instant,
}

#[derive(Clone, Debug)]
struct CachedActiveRecovery {
    events: Vec<TaskEvent>,
    fetched_at: Instant,
}

#[derive(Clone, Debug)]
struct BridgeConfig {
    bind: String,
    token: Option<String>,
    tracked_session_id: Option<String>,
    codex_home: PathBuf,
    weather_latitude: f64,
    weather_longitude: f64,
    weather_label: String,
    weather_provider: WeatherProvider,
    qweather_host: Option<String>,
    qweather_token: Option<String>,
    caiyun_token: Option<String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum WeatherProvider {
    Auto,
    OpenMeteo,
    QWeather,
    Caiyun,
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
    source: Option<String>,
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
    source_tasks: SourceTasks,
    done_seq: u64,
    last_done_task: Option<TaskEvent>,
    done_task_count: usize,
    unmatched_stop_count: usize,
    quota: QuotaSnapshot,
    weather: WeatherSnapshot,
    bridge: BridgeInfo,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
struct WeatherSnapshot {
    status: String,
    label: String,
    summary: String,
    icon: String,
    temperature_c: Option<i32>,
    wind_kmh: Option<i32>,
    weather_code: Option<i32>,
    observed_at: String,
}

#[derive(Clone, Debug)]
struct CachedWeather {
    snapshot: WeatherSnapshot,
    fetched_at: Instant,
}

#[derive(Deserialize)]
struct OpenMeteoResponse {
    current: Option<OpenMeteoCurrent>,
}

#[derive(Deserialize)]
struct OpenMeteoCurrent {
    temperature_2m: Option<f64>,
    weather_code: Option<i32>,
    wind_speed_10m: Option<f64>,
}

#[derive(Deserialize)]
struct QWeatherResponse {
    code: String,
    now: Option<QWeatherNow>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct QWeatherNow {
    obs_time: Option<String>,
    temp: Option<String>,
    icon: Option<String>,
    text: Option<String>,
    wind_speed: Option<String>,
}

#[derive(Deserialize)]
struct CaiyunResponse {
    status: String,
    result: Option<CaiyunResult>,
}

#[derive(Deserialize)]
struct CaiyunResult {
    realtime: Option<CaiyunRealtime>,
}

#[derive(Deserialize)]
struct CaiyunRealtime {
    temperature: Option<f64>,
    skycon: Option<String>,
    wind: Option<CaiyunWind>,
}

#[derive(Deserialize)]
struct CaiyunWind {
    speed: Option<f64>,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct SourceTasks {
    codex: SourceTaskSummary,
    claude: SourceTaskSummary,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct SourceTaskSummary {
    status: String,
    active_count: usize,
    done_seq: u64,
    task: Option<TaskEvent>,
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
    source_tasks: SourceTasks,
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

#[derive(Clone, Debug)]
struct PendingFinalAnswer {
    turn_id: String,
    timestamp: Option<String>,
    message: Option<String>,
}

struct QueuedTaskEvent {
    event: TaskEvent,
    completion: SyncSender<TaskDispatchResult>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TaskDispatchResult {
    Applied,
    Filtered,
    Queued,
    LockUnavailable,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TaskQueueError {
    Full,
    Closed,
    CompletionDropped,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum DoneSource {
    Claude,
    Codex,
    Other,
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
        weather_latitude: env_f64("CODEX_ORNAMENT_WEATHER_LAT").unwrap_or(DEFAULT_WEATHER_LATITUDE),
        weather_longitude: env_f64("CODEX_ORNAMENT_WEATHER_LON")
            .unwrap_or(DEFAULT_WEATHER_LONGITUDE),
        weather_label: env_text("CODEX_ORNAMENT_WEATHER_LABEL")
            .unwrap_or_else(|| DEFAULT_WEATHER_LABEL.to_string()),
        weather_provider: env_weather_provider("CODEX_ORNAMENT_WEATHER_PROVIDER")
            .unwrap_or(DEFAULT_WEATHER_PROVIDER),
        qweather_host: env_text("CODEX_ORNAMENT_QWEATHER_HOST"),
        qweather_token: env_text("CODEX_ORNAMENT_QWEATHER_TOKEN")
            .or_else(|| env_text("CODEX_ORNAMENT_QWEATHER_KEY")),
        caiyun_token: env_text("CODEX_ORNAMENT_CAIYUN_TOKEN")
            .or_else(|| env_text("CODEX_ORNAMENT_CAIYUN_KEY")),
    };
    let listener = TcpListener::bind(&config.bind)?;
    let state = Arc::new(Mutex::new(BridgeState::default()));
    let (task_events, task_event_receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
    spawn_task_event_consumer(Arc::clone(&state), config.clone(), task_event_receiver);
    spawn_discovery_responder(config.clone());
    spawn_quota_refresh_loop(Arc::clone(&state));

    eprintln!("codex ornament bridge listening on http://{}", config.bind);
    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                let state = Arc::clone(&state);
                let config = config.clone();
                let task_events = task_events.clone();
                std::thread::spawn(move || {
                    if let Err(error) = handle_connection(stream, state, task_events, config) {
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
    state: SharedBridgeState,
    task_events: TaskEventSender,
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
                source_tasks: snapshot.source_tasks,
                done_seq: snapshot.done_seq,
                last_done_task: snapshot.last_done_task,
                done_task_count: snapshot.done_task_count,
                unmatched_stop_count: snapshot.unmatched_stop_count,
                quota: cached_or_refresh_quota_background(&state),
                weather: cached_or_refresh_weather(&state, &config),
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

            let payload = match parse_hook_payload(&request.body) {
                Ok(payload) => payload,
                Err(error) => {
                    eprintln!(
                        "invalid hook json ignored: {error}; body={}",
                        clip(&String::from_utf8_lossy(&request.body), 160)
                    );
                    return write_json(
                        &mut stream,
                        400,
                        &json!({"ok": false, "error": "invalid hook json"}),
                    );
                }
            };
            let event = normalize_event(&payload);
            match dispatch_task_event(&task_events, event.clone()) {
                Ok(TaskDispatchResult::Applied | TaskDispatchResult::Filtered) => {
                    write_json(&mut stream, 200, &json!({"ok": true, "event": event}))
                }
                Ok(TaskDispatchResult::Queued) => write_json(
                    &mut stream,
                    202,
                    &json!({"ok": true, "queued": true, "event": event}),
                ),
                Ok(TaskDispatchResult::LockUnavailable) => write_json(
                    &mut stream,
                    500,
                    &json!({"ok": false, "error": "task state lock unavailable", "event": event}),
                ),
                Err(TaskQueueError::Full) => write_json(
                    &mut stream,
                    503,
                    &json!({"ok": false, "error": "task queue full", "event": event}),
                ),
                Err(TaskQueueError::Closed | TaskQueueError::CompletionDropped) => write_json(
                    &mut stream,
                    503,
                    &json!({"ok": false, "error": "task consumer unavailable", "event": event}),
                ),
            }
        }
        _ => write_json(
            &mut stream,
            404,
            &json!({"ok": false, "error": "not found"}),
        ),
    }
}

fn parse_hook_payload(body: &[u8]) -> Result<Value, serde_json::Error> {
    serde_json::from_slice::<Value>(body)
}

fn spawn_task_event_consumer(
    state: SharedBridgeState,
    config: BridgeConfig,
    receiver: TaskEventReceiver,
) {
    std::thread::spawn(move || consume_task_events(state, config, receiver));
}

fn spawn_discovery_responder(config: BridgeConfig) {
    std::thread::spawn(move || {
        if let Err(error) = run_discovery_responder(config) {
            eprintln!("discovery responder failed: {error}");
        }
    });
}

fn spawn_quota_refresh_loop(state: SharedBridgeState) {
    std::thread::spawn(move || loop {
        maybe_spawn_quota_refresh_if_due(Arc::clone(&state));
        std::thread::sleep(QUOTA_REFRESH_INTERVAL);
    });
}

fn run_discovery_responder(config: BridgeConfig) -> io::Result<()> {
    let port = bind_port(&config.bind).unwrap_or(8787);
    let socket = UdpSocket::bind(("0.0.0.0", port))?;
    socket.set_broadcast(true)?;
    eprintln!("codex ornament discovery listening on udp://0.0.0.0:{port}");

    let mut buffer = [0_u8; 256];
    loop {
        let (length, peer) = socket.recv_from(&mut buffer)?;
        let request = String::from_utf8_lossy(&buffer[..length]);
        if request.trim() != DISCOVERY_MAGIC {
            continue;
        }

        let Ok(info) = discover_info_for_peer(&config, peer) else {
            continue;
        };
        let Ok(response) = serde_json::to_vec(&info) else {
            continue;
        };
        let _ = socket.send_to(&response, peer);
    }
}

fn consume_task_events(
    state: SharedBridgeState,
    config: BridgeConfig,
    receiver: TaskEventReceiver,
) {
    for queued in receiver {
        let result = consume_task_event(&state, &config, queued.event);
        let _ = queued.completion.send(result);
    }
}

fn consume_task_event(
    state: &SharedBridgeState,
    config: &BridgeConfig,
    event: TaskEvent,
) -> TaskDispatchResult {
    if !event_in_scope(&event, config) {
        return TaskDispatchResult::Filtered;
    }

    let Ok(mut state) = state.lock() else {
        return TaskDispatchResult::LockUnavailable;
    };
    apply_task_event(&mut state, event);
    TaskDispatchResult::Applied
}

fn dispatch_task_event(
    sender: &TaskEventSender,
    event: TaskEvent,
) -> Result<TaskDispatchResult, TaskQueueError> {
    let (completion, completed) = mpsc::sync_channel(0);
    let queued = QueuedTaskEvent { event, completion };

    match sender.try_send(queued) {
        Ok(()) => match completed.recv_timeout(TASK_EVENT_ACK_TIMEOUT) {
            Ok(result) => Ok(result),
            Err(mpsc::RecvTimeoutError::Timeout) => Ok(TaskDispatchResult::Queued),
            Err(mpsc::RecvTimeoutError::Disconnected) => Err(TaskQueueError::CompletionDropped),
        },
        Err(TrySendError::Full(_)) => Err(TaskQueueError::Full),
        Err(TrySendError::Disconnected(_)) => Err(TaskQueueError::Closed),
    }
}

fn task_snapshot(state: &SharedBridgeState, config: &BridgeConfig) -> TaskSnapshot {
    let Ok(mut state) = state.lock() else {
        return TaskSnapshot {
            status: "error".to_string(),
            active_task_count: 0,
            task: None,
            active_tasks: Vec::new(),
            source_tasks: SourceTasks::default(),
            done_seq: 0,
            last_done_task: None,
            done_task_count: 0,
            unmatched_stop_count: 0,
        };
    };

    recover_active_task_if_needed(&mut state, config);
    dedupe_active_tasks_by_turn(&mut state);
    dedupe_active_tasks_by_session_forks(&mut state, config);
    reconcile_active_tasks(&mut state, config);
    let active_task_count = state.active_tasks.len();
    let active_tasks = ordered_active_tasks(&state);
    let source_tasks = source_task_summaries(&state, &active_tasks);
    let mut task = display_task(&state, &active_tasks);
    if matches!(
        task.as_ref().map(|event| event.status.as_str()),
        Some("done")
    ) {
        if let Some(combined) = combined_recent_done_task(&state) {
            task = Some(combined);
        }
    }
    let status = display_status(active_task_count, task.as_ref());

    TaskSnapshot {
        status,
        active_task_count,
        task,
        active_tasks,
        source_tasks,
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
            let matched_active_task = finish_active_task(state, &event);
            if matched_active_task {
                remember_done_task(state, event.clone());
                state.task = Some(event);
            } else if state.active_tasks.is_empty() {
                if event_has_task_identity(&event) {
                    remember_unmatched_stop(state, event);
                } else {
                    remember_done_task(state, event.clone());
                    state.task = Some(event);
                }
            } else if !event_has_task_identity(&event)
                && active_task_count_for_source(state, &event) == 1
            {
                remove_recent_active_task_for_source(state, &event);
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

fn event_has_task_identity(event: &TaskEvent) -> bool {
    event.session_id.is_some() || event.turn_id.is_some()
}

fn active_task_key_for_start(state: &mut BridgeState, event: &TaskEvent) -> String {
    if event.turn_id.is_some() {
        remove_active_tasks_for_same_turn(state, event);
        if let Some(key) = stable_task_key(event) {
            return key;
        }
    }

    state.next_anonymous_task_id += 1;
    let source = task_source_key(event);
    match event.session_id.as_deref() {
        Some(session_id) => format!(
            "source:{source}:session:{session_id}:anonymous:{}",
            state.next_anonymous_task_id
        ),
        None => format!("source:{source}:anonymous:{}", state.next_anonymous_task_id),
    }
}

fn insert_active_task(state: &mut BridgeState, key: String, event: TaskEvent) {
    if !state.active_tasks.contains_key(&key) {
        state.active_order.push_back(key.clone());
    }
    state.active_tasks.insert(key, event);
}

fn finish_active_task(state: &mut BridgeState, event: &TaskEvent) -> bool {
    let matched_exact = stable_task_key(event)
        .and_then(|key| remove_active_task(state, &key))
        .is_some();
    let matched_same_turn = remove_active_tasks_for_same_turn(state, event) > 0;
    if matched_exact || matched_same_turn {
        return true;
    }

    if event.turn_id.is_none() {
        if let Some(session_id) = event.session_id.as_deref() {
            return remove_recent_active_task_for_session(state, session_id, event).is_some();
        }
    }

    false
}

fn remove_active_tasks_for_same_turn(state: &mut BridgeState, event: &TaskEvent) -> usize {
    let Some(turn_id) = event.turn_id.as_deref() else {
        return 0;
    };

    let keys = state
        .active_order
        .iter()
        .filter_map(|key| {
            state.active_tasks.get(key).and_then(|active| {
                (active.turn_id.as_deref() == Some(turn_id) && task_sources_match(active, event))
                    .then(|| key.clone())
            })
        })
        .collect::<Vec<_>>();
    let count = keys.len();
    for key in keys {
        remove_active_task(state, &key);
    }
    count
}

fn dedupe_active_tasks_by_turn(state: &mut BridgeState) {
    let mut latest_by_turn = HashMap::new();
    for key in state.active_order.iter() {
        let Some(event) = state.active_tasks.get(key) else {
            continue;
        };
        let Some(turn_id) = event.turn_id.as_deref() else {
            continue;
        };
        latest_by_turn.insert((task_source_key(event), turn_id.to_string()), key.clone());
    }

    let stale_keys = state
        .active_order
        .iter()
        .filter_map(|key| {
            let event = state.active_tasks.get(key)?;
            let turn_id = event.turn_id.as_deref()?;
            let latest_key = latest_by_turn.get(&(task_source_key(event), turn_id.to_string()))?;
            (latest_key != key).then(|| key.clone())
        })
        .collect::<Vec<_>>();

    for key in stale_keys {
        clear_active_task(state, &key);
    }
}

fn dedupe_active_tasks_by_session_forks(state: &mut BridgeState, config: &BridgeConfig) {
    let active_sessions = state
        .active_order
        .iter()
        .filter_map(|key| {
            let event = state.active_tasks.get(key)?;
            Some((
                key.clone(),
                event.clone(),
                event.session_id.as_deref()?.to_string(),
            ))
        })
        .collect::<Vec<_>>();
    if active_sessions.len() < 2 {
        return;
    }

    let mut ancestor_cache: HashMap<String, HashSet<String>> = HashMap::new();
    let mut stale_keys = HashSet::new();

    for (descendant_key, descendant_event, descendant_session) in &active_sessions {
        let ancestors = ancestor_cache
            .entry(descendant_session.clone())
            .or_insert_with(|| {
                session_ancestor_ids(&config.codex_home, descendant_session)
                    .into_iter()
                    .collect()
            });
        if ancestors.is_empty() {
            continue;
        }

        for (candidate_key, candidate_event, candidate_session) in &active_sessions {
            if candidate_key == descendant_key
                || !task_sources_match(candidate_event, descendant_event)
            {
                continue;
            }
            if ancestors.contains(candidate_session)
                && session_fork_happened_after_event(
                    &config.codex_home,
                    descendant_session,
                    candidate_event,
                )
            {
                stale_keys.insert(candidate_key.clone());
            }
        }
    }

    for key in stale_keys {
        clear_active_task(state, &key);
    }
}

fn remove_active_task(state: &mut BridgeState, key: &str) -> Option<TaskEvent> {
    let removed = state.active_tasks.remove(key);
    if removed.is_some() {
        state.active_order.retain(|candidate| candidate != key);
    }
    if let Some(event) = removed.as_ref() {
        forget_active_recovery_event(state, event);
    }
    removed
}

fn forget_active_recovery_event(state: &mut BridgeState, event: &TaskEvent) {
    let Some(event_key) = stable_task_key(event) else {
        return;
    };
    let Some(cache) = state.active_recovery.as_mut() else {
        return;
    };
    cache.events.retain(|candidate| {
        stable_task_key(candidate)
            .map(|candidate_key| candidate_key != event_key)
            .unwrap_or(true)
    });
}

fn remove_recent_active_task_for_session(
    state: &mut BridgeState,
    session_id: &str,
    terminal_event: &TaskEvent,
) -> Option<TaskEvent> {
    let key = state.active_order.iter().rev().find_map(|key| {
        state.active_tasks.get(key).and_then(|event| {
            (event.session_id.as_deref() == Some(session_id)
                && event.turn_id.is_none()
                && task_sources_match(event, terminal_event))
            .then(|| key.clone())
        })
    })?;
    remove_active_task(state, &key)
}

fn remove_recent_active_task_for_source(
    state: &mut BridgeState,
    terminal_event: &TaskEvent,
) -> Option<TaskEvent> {
    let key = state.active_order.iter().rev().find_map(|key| {
        state
            .active_tasks
            .get(key)
            .and_then(|event| task_sources_match(event, terminal_event).then(|| key.clone()))
    })?;
    remove_active_task(state, &key)
}

fn active_task_count_for_source(state: &BridgeState, terminal_event: &TaskEvent) -> usize {
    state
        .active_tasks
        .values()
        .filter(|event| task_sources_match(event, terminal_event))
        .count()
}

fn remember_done_task(state: &mut BridgeState, event: TaskEvent) {
    state.done_seq = state.done_seq.saturating_add(1);
    match done_source(&event) {
        DoneSource::Claude => state.claude_done_seq = state.claude_done_seq.saturating_add(1),
        DoneSource::Codex => state.codex_done_seq = state.codex_done_seq.saturating_add(1),
        DoneSource::Other => {}
    }
    state.done_tasks.push_back(event);
    trim_history(&mut state.done_tasks);
}

fn recover_active_task_if_needed(state: &mut BridgeState, config: &BridgeConfig) {
    let events = if let Some(session_id) = config.tracked_session_id.as_deref() {
        active_task_for_session(&config.codex_home, session_id)
            .into_iter()
            .collect::<Vec<_>>()
    } else {
        cached_active_recovery_events(state, config)
    };

    for event in events {
        if !event_in_scope(&event, config) || active_task_is_already_tracked(state, &event) {
            continue;
        }
        apply_task_event(state, event);
    }
}

fn cached_active_recovery_events(state: &mut BridgeState, config: &BridgeConfig) -> Vec<TaskEvent> {
    if let Some(cache) = state.active_recovery.as_ref() {
        if cache.fetched_at.elapsed() < ACTIVE_RECOVERY_SCAN_TTL {
            return cache.events.clone();
        }
    }

    let events = active_tasks_in_recent_session_files(&config.codex_home);
    state.active_recovery = Some(CachedActiveRecovery {
        events: events.clone(),
        fetched_at: Instant::now(),
    });
    events
}

fn active_task_for_session(codex_home: &Path, session_id: &str) -> Option<TaskEvent> {
    let session_file = find_session_file(codex_home, session_id)?;
    active_task_in_session_file(&session_file, session_id)
        .ok()
        .flatten()
}

fn active_task_is_already_tracked(state: &BridgeState, event: &TaskEvent) -> bool {
    stable_task_key(event)
        .map(|key| state.active_tasks.contains_key(&key))
        .unwrap_or(false)
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
            if active_task_is_stale_against_session_log(&config.codex_home, &event) {
                clear_active_task(state, &key);
            }
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
        title: task_title_for("done", start.source.as_deref()),
        message: terminal
            .message
            .as_deref()
            .map(|message| clip(message.trim(), 160))
            .filter(|message| !message.is_empty())
            .unwrap_or_else(|| task_title_for("done", start.source.as_deref())),
        received_at: terminal.timestamp.clone().unwrap_or_else(now_local),
        source: start.source.clone(),
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
    if let Some(task) = state.task.as_ref() {
        if matches!(task.status.as_str(), "done" | "error" | "event")
            && active_tasks
                .iter()
                .any(|active| stable_task_key(active) == stable_task_key(task))
        {
            return active_tasks.first().cloned().or_else(|| state.task.clone());
        }
    }
    if matches!(
        state.task.as_ref().map(|event| event.status.as_str()),
        Some("done" | "error" | "event")
    ) {
        return state.task.clone();
    }
    active_tasks.first().cloned().or_else(|| state.task.clone())
}

impl Default for SourceTasks {
    fn default() -> Self {
        Self {
            codex: SourceTaskSummary::empty(),
            claude: SourceTaskSummary::empty(),
        }
    }
}

impl SourceTaskSummary {
    fn empty() -> Self {
        Self {
            status: "done".to_string(),
            active_count: 0,
            done_seq: 0,
            task: None,
        }
    }
}

fn source_task_summaries(state: &BridgeState, active_tasks: &[TaskEvent]) -> SourceTasks {
    SourceTasks {
        codex: source_task_summary(state, active_tasks, DoneSource::Codex, state.codex_done_seq),
        claude: source_task_summary(
            state,
            active_tasks,
            DoneSource::Claude,
            state.claude_done_seq,
        ),
    }
}

fn source_task_summary(
    state: &BridgeState,
    active_tasks: &[TaskEvent],
    source: DoneSource,
    done_seq: u64,
) -> SourceTaskSummary {
    let active = active_tasks
        .iter()
        .filter(|event| done_source(event) == source)
        .cloned()
        .collect::<Vec<_>>();
    let active_count = active.len();
    let task = source_display_task(state, &active, source);
    let status = display_status(active_count, task.as_ref());
    SourceTaskSummary {
        status,
        active_count,
        done_seq,
        task,
    }
}

fn source_display_task(
    state: &BridgeState,
    active_tasks: &[TaskEvent],
    source: DoneSource,
) -> Option<TaskEvent> {
    if let Some(active_task) = active_tasks.first() {
        return Some(active_task.clone());
    }
    if let Some(task) = state.task.as_ref() {
        if matches!(task.status.as_str(), "done" | "error" | "event") && done_source(task) == source
        {
            return Some(task.clone());
        }
    }
    latest_done_task_for_source(state, source)
}

fn latest_done_task_for_source(state: &BridgeState, source: DoneSource) -> Option<TaskEvent> {
    state
        .done_tasks
        .iter()
        .rev()
        .find(|event| event.status == "done" && done_source(event) == source)
        .cloned()
}

fn combined_recent_done_task(state: &BridgeState) -> Option<TaskEvent> {
    let latest = state.done_tasks.back()?;
    if latest.status != "done" {
        return None;
    }

    let latest_at = DateTime::parse_from_rfc3339(&latest.received_at).ok()?;
    let mut saw_claude = false;
    let mut saw_codex = false;

    for event in state.done_tasks.iter().rev() {
        if event.status != "done" {
            continue;
        }
        let Some(age) = age_from_latest_done(latest_at, event) else {
            continue;
        };
        if age > COMBINED_DONE_SOURCE_WINDOW {
            break;
        }

        match done_source(event) {
            DoneSource::Claude => saw_claude = true,
            DoneSource::Codex => saw_codex = true,
            DoneSource::Other => {}
        }

        if saw_claude && saw_codex {
            let mut combined = latest.clone();
            combined.title = "Claude + Codex done".to_string();
            combined.message = "Claude + Codex done".to_string();
            combined.source = Some("claude+codex".to_string());
            return Some(combined);
        }
    }

    None
}

fn age_from_latest_done(latest_at: DateTime<FixedOffset>, event: &TaskEvent) -> Option<Duration> {
    let event_at = DateTime::parse_from_rfc3339(&event.received_at).ok()?;
    latest_at.signed_duration_since(event_at).to_std().ok()
}

fn done_source(event: &TaskEvent) -> DoneSource {
    if event
        .source
        .as_deref()
        .map(source_is_claude)
        .unwrap_or(false)
        || event.title.eq_ignore_ascii_case("Claude running")
        || event.title.eq_ignore_ascii_case("Claude done")
    {
        return DoneSource::Claude;
    }
    if event
        .source
        .as_deref()
        .map(source_is_codex)
        .unwrap_or(false)
        || event.source.is_none()
        || event.title.eq_ignore_ascii_case("Codex running")
        || event.title.eq_ignore_ascii_case("Codex done")
    {
        return DoneSource::Codex;
    }
    DoneSource::Other
}

fn source_is_claude(source: &str) -> bool {
    matches!(canonical_source(source).as_str(), "claude" | "claudecode")
}

fn source_is_codex(source: &str) -> bool {
    canonical_source(source) == "codex"
}

fn canonical_source(source: &str) -> String {
    source
        .chars()
        .filter(|ch| *ch != '_' && *ch != '-' && !ch.is_whitespace())
        .collect::<String>()
        .to_ascii_lowercase()
}

fn task_source_key(event: &TaskEvent) -> String {
    match done_source(event) {
        DoneSource::Claude => "claude".to_string(),
        DoneSource::Codex => "codex".to_string(),
        DoneSource::Other => event
            .source
            .as_deref()
            .map(canonical_source)
            .filter(|source| !source.is_empty())
            .unwrap_or_else(|| "other".to_string()),
    }
}

fn task_sources_match(active_event: &TaskEvent, terminal_event: &TaskEvent) -> bool {
    task_source_key(active_event) == task_source_key(terminal_event)
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
    let source = task_source_key(event);
    match (event.session_id.as_deref(), event.turn_id.as_deref()) {
        (Some(session_id), Some(turn_id)) => Some(format!(
            "source:{source}:session:{session_id}:turn:{turn_id}"
        )),
        (Some(session_id), None) => Some(format!("source:{source}:session:{session_id}")),
        (None, Some(turn_id)) => Some(format!("source:{source}:turn:{turn_id}")),
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

fn discover_info_for_peer(config: &BridgeConfig, peer: SocketAddr) -> io::Result<DiscoveryInfo> {
    let port = bind_port(&config.bind).unwrap_or(8787);
    let local_ip = match peer.ip() {
        IpAddr::V4(ip) if !ip.is_loopback() => local_lan_ip_for_peer(IpAddr::V4(ip)),
        _ => local_lan_ip(),
    }
    .unwrap_or_else(|| "127.0.0.1".to_string());

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
    if let Some(ip) = configured_lan_ip() {
        return Some(ip);
    }

    [
        "192.168.1.1:80",
        "192.168.0.1:80",
        "10.0.0.1:80",
        "172.16.0.1:80",
        "8.8.8.8:80",
    ]
    .into_iter()
    .filter_map(|peer| peer.parse::<SocketAddr>().ok())
    .find_map(|peer| local_lan_ip_for_peer(peer.ip()))
}

fn local_lan_ip_for_peer(peer: IpAddr) -> Option<String> {
    let socket = UdpSocket::bind("0.0.0.0:0").ok()?;
    socket.connect(SocketAddr::new(peer, 80)).ok()?;
    let ip = socket.local_addr().ok()?.ip();
    match ip {
        IpAddr::V4(ip) if lan_discovery_ip_is_usable(ip) => Some(ip.to_string()),
        _ => None,
    }
}

fn configured_lan_ip() -> Option<String> {
    let ip = env_text("CODEX_ORNAMENT_LAN_IP")?;
    ip.parse::<Ipv4Addr>()
        .ok()
        .filter(|ip| lan_discovery_ip_is_usable(*ip))
        .map(|ip| ip.to_string())
}

fn lan_discovery_ip_is_usable(ip: Ipv4Addr) -> bool {
    let [first, second, ..] = ip.octets();
    let is_benchmark_or_proxy = first == 198 && matches!(second, 18 | 19);
    !ip.is_loopback()
        && !ip.is_unspecified()
        && !ip.is_link_local()
        && !ip.is_broadcast()
        && !ip.is_documentation()
        && !is_benchmark_or_proxy
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
    let source = event_source(payload);
    let status = match kind.as_str() {
        "UserPromptSubmit" => "running",
        "Stop" | "agent-turn-complete" => "done",
        "InvalidJson" => "error",
        _ => "event",
    }
    .to_string();
    let title = task_title_for(&status, source.as_deref());
    let message = event_message(payload).unwrap_or_else(|| kind.clone());

    TaskEvent {
        kind,
        status,
        title,
        message,
        received_at: now_local(),
        source,
        session_id: event_text_field(
            payload,
            &["session_id", "sessionId", "thread-id", "thread_id"],
        ),
        turn_id: event_text_field(payload, &["turn_id", "turnId", "turn-id"]),
        cwd: event_text_field(payload, &["cwd"]).map(|value| clip(&value, 120)),
        model: event_text_field(payload, &["model"]),
    }
}

fn event_source(payload: &Value) -> Option<String> {
    text_field(payload, &["source", "agent", "client"])
        .map(|value| value.trim().to_ascii_lowercase())
        .filter(|value| !value.is_empty())
}

fn task_title_for(status: &str, source: Option<&str>) -> String {
    let prefix = match source {
        Some(source) if source_is_claude(source) => "Claude",
        _ => "Codex",
    };
    match status {
        "running" => format!("{prefix} running"),
        "done" => format!("{prefix} done"),
        "error" => format!("{prefix} hook error"),
        _ => format!("{prefix} event"),
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

fn event_text_field(payload: &Value, keys: &[&str]) -> Option<String> {
    text_field(payload, keys).or_else(|| nested_event_text_field(payload, keys))
}

fn nested_event_text_field(payload: &Value, keys: &[&str]) -> Option<String> {
    ["message", "raw"].iter().find_map(|container_key| {
        let text = payload.get(*container_key)?.as_str()?.trim();
        text_field_from_json_text(text, keys)
            .or_else(|| text_field_from_partial_json_text(text, keys))
    })
}

fn text_field_from_json_text(text: &str, keys: &[&str]) -> Option<String> {
    let parsed = serde_json::from_str::<Value>(text).ok()?;
    text_field(&parsed, keys)
}

fn text_field_from_partial_json_text(text: &str, keys: &[&str]) -> Option<String> {
    keys.iter()
        .find_map(|key| quoted_json_string_value(text, key))
        .filter(|value| !value.is_empty())
}

fn quoted_json_string_value(text: &str, key: &str) -> Option<String> {
    let quoted_key = format!("\"{key}\"");
    let after_key = text.split_once(&quoted_key)?.1;
    let after_colon = after_key.trim_start().strip_prefix(':')?.trim_start();
    let mut chars = after_colon.chars();
    if chars.next()? != '"' {
        return None;
    }

    let mut value = String::new();
    let mut escaped = false;
    for ch in chars {
        if escaped {
            value.push(match ch {
                '"' => '"',
                '\\' => '\\',
                '/' => '/',
                'b' => '\u{0008}',
                'f' => '\u{000c}',
                'n' => '\n',
                'r' => '\r',
                't' => '\t',
                other => other,
            });
            escaped = false;
            continue;
        }

        match ch {
            '\\' => escaped = true,
            '"' => return Some(value),
            other => value.push(other),
        }
    }

    (!value.is_empty()).then_some(value)
}

fn terminal_turn_for_event(codex_home: &Path, event: &TaskEvent) -> Option<TerminalTurn> {
    let session_id = event.session_id.as_deref()?;
    let turn_id = event.turn_id.as_deref()?;
    let session_file = find_session_file(codex_home, session_id)?;
    terminal_turn_in_file(&session_file, turn_id).ok().flatten()
}

fn active_task_is_stale_against_session_log(codex_home: &Path, event: &TaskEvent) -> bool {
    if done_source(event) != DoneSource::Codex {
        return false;
    }

    let Some(session_id) = event.session_id.as_deref() else {
        return false;
    };
    let Some(turn_id) = event.turn_id.as_deref() else {
        return false;
    };

    let Some(session_file) = find_session_file(codex_home, session_id) else {
        return timestamp_is_older_than(&event.received_at, ACTIVE_SESSION_FILE_MISSING_GRACE);
    };

    if active_task_has_newer_turn_in_session_file(&session_file, event)
        .ok()
        .unwrap_or(false)
    {
        return true;
    }

    match active_task_in_session_file(&session_file, session_id) {
        Ok(Some(active)) => {
            active.turn_id.as_deref() != Some(turn_id)
                && timestamp_is_older_than(&event.received_at, ACTIVE_SESSION_FILE_MISSING_GRACE)
        }
        Ok(None) => timestamp_is_older_than(&event.received_at, ACTIVE_SESSION_FILE_MISSING_GRACE),
        Err(_) => false,
    }
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

fn active_tasks_in_recent_session_files(codex_home: &Path) -> Vec<TaskEvent> {
    recent_session_files(&codex_home.join("sessions"))
        .into_iter()
        .filter_map(|path| {
            let session_id = session_id_from_file_name(&path)?;
            let active = active_task_in_session_file(&path, &session_id)
                .ok()
                .flatten()?;
            let has_newer_turn = active_task_has_newer_turn_in_session_file(&path, &active)
                .ok()
                .unwrap_or(false);
            (!has_newer_turn).then_some(active)
        })
        .filter(|active| timestamp_is_recent(&active.received_at, RECOVER_ACTIVE_TASK_WINDOW))
        .collect()
}

fn session_ancestor_ids(codex_home: &Path, session_id: &str) -> Vec<String> {
    let mut ancestors = Vec::new();
    let mut current = session_id.to_string();

    for _ in 0..SESSION_FORK_CHAIN_LIMIT {
        let Some(parent) = session_fork_parent_id(codex_home, &current) else {
            break;
        };
        if parent.is_empty() || ancestors.iter().any(|ancestor| ancestor == &parent) {
            break;
        }

        current = parent.clone();
        ancestors.push(parent);
    }

    ancestors
}

fn session_fork_happened_after_event(
    codex_home: &Path,
    descendant_session_id: &str,
    event: &TaskEvent,
) -> bool {
    let Some(forked_at) = session_timestamp(codex_home, descendant_session_id) else {
        return false;
    };
    timestamp_is_before(&event.received_at, &forked_at)
}

fn session_fork_parent_id(codex_home: &Path, session_id: &str) -> Option<String> {
    let session_file = find_session_file(codex_home, session_id)?;
    session_fork_parent_id_in_file(&session_file)
}

fn session_timestamp(codex_home: &Path, session_id: &str) -> Option<String> {
    let session_file = find_session_file(codex_home, session_id)?;
    session_metadata_in_file(&session_file).and_then(|metadata| metadata.timestamp)
}

fn session_fork_parent_id_in_file(path: &Path) -> Option<String> {
    session_metadata_in_file(path).and_then(|metadata| metadata.forked_from_id)
}

struct SessionMetadata {
    forked_from_id: Option<String>,
    timestamp: Option<String>,
}

fn session_metadata_in_file(path: &Path) -> Option<SessionMetadata> {
    let file = open_shared_read(path).ok()?;
    let reader = BufReader::new(file);

    for line in reader.lines() {
        let line = line.ok()?;
        if !line.contains("\"session_meta\"") {
            continue;
        }
        let record = serde_json::from_str::<Value>(&line).ok()?;
        if record.get("type").and_then(Value::as_str) != Some("session_meta") {
            continue;
        }
        let payload = &record["payload"];
        return Some(SessionMetadata {
            forked_from_id: payload
                .get("forked_from_id")
                .and_then(Value::as_str)
                .map(str::to_string)
                .filter(|value| !value.is_empty()),
            timestamp: payload
                .get("timestamp")
                .and_then(Value::as_str)
                .map(str::to_string)
                .or_else(|| {
                    record
                        .get("timestamp")
                        .and_then(Value::as_str)
                        .map(str::to_string)
                }),
        });
    }

    None
}

fn line_may_contain_active_task_record(line: &str) -> bool {
    line.contains("\"task_started\"")
        || line.contains("\"turn_context\"")
        || line.contains("\"task_complete\"")
        || line.contains("\"turn_aborted\"")
        || line.contains("\"final_answer\"")
}

fn line_may_contain_terminal_task_record(line: &str) -> bool {
    line.contains("\"task_started\"")
        || line.contains("\"turn_context\"")
        || line.contains("\"task_complete\"")
        || line.contains("\"turn_aborted\"")
        || line.contains("\"final_answer\"")
}

fn recent_session_files(root: &Path) -> Vec<PathBuf> {
    let mut files = Vec::new();
    collect_session_files(root, &mut files);
    files.sort_by(|left, right| {
        file_modified_at(right)
            .cmp(&file_modified_at(left))
            .then_with(|| right.cmp(left))
    });
    files.truncate(RECOVER_ACTIVE_SESSION_SCAN_LIMIT);
    files
}

fn collect_session_files(root: &Path, files: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(root) else {
        return;
    };

    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            collect_session_files(&path, files);
        } else if path.extension().and_then(|extension| extension.to_str()) == Some("jsonl") {
            files.push(path);
        }
    }
}

fn file_modified_at(path: &Path) -> Option<std::time::SystemTime> {
    path.metadata()
        .and_then(|metadata| metadata.modified())
        .ok()
}

fn session_id_from_file_name(path: &Path) -> Option<String> {
    let file_name = path.file_name()?.to_str()?;
    let stem = file_name.strip_suffix(".jsonl")?;
    let session_id = stem.get(stem.len().checked_sub(36)?..)?;
    let looks_like_uuid = session_id.len() == 36
        && session_id.chars().filter(|ch| *ch == '-').count() == 4
        && session_id
            .chars()
            .all(|ch| ch.is_ascii_hexdigit() || ch == '-');
    looks_like_uuid.then(|| session_id.to_string())
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

fn open_shared_read(path: &Path) -> io::Result<File> {
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        const FILE_SHARE_READ: u32 = 0x00000001;
        const FILE_SHARE_WRITE: u32 = 0x00000002;
        const FILE_SHARE_DELETE: u32 = 0x00000004;

        fs::OpenOptions::new()
            .read(true)
            .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
            .open(path)
    }

    #[cfg(not(windows))]
    {
        File::open(path)
    }
}

fn open_shared_tail_read(path: &Path, max_bytes: u64) -> io::Result<File> {
    let mut file = open_shared_read(path)?;
    let length = file.metadata()?.len();
    if length > max_bytes {
        file.seek(SeekFrom::Start(length - max_bytes))?;
    }
    Ok(file)
}

fn active_task_in_session_file(path: &Path, session_id: &str) -> io::Result<Option<TaskEvent>> {
    Ok(active_tasks_in_session_file(path, session_id)?.pop())
}

fn active_tasks_in_session_file(path: &Path, session_id: &str) -> io::Result<Vec<TaskEvent>> {
    let file = open_shared_tail_read(path, SESSION_TASK_SCAN_TAIL_BYTES)?;
    let reader = BufReader::new(file);
    let mut active: HashMap<String, TaskEvent> = HashMap::new();
    let mut active_order = VecDeque::new();
    let mut pending_final_answer = None;

    for line in reader.lines() {
        let line = line?;
        if !line_may_contain_active_task_record(&line) {
            continue;
        }
        let Ok(record) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        let payload = &record["payload"];
        if record_is_final_answer(&record, payload) {
            pending_final_answer =
                remember_latest_pending_final_answer(&active, &active_order, &record, payload);
            continue;
        }

        let Some(kind) = session_record_kind(&record, payload) else {
            continue;
        };

        match kind {
            "task_started" => {
                let Some(turn_id) = payload.get("turn_id").and_then(Value::as_str) else {
                    continue;
                };
                if should_finalize_pending_final_answer(
                    pending_final_answer.as_ref(),
                    Some(turn_id),
                    kind,
                ) {
                    apply_pending_final_answer(
                        &mut active,
                        &mut active_order,
                        pending_final_answer.take(),
                    );
                }
                if !active.contains_key(turn_id) {
                    active_order.push_back(turn_id.to_string());
                }
                active.insert(
                    turn_id.to_string(),
                    TaskEvent {
                        kind: "UserPromptSubmit".to_string(),
                        status: "running".to_string(),
                        title: "Codex running".to_string(),
                        message: "Codex running".to_string(),
                        received_at: record
                            .get("timestamp")
                            .and_then(Value::as_str)
                            .map(str::to_string)
                            .unwrap_or_else(now_local),
                        source: None,
                        session_id: Some(session_id.to_string()),
                        turn_id: Some(turn_id.to_string()),
                        cwd: None,
                        model: None,
                    },
                );
            }
            "turn_context" => {
                let Some(turn_id) = payload.get("turn_id").and_then(Value::as_str) else {
                    continue;
                };
                if pending_final_answer_turn_matches(pending_final_answer.as_ref(), turn_id) {
                    pending_final_answer = None;
                }
                if let Some(event) = active.get_mut(turn_id) {
                    event.cwd = text_field(payload, &["cwd"]).map(|value| clip(&value, 120));
                    event.model = text_field(payload, &["model"]);
                }
            }
            "task_complete" | "turn_aborted" => {
                if let Some(turn_id) = payload.get("turn_id").and_then(Value::as_str) {
                    if should_finalize_pending_final_answer(
                        pending_final_answer.as_ref(),
                        Some(turn_id),
                        kind,
                    ) {
                        apply_pending_final_answer(
                            &mut active,
                            &mut active_order,
                            pending_final_answer.take(),
                        );
                    }
                    active.remove(turn_id);
                    active_order.retain(|candidate| candidate != turn_id);
                }
            }
            _ => {}
        }
    }

    apply_pending_final_answer(&mut active, &mut active_order, pending_final_answer);

    Ok(active_order
        .into_iter()
        .filter_map(|turn_id| active.remove(&turn_id))
        .collect())
}

fn terminal_turn_in_file(path: &Path, turn_id: &str) -> io::Result<Option<TerminalTurn>> {
    let file = open_shared_tail_read(path, SESSION_TASK_SCAN_TAIL_BYTES)?;
    let reader = BufReader::new(file);
    let mut terminal = None;
    let mut active_order = VecDeque::new();
    let mut pending_final_answer = None;

    for line in reader.lines() {
        let line = line?;
        if !line_may_contain_terminal_task_record(&line) {
            continue;
        }
        let Ok(record) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        let payload = &record["payload"];
        if record_is_final_answer(&record, payload) {
            pending_final_answer =
                remember_latest_pending_final_answer_id(&active_order, &record, payload);
            continue;
        }

        let Some(kind) = session_record_kind(&record, payload) else {
            continue;
        };

        if kind == "task_started" {
            if let Some(started_turn_id) = payload.get("turn_id").and_then(Value::as_str) {
                if should_finalize_pending_final_answer(
                    pending_final_answer.as_ref(),
                    Some(started_turn_id),
                    kind,
                ) {
                    remember_terminal_from_pending_final_answer(
                        &mut terminal,
                        pending_final_answer.take(),
                        turn_id,
                    );
                    remove_latest_active_turn_id(&mut active_order);
                }
                remember_active_turn_id(&mut active_order, started_turn_id);
            }
            continue;
        }

        if kind == "turn_context" {
            if let Some(context_turn_id) = payload.get("turn_id").and_then(Value::as_str) {
                if pending_final_answer_turn_matches(pending_final_answer.as_ref(), context_turn_id)
                {
                    pending_final_answer = None;
                }
            }
            continue;
        }

        if kind != "task_complete" && kind != "turn_aborted" {
            continue;
        }
        let Some(terminal_turn_id) = payload.get("turn_id").and_then(Value::as_str) else {
            continue;
        };
        if should_finalize_pending_final_answer(
            pending_final_answer.as_ref(),
            Some(terminal_turn_id),
            kind,
        ) {
            remember_terminal_from_pending_final_answer(
                &mut terminal,
                pending_final_answer.take(),
                turn_id,
            );
            remove_latest_active_turn_id(&mut active_order);
        }
        forget_active_turn_id(&mut active_order, terminal_turn_id);
        if terminal_turn_id != turn_id {
            continue;
        }

        let previous_message = terminal.as_ref().and_then(|terminal: &TerminalTurn| {
            terminal
                .message
                .as_deref()
                .filter(|message| !message.is_empty())
                .map(str::to_string)
        });

        terminal = Some(TerminalTurn {
            kind: kind.to_string(),
            timestamp: record
                .get("timestamp")
                .and_then(Value::as_str)
                .map(str::to_string),
            message: terminal_message(payload).or(previous_message),
        });
    }

    remember_terminal_from_pending_final_answer(&mut terminal, pending_final_answer, turn_id);

    Ok(terminal)
}

fn active_task_has_newer_turn_in_session_file(path: &Path, event: &TaskEvent) -> io::Result<bool> {
    let Some(turn_id) = event.turn_id.as_deref() else {
        return Ok(false);
    };

    let file = open_shared_tail_read(path, SESSION_TASK_SCAN_TAIL_BYTES)?;
    let reader = BufReader::new(file);
    let mut saw_event_turn = false;

    for line in reader.lines() {
        let line = line?;
        if !line.contains("\"task_started\"") {
            continue;
        }
        let Ok(record) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        let payload = &record["payload"];
        if session_record_kind(&record, payload) != Some("task_started") {
            continue;
        }
        let Some(started_turn_id) = payload.get("turn_id").and_then(Value::as_str) else {
            continue;
        };
        if saw_event_turn && started_turn_id != turn_id {
            return Ok(true);
        }
        if started_turn_id == turn_id {
            saw_event_turn = true;
        }
    }

    Ok(false)
}

fn record_is_final_answer(record: &Value, payload: &Value) -> bool {
    record.get("type").and_then(Value::as_str) == Some("response_item")
        && payload.get("phase").and_then(Value::as_str) == Some("final_answer")
}

fn session_record_kind<'a>(record: &'a Value, payload: &'a Value) -> Option<&'a str> {
    payload
        .get("type")
        .and_then(Value::as_str)
        .or_else(|| record.get("type").and_then(Value::as_str))
}

fn terminal_message(payload: &Value) -> Option<String> {
    text_field(payload, &["last_agent_message", "message", "reason"])
        .map(|value| clip(value.trim(), 160))
        .filter(|value| !value.is_empty())
}

fn final_answer_message(payload: &Value) -> Option<String> {
    payload
        .get("content")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(|item| item.get("text").and_then(Value::as_str))
        .find_map(|text| {
            let text = text.trim();
            (!text.is_empty()).then(|| clip(text, 160))
        })
}

fn remember_latest_pending_final_answer(
    active: &HashMap<String, TaskEvent>,
    active_order: &VecDeque<String>,
    record: &Value,
    payload: &Value,
) -> Option<PendingFinalAnswer> {
    let turn_id = active_order
        .iter()
        .rev()
        .find(|candidate| active.contains_key(candidate.as_str()))?
        .clone();
    Some(PendingFinalAnswer {
        turn_id,
        timestamp: record
            .get("timestamp")
            .and_then(Value::as_str)
            .map(str::to_string),
        message: final_answer_message(payload),
    })
}

fn remember_latest_pending_final_answer_id(
    active_order: &VecDeque<String>,
    record: &Value,
    payload: &Value,
) -> Option<PendingFinalAnswer> {
    Some(PendingFinalAnswer {
        turn_id: active_order.back()?.clone(),
        timestamp: record
            .get("timestamp")
            .and_then(Value::as_str)
            .map(str::to_string),
        message: final_answer_message(payload),
    })
}

fn pending_final_answer_turn_matches(
    pending_final_answer: Option<&PendingFinalAnswer>,
    turn_id: &str,
) -> bool {
    pending_final_answer
        .map(|pending| pending.turn_id == turn_id)
        .unwrap_or(false)
}

fn should_finalize_pending_final_answer(
    pending_final_answer: Option<&PendingFinalAnswer>,
    turn_id: Option<&str>,
    kind: &str,
) -> bool {
    let Some(pending) = pending_final_answer else {
        return false;
    };
    match kind {
        "task_started" => turn_id.map(|turn_id| turn_id != pending.turn_id).unwrap_or(true),
        "task_complete" | "turn_aborted" => {
            turn_id.map(|turn_id| turn_id == pending.turn_id).unwrap_or(false)
        }
        _ => false,
    }
}

fn apply_pending_final_answer(
    active: &mut HashMap<String, TaskEvent>,
    active_order: &mut VecDeque<String>,
    pending_final_answer: Option<PendingFinalAnswer>,
) {
    let Some(pending) = pending_final_answer else {
        return;
    };
    active.remove(&pending.turn_id);
    active_order.retain(|candidate| candidate != &pending.turn_id);
}

fn remember_terminal_from_pending_final_answer(
    terminal: &mut Option<TerminalTurn>,
    pending_final_answer: Option<PendingFinalAnswer>,
    turn_id: &str,
) {
    let Some(pending) = pending_final_answer else {
        return;
    };
    if pending.turn_id != turn_id {
        return;
    }
    *terminal = Some(TerminalTurn {
        kind: "task_complete".to_string(),
        timestamp: pending.timestamp,
        message: pending.message,
    });
}

fn remember_active_turn_id(active_order: &mut VecDeque<String>, turn_id: &str) {
    if !active_order.iter().any(|candidate| candidate == turn_id) {
        active_order.push_back(turn_id.to_string());
    }
}

fn forget_active_turn_id(active_order: &mut VecDeque<String>, turn_id: &str) {
    active_order.retain(|candidate| candidate != turn_id);
}

fn remove_latest_active_turn_id(active_order: &mut VecDeque<String>) {
    active_order.pop_back();
}

fn timestamp_is_recent(timestamp: &str, window: Duration) -> bool {
    let Ok(timestamp) = DateTime::parse_from_rfc3339(timestamp) else {
        return false;
    };
    let age = Local::now().signed_duration_since(timestamp.with_timezone(&Local));
    age.to_std().map(|age| age <= window).unwrap_or(false)
}

fn timestamp_is_older_than(timestamp: &str, window: Duration) -> bool {
    let Ok(timestamp) = DateTime::parse_from_rfc3339(timestamp) else {
        return false;
    };
    let age = Local::now().signed_duration_since(timestamp.with_timezone(&Local));
    age.to_std().map(|age| age > window).unwrap_or(false)
}

fn timestamp_is_before(left: &str, right: &str) -> bool {
    let Ok(left) = DateTime::parse_from_rfc3339(left) else {
        return false;
    };
    let Ok(right) = DateTime::parse_from_rfc3339(right) else {
        return false;
    };
    left < right
}

fn cached_or_refresh_quota(state: &Arc<Mutex<BridgeState>>) -> QuotaSnapshot {
    if let Ok(state) = state.lock() {
        if let Some(cache) = state.quota.as_ref() {
            if cache.fetched_at.elapsed() < QUOTA_CACHE_TTL {
                return cache.snapshot.clone();
            }
        }
    }

    store_quota_snapshot(state, get_quota_snapshot())
}

fn cached_or_refresh_quota_background(state: &Arc<Mutex<BridgeState>>) -> QuotaSnapshot {
    if let Ok(state) = state.lock() {
        if let Some(cache) = state.quota.as_ref() {
            if cache.fetched_at.elapsed() < QUOTA_CACHE_TTL {
                return cache.snapshot.clone();
            }
        }
    }

    maybe_spawn_quota_refresh(Arc::clone(state));

    if let Ok(state) = state.lock() {
        if let Some(cache) = state.quota.as_ref() {
            return cache.snapshot.clone();
        }
    }
    quota_unavailable()
}

fn maybe_spawn_quota_refresh(state: SharedBridgeState) {
    let should_spawn = {
        let Ok(mut state) = state.lock() else {
            return;
        };
        if state.quota_refreshing {
            false
        } else {
            state.quota_refreshing = true;
            true
        }
    };
    if !should_spawn {
        return;
    }

    std::thread::spawn(move || {
        store_quota_snapshot(&state, get_quota_snapshot());
    });
}

fn maybe_spawn_quota_refresh_if_due(state: SharedBridgeState) {
    let refresh_due = {
        let Ok(state) = state.lock() else {
            return;
        };
        quota_refresh_is_due(&state)
    };
    if refresh_due {
        maybe_spawn_quota_refresh(state);
    }
}

fn quota_refresh_is_due(state: &BridgeState) -> bool {
    if state.quota_refreshing {
        return false;
    }
    state
        .quota
        .as_ref()
        .map(|cache| cache.fetched_at.elapsed() >= QUOTA_CACHE_TTL)
        .unwrap_or(true)
}

fn quota_unavailable() -> QuotaSnapshot {
    QuotaSnapshot {
        status: SnapshotStatus::NoData,
        source: "codex-wham".to_string(),
        source_label: Some("ChatGPT usage API".to_string()),
        web_url: Some(quota_core::USAGE_URL.to_string()),
        limit_id: Some("codex".to_string()),
        plan_type: None,
        primary_used_percent: None,
        primary_remaining_percent: None,
        primary_window_minutes: None,
        primary_resets_at: None,
        secondary_used_percent: None,
        secondary_remaining_percent: None,
        secondary_window_minutes: None,
        secondary_resets_at: None,
        credits: None,
        observed_at: Some(now_local()),
        captured_at: None,
        error: Some("quota refresh pending".to_string()),
    }
}

fn store_quota_snapshot(
    state: &Arc<Mutex<BridgeState>>,
    fetched_snapshot: QuotaSnapshot,
) -> QuotaSnapshot {
    let previous_snapshot = cached_quota_snapshot(state).or_else(load_persisted_quota_snapshot);
    let snapshot = merge_quota_snapshot(previous_snapshot.as_ref(), fetched_snapshot);
    let _ = write_state(state_path(), &snapshot);
    if let Ok(mut state) = state.lock() {
        state.quota = Some(CachedQuota {
            snapshot: snapshot.clone(),
            fetched_at: Instant::now(),
        });
        state.quota_refreshing = false;
    }
    snapshot
}

fn cached_quota_snapshot(state: &Arc<Mutex<BridgeState>>) -> Option<QuotaSnapshot> {
    state
        .lock()
        .ok()
        .and_then(|state| state.quota.as_ref().map(|cache| cache.snapshot.clone()))
}

fn load_persisted_quota_snapshot() -> Option<QuotaSnapshot> {
    read_state(state_path())
        .ok()
        .flatten()
        .filter(quota_snapshot_has_display_data)
}

fn merge_quota_snapshot(
    previous: Option<&QuotaSnapshot>,
    mut fetched: QuotaSnapshot,
) -> QuotaSnapshot {
    let Some(previous) = previous.filter(|snapshot| quota_snapshot_has_display_data(snapshot))
    else {
        return fetched;
    };

    if !quota_snapshot_has_actual_data(&fetched) {
        return previous.clone();
    }

    fetched.source_label = fetched
        .source_label
        .or_else(|| previous.source_label.clone());
    fetched.web_url = fetched.web_url.or_else(|| previous.web_url.clone());
    fetched.limit_id = fetched.limit_id.or_else(|| previous.limit_id.clone());
    fetched.plan_type = fetched.plan_type.or_else(|| previous.plan_type.clone());
    fetched.primary_used_percent = fetched
        .primary_used_percent
        .or(previous.primary_used_percent);
    fetched.primary_remaining_percent = fetched
        .primary_remaining_percent
        .or(previous.primary_remaining_percent);
    fetched.primary_window_minutes = fetched
        .primary_window_minutes
        .or(previous.primary_window_minutes);
    fetched.primary_resets_at = merged_reset_time(
        previous.primary_resets_at.as_deref(),
        fetched.primary_resets_at,
    );
    fetched.secondary_used_percent = fetched
        .secondary_used_percent
        .or(previous.secondary_used_percent);
    fetched.secondary_remaining_percent = fetched
        .secondary_remaining_percent
        .or(previous.secondary_remaining_percent);
    fetched.secondary_window_minutes = fetched
        .secondary_window_minutes
        .or(previous.secondary_window_minutes);
    fetched.secondary_resets_at = merged_reset_time(
        previous.secondary_resets_at.as_deref(),
        fetched.secondary_resets_at,
    );
    fetched.credits = fetched.credits.or_else(|| previous.credits.clone());
    fetched
}

fn merged_reset_time(previous: Option<&str>, fetched: Option<String>) -> Option<String> {
    if let Some(previous) = previous {
        if !timestamp_has_passed(previous) {
            return Some(previous.to_string());
        }
    }
    fetched.or_else(|| previous.map(str::to_string))
}

fn timestamp_has_passed(timestamp: &str) -> bool {
    let Ok(timestamp) = DateTime::parse_from_rfc3339(timestamp) else {
        return true;
    };
    timestamp.with_timezone(&Local) <= Local::now()
}

fn quota_snapshot_has_actual_data(snapshot: &QuotaSnapshot) -> bool {
    snapshot.status == SnapshotStatus::Ok && quota_snapshot_has_display_data(snapshot)
}

fn quota_snapshot_has_display_data(snapshot: &QuotaSnapshot) -> bool {
    snapshot.primary_remaining_percent.is_some()
        || snapshot.primary_used_percent.is_some()
        || snapshot.primary_resets_at.is_some()
        || snapshot.secondary_remaining_percent.is_some()
        || snapshot.secondary_used_percent.is_some()
        || snapshot.secondary_resets_at.is_some()
}

fn cached_or_refresh_weather(
    state: &Arc<Mutex<BridgeState>>,
    config: &BridgeConfig,
) -> WeatherSnapshot {
    if let Ok(state) = state.lock() {
        if let Some(cache) = state.weather.as_ref() {
            if cache.fetched_at.elapsed() < WEATHER_CACHE_TTL {
                return cache.snapshot.clone();
            }
        }
    }

    maybe_spawn_weather_refresh(Arc::clone(state), config.clone());

    if let Ok(state) = state.lock() {
        if let Some(cache) = state.weather.as_ref() {
            return cache.snapshot.clone();
        }
    }
    weather_unavailable(config)
}

fn maybe_spawn_weather_refresh(state: SharedBridgeState, config: BridgeConfig) {
    let should_spawn = {
        let Ok(mut state) = state.lock() else {
            return;
        };
        if state.weather_refreshing {
            false
        } else {
            state.weather_refreshing = true;
            true
        }
    };
    if !should_spawn {
        return;
    }

    std::thread::spawn(move || {
        let fetched = fetch_weather_snapshot(&config);
        let Ok(mut state) = state.lock() else {
            return;
        };
        match fetched {
            Ok(snapshot) => {
                state.weather = Some(CachedWeather {
                    snapshot,
                    fetched_at: Instant::now(),
                });
            }
            Err(error) => {
                eprintln!("weather fetch failed: {error}");
            }
        }
        state.weather_refreshing = false;
    });
}

fn fetch_weather_snapshot(config: &BridgeConfig) -> io::Result<WeatherSnapshot> {
    let client = reqwest::blocking::Client::builder()
        .timeout(WEATHER_FETCH_TIMEOUT)
        .build()
        .map_err(io_other)?;
    let mut last_error = None;

    for provider in weather_fetch_order(config) {
        let fetched = match provider {
            WeatherProvider::Auto => unreachable!("auto is expanded by weather_fetch_order"),
            WeatherProvider::OpenMeteo => fetch_open_meteo_weather(&client, config),
            WeatherProvider::QWeather => fetch_qweather_weather(&client, config),
            WeatherProvider::Caiyun => fetch_caiyun_weather(&client, config),
        };
        match fetched {
            Ok(snapshot) => return Ok(snapshot),
            Err(error) => {
                eprintln!(
                    "weather provider {} failed: {error}",
                    weather_provider_name(provider)
                );
                last_error = Some(error);
            }
        }
    }

    Err(last_error.unwrap_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "no weather provider configured",
        )
    }))
}

fn weather_fetch_order(config: &BridgeConfig) -> Vec<WeatherProvider> {
    match config.weather_provider {
        WeatherProvider::Auto => {
            let mut providers = Vec::new();
            if config.caiyun_token.is_some() {
                providers.push(WeatherProvider::Caiyun);
            }
            if config.qweather_host.is_some() && config.qweather_token.is_some() {
                providers.push(WeatherProvider::QWeather);
            }
            providers.push(WeatherProvider::OpenMeteo);
            providers
        }
        WeatherProvider::OpenMeteo => vec![WeatherProvider::OpenMeteo],
        WeatherProvider::QWeather => vec![WeatherProvider::QWeather, WeatherProvider::OpenMeteo],
        WeatherProvider::Caiyun => vec![WeatherProvider::Caiyun, WeatherProvider::OpenMeteo],
    }
}

fn fetch_open_meteo_weather(
    client: &reqwest::blocking::Client,
    config: &BridgeConfig,
) -> io::Result<WeatherSnapshot> {
    let url = open_meteo_url(config.weather_latitude, config.weather_longitude);
    let response = get_weather_json::<OpenMeteoResponse>(client, url, WeatherProvider::OpenMeteo)?;
    let Some(current) = response.current else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Open-Meteo response missing current weather",
        ));
    };

    let temperature_c = current.temperature_2m.map(round_f64_to_i32);
    let wind_kmh = current.wind_speed_10m.map(round_f64_to_i32);
    let summary = current
        .weather_code
        .map(open_meteo_weather_summary_for_code)
        .unwrap_or("WEATHER")
        .to_string();
    let icon = current
        .weather_code
        .map(open_meteo_weather_icon_for_code)
        .unwrap_or("unknown")
        .to_string();

    Ok(WeatherSnapshot {
        status: "ok".to_string(),
        label: config.weather_label.clone(),
        summary,
        icon,
        temperature_c,
        wind_kmh,
        weather_code: current.weather_code,
        observed_at: now_local(),
    })
}

fn fetch_qweather_weather(
    client: &reqwest::blocking::Client,
    config: &BridgeConfig,
) -> io::Result<WeatherSnapshot> {
    let host = config.qweather_host.as_deref().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "CODEX_ORNAMENT_QWEATHER_HOST is required for qweather",
        )
    })?;
    let token = config.qweather_token.as_deref().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "CODEX_ORNAMENT_QWEATHER_TOKEN is required for qweather",
        )
    })?;
    let url = qweather_url(host, config.weather_latitude, config.weather_longitude);
    let response = client
        .get(url)
        .bearer_auth(token)
        .send()
        .map_err(|error| weather_request_error(WeatherProvider::QWeather, error))?;
    let response = response
        .error_for_status()
        .map_err(|error| weather_status_error(WeatherProvider::QWeather, error))?
        .json::<QWeatherResponse>()
        .map_err(|_| weather_parse_error(WeatherProvider::QWeather))?;
    if response.code != "200" {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("QWeather response code {}", response.code),
        ));
    };
    let Some(now) = response.now else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "QWeather response missing now weather",
        ));
    };

    let summary = qweather_summary(&now).to_string();
    let icon = qweather_icon(&summary, now.icon.as_deref()).to_string();

    Ok(WeatherSnapshot {
        status: "ok".to_string(),
        label: config.weather_label.clone(),
        summary,
        icon,
        temperature_c: now.temp.as_deref().and_then(parse_i32_text),
        wind_kmh: now.wind_speed.as_deref().and_then(parse_i32_text),
        weather_code: now.icon.as_deref().and_then(parse_i32_text),
        observed_at: now.obs_time.unwrap_or_else(now_local),
    })
}

fn fetch_caiyun_weather(
    client: &reqwest::blocking::Client,
    config: &BridgeConfig,
) -> io::Result<WeatherSnapshot> {
    let token = config.caiyun_token.as_deref().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "CODEX_ORNAMENT_CAIYUN_TOKEN is required for caiyun",
        )
    })?;
    let url = caiyun_realtime_url(token, config.weather_latitude, config.weather_longitude);
    let response = get_weather_json::<CaiyunResponse>(client, url, WeatherProvider::Caiyun)?;
    if response.status != "ok" {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("Caiyun response status {}", response.status),
        ));
    }
    let Some(realtime) = response.result.and_then(|result| result.realtime) else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Caiyun response missing realtime weather",
        ));
    };
    let skycon = realtime.skycon.unwrap_or_else(|| "UNKNOWN".to_string());

    Ok(WeatherSnapshot {
        status: "ok".to_string(),
        label: config.weather_label.clone(),
        summary: caiyun_summary_for_skycon(&skycon).to_string(),
        icon: caiyun_icon_for_skycon(&skycon).to_string(),
        temperature_c: realtime.temperature.map(round_f64_to_i32),
        wind_kmh: realtime
            .wind
            .and_then(|wind| wind.speed)
            .map(round_f64_to_i32),
        weather_code: None,
        observed_at: now_local(),
    })
}

fn open_meteo_url(latitude: f64, longitude: f64) -> String {
    format!(
        "https://api.open-meteo.com/v1/forecast?latitude={latitude:.4}&longitude={longitude:.4}&current=temperature_2m,weather_code,wind_speed_10m&timezone=auto"
    )
}

fn qweather_url(host: &str, latitude: f64, longitude: f64) -> String {
    let host = host.trim_end_matches('/');
    format!("{host}/v7/weather/now?location={longitude:.6},{latitude:.6}")
}

fn caiyun_realtime_url(token: &str, latitude: f64, longitude: f64) -> String {
    format!("https://api.caiyunapp.com/v2.6/{token}/{longitude:.6},{latitude:.6}/realtime")
}

fn get_weather_json<T: for<'de> Deserialize<'de>>(
    client: &reqwest::blocking::Client,
    url: String,
    provider: WeatherProvider,
) -> io::Result<T> {
    let response = client
        .get(url)
        .send()
        .map_err(|error| weather_request_error(provider, error))?;
    response
        .error_for_status()
        .map_err(|error| weather_status_error(provider, error))?
        .json::<T>()
        .map_err(|_| weather_parse_error(provider))
}

fn weather_request_error(provider: WeatherProvider, error: reqwest::Error) -> io::Error {
    io::Error::other(format!(
        "{} weather request failed: {error}",
        weather_provider_name(provider)
    ))
}

fn weather_status_error(provider: WeatherProvider, error: reqwest::Error) -> io::Error {
    io::Error::other(format!(
        "{} weather HTTP status failed: {error}",
        weather_provider_name(provider)
    ))
}

fn weather_parse_error(provider: WeatherProvider) -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidData,
        format!(
            "{} weather response parse failed",
            weather_provider_name(provider)
        ),
    )
}

fn weather_provider_name(provider: WeatherProvider) -> &'static str {
    match provider {
        WeatherProvider::Auto => "auto",
        WeatherProvider::OpenMeteo => "openmeteo",
        WeatherProvider::QWeather => "qweather",
        WeatherProvider::Caiyun => "caiyun",
    }
}

fn weather_unavailable(config: &BridgeConfig) -> WeatherSnapshot {
    WeatherSnapshot {
        status: "unavailable".to_string(),
        label: config.weather_label.clone(),
        summary: "WEATHER --".to_string(),
        icon: "unknown".to_string(),
        temperature_c: None,
        wind_kmh: None,
        weather_code: None,
        observed_at: now_local(),
    }
}

fn round_f64_to_i32(value: f64) -> i32 {
    if value.is_finite() {
        value.round() as i32
    } else {
        0
    }
}

fn open_meteo_weather_summary_for_code(code: i32) -> &'static str {
    match code {
        0 => "CLEAR",
        1 | 2 => "PARTLY CLOUDY",
        3 => "CLOUDY",
        45 | 48 => "FOG",
        51 | 53 | 55 | 56 | 57 => "DRIZZLE",
        61 | 63 | 65 | 66 | 67 | 80 | 81 | 82 => "RAIN",
        71 | 73 | 75 | 77 | 85 | 86 => "SNOW",
        95 | 96 | 99 => "STORM",
        _ => "WEATHER",
    }
}

fn open_meteo_weather_icon_for_code(code: i32) -> &'static str {
    match code {
        0 => "sun",
        1 | 2 => "partly-cloudy",
        3 => "cloud",
        45 | 48 => "fog",
        51 | 53 | 55 | 80 | 81 => "drizzle",
        56 | 57 | 66 | 67 | 77 => "sleet",
        61 | 63 | 65 => "rain",
        82 => "heavy-rain",
        71 | 73 | 75 | 85 | 86 => "snow",
        95 | 96 | 99 => "storm",
        _ => "unknown",
    }
}

fn qweather_summary(now: &QWeatherNow) -> &str {
    now.text.as_deref().unwrap_or("WEATHER")
}

fn qweather_icon(summary: &str, icon: Option<&str>) -> &'static str {
    if let Some(icon) = icon {
        if let Ok(code) = icon.parse::<i32>() {
            return match code {
                100 => "sun",
                101 => "partly-cloudy",
                102..=104 => "cloud",
                150 => "sun",
                151..=153 => "partly-cloudy",
                154 => "cloud",
                300..=304 => "drizzle",
                305..=309 => "rain",
                310..=315 => "heavy-rain",
                400..=405 => "snow",
                406..=409 => "sleet",
                500..=515 => "fog",
                _ => "unknown",
            };
        }
    }
    compact_weather_icon(summary)
}

fn caiyun_summary_for_skycon(skycon: &str) -> &'static str {
    match skycon {
        "CLEAR_DAY" | "CLEAR_NIGHT" => "CLEAR",
        "PARTLY_CLOUDY_DAY" | "PARTLY_CLOUDY_NIGHT" => "PARTLY CLOUDY",
        "CLOUDY" => "CLOUDY",
        "LIGHT_HAZE" | "MODERATE_HAZE" | "HEAVY_HAZE" | "FOG" => "FOG",
        "LIGHT_RAIN" | "MODERATE_RAIN" | "HEAVY_RAIN" | "STORM_RAIN" => "RAIN",
        "LIGHT_SNOW" | "MODERATE_SNOW" | "HEAVY_SNOW" | "STORM_SNOW" => "SNOW",
        "DUST" | "SAND" | "WIND" => "WIND",
        _ => "WEATHER",
    }
}

fn caiyun_icon_for_skycon(skycon: &str) -> &'static str {
    match skycon {
        "CLEAR_DAY" | "CLEAR_NIGHT" => "sun",
        "PARTLY_CLOUDY_DAY" | "PARTLY_CLOUDY_NIGHT" => "partly-cloudy",
        "CLOUDY" => "cloud",
        "LIGHT_HAZE" | "MODERATE_HAZE" | "HEAVY_HAZE" | "DUST" | "SAND" => "haze",
        "FOG" => "fog",
        "LIGHT_RAIN" => "drizzle",
        "MODERATE_RAIN" => "rain",
        "HEAVY_RAIN" | "STORM_RAIN" => "heavy-rain",
        "LIGHT_SNOW" | "MODERATE_SNOW" | "HEAVY_SNOW" | "STORM_SNOW" => "snow",
        "WIND" => "windy",
        _ => "unknown",
    }
}

fn compact_weather_icon(summary: &str) -> &'static str {
    let summary = summary.to_ascii_lowercase();
    if summary.contains("晴") || summary.contains("clear") || summary.contains("sun") {
        "sun"
    } else if summary.contains("多云") && !summary.contains("阴") || summary.contains("partly") {
        "partly-cloudy"
    } else if summary.contains("云") || summary.contains("阴") || summary.contains("cloud") {
        "cloud"
    } else if summary.contains("霾")
        || summary.contains("尘")
        || summary.contains("沙")
        || summary.contains("haze")
        || summary.contains("dust")
        || summary.contains("sand")
    {
        "haze"
    } else if summary.contains("雾") || summary.contains("fog") {
        "fog"
    } else if summary.contains("毛毛雨") || summary.contains("drizzle") {
        "drizzle"
    } else if summary.contains("冻雨") || summary.contains("雨夹雪") || summary.contains("sleet")
    {
        "sleet"
    } else if summary.contains("暴雨") || summary.contains("大暴雨") || summary.contains("heavy")
    {
        "heavy-rain"
    } else if summary.contains("雨") || summary.contains("rain") {
        "rain"
    } else if summary.contains("雪") || summary.contains("snow") {
        "snow"
    } else if summary.contains("风") || summary.contains("wind") {
        "windy"
    } else {
        "unknown"
    }
}

fn parse_i32_text(value: &str) -> Option<i32> {
    value.parse::<f64>().ok().map(round_f64_to_i32)
}

fn io_other(error: impl std::error::Error + Send + Sync + 'static) -> io::Error {
    io::Error::other(error)
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
        202 => "Accepted",
        204 => "No Content",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        500 => "Internal Server Error",
        503 => "Service Unavailable",
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

fn env_f64(name: &str) -> Option<f64> {
    env_text(name).and_then(|value| value.parse::<f64>().ok())
}

fn env_weather_provider(name: &str) -> Option<WeatherProvider> {
    env_text(name).and_then(|value| parse_weather_provider(&value))
}

fn parse_weather_provider(value: &str) -> Option<WeatherProvider> {
    match value.trim().to_ascii_lowercase().as_str() {
        "auto" => Some(WeatherProvider::Auto),
        "openmeteo" | "open-meteo" => Some(WeatherProvider::OpenMeteo),
        "qweather" | "q-weather" | "heweather" => Some(WeatherProvider::QWeather),
        "caiyun" | "caiyunapp" => Some(WeatherProvider::Caiyun),
        _ => None,
    }
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
            weather_latitude: DEFAULT_WEATHER_LATITUDE,
            weather_longitude: DEFAULT_WEATHER_LONGITUDE,
            weather_label: DEFAULT_WEATHER_LABEL.to_string(),
            weather_provider: DEFAULT_WEATHER_PROVIDER,
            qweather_host: None,
            qweather_token: None,
            caiyun_token: None,
        }
    }

    fn scoped_test_config(session_id: &str, codex_home: impl Into<PathBuf>) -> BridgeConfig {
        BridgeConfig {
            bind: "127.0.0.1:8787".to_string(),
            token: None,
            tracked_session_id: Some(session_id.to_string()),
            codex_home: codex_home.into(),
            weather_latitude: DEFAULT_WEATHER_LATITUDE,
            weather_longitude: DEFAULT_WEATHER_LONGITUDE,
            weather_label: DEFAULT_WEATHER_LABEL.to_string(),
            weather_provider: DEFAULT_WEATHER_PROVIDER,
            qweather_host: None,
            qweather_token: None,
            caiyun_token: None,
        }
    }

    fn recent_timestamp(seconds_ago: i64) -> String {
        (Local::now() - chrono::Duration::seconds(seconds_ago)).to_rfc3339()
    }

    fn future_timestamp(seconds_from_now: i64) -> String {
        (Local::now() + chrono::Duration::seconds(seconds_from_now)).to_rfc3339()
    }

    fn past_timestamp(seconds_ago: i64) -> String {
        recent_timestamp(seconds_ago)
    }

    fn quota_snapshot(
        primary_remaining: f64,
        secondary_remaining: f64,
        primary_reset: &str,
        secondary_reset: &str,
    ) -> QuotaSnapshot {
        QuotaSnapshot {
            status: SnapshotStatus::Ok,
            source: "codex-wham".to_string(),
            source_label: Some("ChatGPT usage API".to_string()),
            web_url: Some(quota_core::USAGE_URL.to_string()),
            limit_id: Some("codex".to_string()),
            plan_type: Some("team".to_string()),
            primary_used_percent: Some(100.0 - primary_remaining),
            primary_remaining_percent: Some(primary_remaining),
            primary_window_minutes: Some(300),
            primary_resets_at: Some(primary_reset.to_string()),
            secondary_used_percent: Some(100.0 - secondary_remaining),
            secondary_remaining_percent: Some(secondary_remaining),
            secondary_window_minutes: Some(10_080),
            secondary_resets_at: Some(secondary_reset.to_string()),
            credits: None,
            observed_at: Some(now_local()),
            captured_at: Some(now_local()),
            error: None,
        }
    }

    #[test]
    fn maps_open_meteo_weather_codes_to_compact_display_labels() {
        assert_eq!(open_meteo_weather_summary_for_code(0), "CLEAR");
        assert_eq!(open_meteo_weather_summary_for_code(2), "PARTLY CLOUDY");
        assert_eq!(open_meteo_weather_summary_for_code(45), "FOG");
        assert_eq!(open_meteo_weather_summary_for_code(65), "RAIN");
        assert_eq!(open_meteo_weather_summary_for_code(75), "SNOW");
        assert_eq!(open_meteo_weather_summary_for_code(95), "STORM");
        assert_eq!(open_meteo_weather_summary_for_code(999), "WEATHER");
    }

    #[test]
    fn maps_open_meteo_weather_codes_to_display_icons() {
        assert_eq!(open_meteo_weather_icon_for_code(0), "sun");
        assert_eq!(open_meteo_weather_icon_for_code(2), "partly-cloudy");
        assert_eq!(open_meteo_weather_icon_for_code(45), "fog");
        assert_eq!(open_meteo_weather_icon_for_code(65), "rain");
        assert_eq!(open_meteo_weather_icon_for_code(75), "snow");
        assert_eq!(open_meteo_weather_icon_for_code(95), "storm");
        assert_eq!(open_meteo_weather_icon_for_code(999), "unknown");
    }

    #[test]
    fn builds_open_meteo_url_from_configured_coordinates() {
        let url = open_meteo_url(31.23041, 121.47369);
        assert!(url.contains("latitude=31.2304"));
        assert!(url.contains("longitude=121.4737"));
        assert!(url.contains("current=temperature_2m,weather_code,wind_speed_10m"));
        assert!(url.contains("timezone=auto"));
    }

    #[test]
    fn builds_caiyun_realtime_url_with_token_in_path_and_lon_lat_order() {
        let url = caiyun_realtime_url(
            "token-123",
            DEFAULT_WEATHER_LATITUDE,
            DEFAULT_WEATHER_LONGITUDE,
        );
        assert_eq!(
            url,
            "https://api.caiyunapp.com/v2.6/token-123/116.341625,39.995401/realtime"
        );
    }

    #[test]
    fn auto_weather_prefers_caiyun_when_token_is_configured() {
        let mut config = test_config(None);
        config.caiyun_token = Some("token-123".to_string());
        assert_eq!(
            weather_fetch_order(&config),
            vec![WeatherProvider::Caiyun, WeatherProvider::OpenMeteo]
        );
    }

    #[test]
    fn parses_weather_provider_names() {
        assert_eq!(
            parse_weather_provider("caiyun"),
            Some(WeatherProvider::Caiyun)
        );
        assert_eq!(
            parse_weather_provider("open-meteo"),
            Some(WeatherProvider::OpenMeteo)
        );
        assert_eq!(parse_weather_provider("bad-provider"), None);
    }

    #[test]
    fn returns_cached_weather_before_fetching_again() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let cached = WeatherSnapshot {
            status: "ok".to_string(),
            label: "TEST".to_string(),
            summary: "CLEAR".to_string(),
            icon: "sun".to_string(),
            temperature_c: Some(25),
            wind_kmh: Some(6),
            weather_code: Some(0),
            observed_at: "2026-05-31T12:00:00+08:00".to_string(),
        };
        {
            let mut state = state.lock().unwrap();
            state.weather = Some(CachedWeather {
                snapshot: cached.clone(),
                fetched_at: Instant::now(),
            });
        }

        assert_eq!(cached_or_refresh_weather(&state, &config), cached);
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
    fn recovers_identity_from_json_message_payload() {
        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "message": "{\"session_id\":\"session-1\",\"turn_id\":\"turn-1\",\"cwd\":\"D:\\\\Desktop\\\\codex\",\"model\":\"gpt-5.5\"}"
        }));

        assert_eq!(event.status, "running");
        assert_eq!(event.session_id.as_deref(), Some("session-1"));
        assert_eq!(event.turn_id.as_deref(), Some("turn-1"));
        assert_eq!(event.cwd.as_deref(), Some("D:\\Desktop\\codex"));
        assert_eq!(event.model.as_deref(), Some("gpt-5.5"));
    }

    #[test]
    fn recovers_identity_from_clipped_json_message_payload() {
        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "message": "{\"session_id\":\"session-1\",\"turn_id\":\"turn-1\",\"transcript_path\":null,\"cwd\":\"C:\\\\Program Files\\\\WindowsApps\\\\OpenAI.Codex_26.527.3686.0_x64__2p2nqsd0c76g0\\\\app"
        }));

        assert_eq!(event.status, "running");
        assert_eq!(event.session_id.as_deref(), Some("session-1"));
        assert_eq!(event.turn_id.as_deref(), Some("turn-1"));
        assert_eq!(
            event.cwd.as_deref(),
            Some("C:\\Program Files\\WindowsApps\\OpenAI.Codex_26.527.3686.0_x64__2p2nqsd0c76g0\\app")
        );
    }

    #[test]
    fn maps_stop_to_done() {
        let event = normalize_event(&json!({
            "hook_event_name": "Stop",
            "session_id": "session-1",
            "cwd": "D:\\Desktop\\codex"
        }));

        assert_eq!(event.status, "done");
        assert_eq!(event.title, "Codex done");
        assert_eq!(event.session_id.as_deref(), Some("session-1"));
        assert_eq!(event.cwd.as_deref(), Some("D:\\Desktop\\codex"));
    }

    #[test]
    fn maps_claude_stop_to_claude_done() {
        let event = normalize_event(&json!({
            "hook_event_name": "Stop",
            "source": "Claude"
        }));

        assert_eq!(event.status, "done");
        assert_eq!(event.title, "Claude done");
        assert_eq!(event.source.as_deref(), Some("claude"));
    }

    #[test]
    fn maps_notify_event_to_done() {
        let event = normalize_event(&json!({
            "type": "agent-turn-complete",
            "last-assistant-message": "finished"
        }));

        assert_eq!(event.kind, "agent-turn-complete");
        assert_eq!(event.status, "done");
        assert_eq!(event.title, "Codex done");
        assert_eq!(event.message, "finished");
    }

    #[test]
    fn rejects_invalid_hook_json_before_normalizing_event() {
        assert!(parse_hook_payload(br#"{"hook_event_name":"UserPromptSubmit"}"#).is_ok());
        assert!(parse_hook_payload(b"{hook_event_name:UserPromptSubmit}").is_err());
    }

    #[test]
    fn quota_background_refresh_returns_placeholder_without_blocking() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let snapshot = cached_or_refresh_quota_background(&state);

        assert_eq!(snapshot.status, SnapshotStatus::NoData);
        assert!(snapshot.error.as_deref().unwrap_or("").contains("pending"));

        let state = state.lock().unwrap();
        assert!(state.quota_refreshing || state.quota.is_some());
    }

    #[test]
    fn quota_refresh_due_respects_one_minute_interval() {
        let mut state = BridgeState::default();
        assert!(quota_refresh_is_due(&state));

        state.quota_refreshing = true;
        assert!(!quota_refresh_is_due(&state));

        state.quota_refreshing = false;
        state.quota = Some(CachedQuota {
            snapshot: quota_unavailable(),
            fetched_at: Instant::now(),
        });
        assert!(!quota_refresh_is_due(&state));

        state.quota = Some(CachedQuota {
            snapshot: quota_unavailable(),
            fetched_at: Instant::now() - QUOTA_REFRESH_INTERVAL - Duration::from_millis(1),
        });
        assert!(quota_refresh_is_due(&state));
    }

    #[test]
    fn quota_merge_keeps_previous_snapshot_when_refresh_has_no_actual_data() {
        let previous = quota_snapshot(
            64.0,
            92.0,
            "2026-06-02T18:00:00+08:00",
            "2026-06-08T18:00:00+08:00",
        );
        let fetched = quota_unavailable();

        let merged = merge_quota_snapshot(Some(&previous), fetched);

        assert_eq!(merged.status, SnapshotStatus::Ok);
        assert_eq!(merged.primary_remaining_percent, Some(64.0));
        assert_eq!(merged.secondary_remaining_percent, Some(92.0));
        assert_eq!(
            merged.primary_resets_at.as_deref(),
            Some("2026-06-02T18:00:00+08:00")
        );
    }

    #[test]
    fn quota_merge_keeps_reset_time_until_recorded_time_has_passed() {
        let previous_reset = future_timestamp(3600);
        let fetched_reset = future_timestamp(7200);
        let previous = quota_snapshot(64.0, 92.0, &previous_reset, &future_timestamp(86_400));
        let fetched = quota_snapshot(63.0, 91.0, &fetched_reset, &future_timestamp(172_800));

        let merged = merge_quota_snapshot(Some(&previous), fetched);

        assert_eq!(merged.primary_remaining_percent, Some(63.0));
        assert_eq!(merged.primary_resets_at, Some(previous_reset));
    }

    #[test]
    fn quota_merge_accepts_new_reset_time_after_recorded_time_has_passed() {
        let previous_reset = past_timestamp(60);
        let fetched_reset = future_timestamp(3600);
        let previous = quota_snapshot(64.0, 92.0, &previous_reset, &past_timestamp(60));
        let fetched = quota_snapshot(99.0, 100.0, &fetched_reset, &future_timestamp(604_800));

        let merged = merge_quota_snapshot(Some(&previous), fetched);

        assert_eq!(merged.primary_remaining_percent, Some(99.0));
        assert_eq!(merged.primary_resets_at, Some(fetched_reset));
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
    fn discovery_info_for_lan_peer_uses_bridge_port() {
        let mut config = test_config(None);
        config.bind = "0.0.0.0:9876".to_string();
        let peer = "192.168.1.44:50000".parse::<SocketAddr>().unwrap();

        let info = discover_info_for_peer(&config, peer).unwrap();

        assert_eq!(info.service, "codex-ornament-bridge");
        assert!(info.state_url.starts_with("http://"));
        assert!(info.state_url.ends_with(":9876/state"));
        assert!(info.health_url.ends_with(":9876/health"));
        assert!(!info.local_ip.is_empty());
    }

    #[test]
    fn discovery_rejects_proxy_and_non_lan_addresses() {
        assert!(!lan_discovery_ip_is_usable("127.0.0.1".parse().unwrap()));
        assert!(!lan_discovery_ip_is_usable("169.254.1.2".parse().unwrap()));
        assert!(!lan_discovery_ip_is_usable("198.18.0.1".parse().unwrap()));
        assert!(!lan_discovery_ip_is_usable(
            "198.19.255.254".parse().unwrap()
        ));
        assert!(lan_discovery_ip_is_usable("192.168.1.101".parse().unwrap()));
        assert!(lan_discovery_ip_is_usable("10.0.0.2".parse().unwrap()));
        assert!(lan_discovery_ip_is_usable("172.16.0.2".parse().unwrap()));
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
            "turn_id": "turn-2"
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
                "source:codex:session:session-1:turn:turn-1",
                "source:codex:session:session-2:turn:turn-2"
            ]
        );
    }

    #[test]
    fn reopening_same_turn_in_another_session_replaces_stale_active_task() {
        let mut state = BridgeState::default();
        let first = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-shared"
        }));
        let reopened = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2",
            "turn_id": "turn-shared"
        }));

        apply_task_event(&mut state, first);
        apply_task_event(&mut state, reopened);

        assert_eq!(state.active_tasks.len(), 1);
        assert_eq!(
            state
                .active_order
                .iter()
                .map(String::as_str)
                .collect::<Vec<_>>(),
            vec!["source:codex:session:session-2:turn:turn-shared"]
        );
    }

    #[test]
    fn stopping_reopened_turn_clears_stale_active_task_from_other_session() {
        let mut state = BridgeState::default();
        let first = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-shared"
        }));
        let reopened = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2",
            "turn_id": "turn-shared"
        }));
        let stop = normalize_event(&json!({
            "hook_event_name": "Stop",
            "session_id": "session-2",
            "turn_id": "turn-shared"
        }));

        apply_task_event(&mut state, first);
        apply_task_event(&mut state, reopened);
        apply_task_event(&mut state, stop);

        assert_eq!(state.active_tasks.len(), 0);
        assert_eq!(state.done_tasks.len(), 1);
        assert_eq!(
            state.task.as_ref().map(|event| event.status.as_str()),
            Some("done")
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
                "source:codex:session:session-1:anonymous:1",
                "source:codex:session:session-1:anonymous:2"
            ]
        );
    }

    #[test]
    fn stop_removes_matching_running_task() {
        let mut state = BridgeState::default();
        let start = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "message": "{\"session_id\":\"session-1\",\"turn_id\":\"turn-1\"}"
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
    fn session_only_stop_does_not_complete_identified_turn_task() {
        let mut state = BridgeState::default();
        let start = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1",
            "prompt": "long task"
        }));
        let permission_auto_allow_stop = normalize_event(&json!({
            "hook_event_name": "Stop",
            "session_id": "session-1"
        }));

        apply_task_event(&mut state, start);
        apply_task_event(&mut state, permission_auto_allow_stop);

        assert_eq!(state.active_tasks.len(), 1);
        assert_eq!(state.done_tasks.len(), 0);
        assert_eq!(state.unmatched_stops.len(), 1);
        assert_eq!(
            state.task.as_ref().map(|event| event.status.as_str()),
            Some("running")
        );
    }

    #[test]
    fn snapshot_dedupes_existing_duplicate_active_turns() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            insert_active_task(
                &mut state,
                "source:codex:session:session-1:turn:turn-shared".to_string(),
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-shared"
                })),
            );
            insert_active_task(
                &mut state,
                "source:codex:session:session-2:turn:turn-shared".to_string(),
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-2",
                    "turn_id": "turn-shared"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(
            snapshot
                .active_tasks
                .first()
                .and_then(|event| event.session_id.as_deref()),
            Some("session-2")
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
    fn identified_stop_without_active_task_is_unmatched() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "codex-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "foreign-session",
                    "turn_id": "foreign-turn",
                    "cwd": "C:\\Program Files\\WindowsApps\\OpenAI.Codex\\app"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.done_seq, 0);
        assert_eq!(snapshot.done_task_count, 0);
        assert_eq!(snapshot.unmatched_stop_count, 1);
        assert_eq!(snapshot.source_tasks.codex.done_seq, 0);
    }

    #[test]
    fn claude_fallback_start_and_stop_update_claude_summary() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "message": "UserPromptSubmit"
                })),
            );
        }

        let running = task_snapshot(&state, &test_config(None));
        assert_eq!(running.source_tasks.claude.status, "running");
        assert_eq!(running.source_tasks.claude.active_count, 1);
        assert_eq!(running.source_tasks.claude.done_seq, 0);
        assert_eq!(running.source_tasks.codex.active_count, 0);

        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "message": "Stop"
                })),
            );
        }

        let done = task_snapshot(&state, &test_config(None));
        assert_eq!(done.source_tasks.claude.status, "done");
        assert_eq!(done.source_tasks.claude.active_count, 0);
        assert_eq!(done.source_tasks.claude.done_seq, 1);
        assert_eq!(done.source_tasks.codex.active_count, 0);
        assert_eq!(done.source_tasks.codex.done_seq, 0);
        assert_eq!(
            done.source_tasks
                .claude
                .task
                .as_ref()
                .map(|event| event.title.as_str()),
            Some("Claude done")
        );
    }

    #[test]
    fn claude_stop_without_active_task_is_unmatched() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "claude-session"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.done_seq, 0);
        assert_eq!(snapshot.source_tasks.claude.status, "done");
        assert_eq!(snapshot.source_tasks.claude.active_count, 0);
        assert_eq!(snapshot.source_tasks.claude.done_seq, 0);
        assert_eq!(snapshot.source_tasks.codex.done_seq, 0);
        assert_eq!(snapshot.unmatched_stop_count, 1);
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
    fn session_stop_without_turn_id_matches_only_same_source() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "shared-session",
                    "prompt": "codex work"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "shared-session",
                    "prompt": "claude work"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "shared-session",
                    "message": "claude done"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.source_tasks.codex.status, "running");
        assert_eq!(snapshot.source_tasks.codex.active_count, 1);
        assert_eq!(snapshot.source_tasks.claude.status, "done");
        assert_eq!(snapshot.source_tasks.claude.active_count, 0);
        assert_eq!(snapshot.source_tasks.claude.done_seq, 1);
        assert_eq!(
            snapshot
                .active_tasks
                .first()
                .map(|event| event.message.as_str()),
            Some("codex work")
        );
    }

    #[test]
    fn source_aware_turn_keys_keep_codex_and_claude_independent() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "shared-session",
                    "turn_id": "turn-1",
                    "prompt": "codex work"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "shared-session",
                    "turn_id": "turn-1",
                    "prompt": "claude work"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "shared-session",
                    "turn_id": "turn-1",
                    "message": "claude done"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.source_tasks.codex.active_count, 1);
        assert_eq!(snapshot.source_tasks.claude.active_count, 0);
        assert_eq!(snapshot.source_tasks.claude.done_seq, 1);
        assert_eq!(
            snapshot
                .source_tasks
                .codex
                .task
                .as_ref()
                .map(|event| event.message.as_str()),
            Some("codex work")
        );
    }

    #[test]
    fn multiple_claude_tasks_same_session_stop_removes_latest_only() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "prompt": "first"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "prompt": "second"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "claude-session"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.source_tasks.claude.status, "running");
        assert_eq!(snapshot.source_tasks.claude.active_count, 1);
        assert_eq!(snapshot.source_tasks.claude.done_seq, 1);
        assert_eq!(
            snapshot
                .source_tasks
                .claude
                .task
                .as_ref()
                .map(|event| event.message.as_str()),
            Some("first")
        );
    }

    #[test]
    fn unmatched_stop_does_not_complete_other_source_task() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "codex-session"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.done_seq, 0);
        assert_eq!(snapshot.unmatched_stop_count, 1);
        assert_eq!(snapshot.source_tasks.claude.status, "running");
        assert_eq!(snapshot.source_tasks.claude.active_count, 1);
        assert_eq!(snapshot.source_tasks.codex.done_seq, 0);
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
                    "turn_id": "turn-2"
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
    fn snapshot_combines_recent_claude_and_codex_done_sources() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "codex-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "codex-session",
                    "turn_id": "turn-1"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(snapshot.status, "done");
        assert_eq!(snapshot.done_seq, 2);
        assert_eq!(snapshot.source_tasks.claude.status, "done");
        assert_eq!(snapshot.source_tasks.codex.status, "done");
        assert_eq!(snapshot.source_tasks.claude.done_seq, 1);
        assert_eq!(snapshot.source_tasks.codex.done_seq, 1);
        assert_eq!(
            snapshot.task.as_ref().map(|event| event.title.as_str()),
            Some("Claude + Codex done")
        );
        assert_eq!(
            snapshot
                .task
                .as_ref()
                .and_then(|event| event.source.as_deref()),
            Some("claude+codex")
        );
    }

    #[test]
    fn snapshot_keeps_source_task_summaries_separate_when_both_are_active() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "codex-session",
                    "turn_id": "turn-1",
                    "prompt": "codex work"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1",
                    "prompt": "claude work"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(snapshot.status, "running");
        assert_eq!(snapshot.active_task_count, 2);
        assert_eq!(snapshot.source_tasks.codex.status, "running");
        assert_eq!(snapshot.source_tasks.codex.active_count, 1);
        assert_eq!(
            snapshot
                .source_tasks
                .codex
                .task
                .as_ref()
                .map(|event| event.title.as_str()),
            Some("Codex running")
        );
        assert_eq!(snapshot.source_tasks.claude.status, "running");
        assert_eq!(snapshot.source_tasks.claude.active_count, 1);
        assert_eq!(
            snapshot
                .source_tasks
                .claude
                .task
                .as_ref()
                .map(|event| event.title.as_str()),
            Some("Claude running")
        );
    }

    #[test]
    fn snapshot_combines_recent_done_sources_regardless_of_arrival_order() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "codex-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "codex-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(
            snapshot.task.as_ref().map(|event| event.title.as_str()),
            Some("Claude + Codex done")
        );
    }

    #[test]
    fn snapshot_keeps_single_done_source_when_previous_source_is_stale() {
        let mut state_value = BridgeState::default();
        remember_done_task(
            &mut state_value,
            TaskEvent {
                kind: "Stop".to_string(),
                status: "done".to_string(),
                title: "Claude done".to_string(),
                message: "Claude done".to_string(),
                received_at: "2026-05-30T00:00:00+08:00".to_string(),
                source: Some("claude".to_string()),
                session_id: Some("claude-session".to_string()),
                turn_id: Some("turn-1".to_string()),
                cwd: None,
                model: None,
            },
        );
        let codex_done = TaskEvent {
            kind: "Stop".to_string(),
            status: "done".to_string(),
            title: "Codex done".to_string(),
            message: "Codex done".to_string(),
            received_at: "2026-05-30T00:00:10+08:00".to_string(),
            source: None,
            session_id: Some("codex-session".to_string()),
            turn_id: Some("turn-1".to_string()),
            cwd: None,
            model: None,
        };
        remember_done_task(&mut state_value, codex_done.clone());
        state_value.task = Some(codex_done);
        let state = Arc::new(Mutex::new(state_value));

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(
            snapshot.task.as_ref().map(|event| event.title.as_str()),
            Some("Codex done")
        );
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
    fn snapshot_drops_completed_active_task_when_session_log_has_terminal_turn() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-stale-terminal-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            concat!(
                "{\"timestamp\":\"2026-05-31T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:02+08:00\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"turn-1\",\"last_agent_message\":\"done\"}}\n"
            ),
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

        let snapshot = task_snapshot(&state, &scoped_test_config("session-1", &codex_home));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
        assert_eq!(snapshot.done_seq, 0);
    }

    #[test]
    fn snapshot_replaces_stale_active_task_when_session_log_moved_to_new_turn() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-stale-active-turn-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        let old_started_at = recent_timestamp(3);
        let old_done_at = recent_timestamp(2);
        let new_started_at = recent_timestamp(1);
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            format!(
                "{{\"timestamp\":\"{old_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"turn-old\"}}}}\n\
                 {{\"timestamp\":\"{old_done_at}\",\"payload\":{{\"type\":\"task_complete\",\"turn_id\":\"turn-old\"}}}}\n\
                 {{\"timestamp\":\"{new_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"turn-new\"}}}}\n"
            ),
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
                    "turn_id": "turn-old"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &scoped_test_config("session-1", &codex_home));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.status, "done");
        assert_eq!(
            snapshot
                .active_tasks
                .first()
                .and_then(|event| event.turn_id.as_deref()),
            Some("turn-new")
        );
    }

    #[test]
    fn snapshot_recovers_recent_session_task_while_other_task_is_active() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-recover-with-active-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        let recovered_at = recent_timestamp(1);
        fs::write(
            session_dir.join("rollout-2026-05-31T20-42-42-11111111-1111-1111-1111-111111111111.jsonl"),
            format!(
                "{{\"timestamp\":\"{recovered_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"recovered-turn\"}}}}\n"
            ),
        )
        .unwrap();
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "22222222-2222-2222-2222-222222222222",
                    "turn_id": "memory-turn"
                })),
            );
        }

        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 2);
        assert_eq!(
            snapshot
                .active_tasks
                .iter()
                .filter_map(|event| event.session_id.as_deref())
                .collect::<Vec<_>>(),
            vec![
                "22222222-2222-2222-2222-222222222222",
                "11111111-1111-1111-1111-111111111111"
            ]
        );
    }

    #[test]
    fn recovered_task_completed_during_cache_ttl_is_not_restored_again() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));
        {
            let mut state = state.lock().unwrap();
            state.active_recovery = Some(CachedActiveRecovery {
                events: vec![event.clone()],
                fetched_at: Instant::now(),
            });
            apply_task_event(&mut state, event);
        }

        let config = test_config(None);
        let first = task_snapshot(&state, &config);
        assert_eq!(first.active_task_count, 1);

        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "session-1",
                    "turn_id": "turn-1"
                })),
            );
        }

        let second = task_snapshot(&state, &config);
        assert_eq!(second.active_task_count, 0);
        assert_eq!(second.done_seq, 1);
        assert_eq!(second.status, "done");
    }

    #[test]
    fn snapshot_keeps_recent_active_task_when_session_file_is_not_written_yet() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-with-delayed-log",
                    "turn_id": "turn-1"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.status, "running");
    }

    #[test]
    fn snapshot_drops_old_active_task_when_session_file_is_missing() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            insert_active_task(
                &mut state,
                "source:codex:session:missing-session:turn:turn-1".to_string(),
                TaskEvent {
                    kind: "UserPromptSubmit".to_string(),
                    status: "running".to_string(),
                    title: "Codex running".to_string(),
                    message: "old missing session".to_string(),
                    received_at: "2026-05-31T00:00:00+08:00".to_string(),
                    source: None,
                    session_id: Some("missing-session".to_string()),
                    turn_id: Some("turn-1".to_string()),
                    cwd: None,
                    model: None,
                },
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
    }

    #[test]
    fn snapshot_keeps_claude_task_when_no_codex_session_file_exists() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.source_tasks.claude.active_count, 1);
        assert_eq!(snapshot.source_tasks.claude.status, "running");
    }

    #[test]
    fn snapshot_keeps_child_session_and_drops_forked_parent_session() {
        let codex_home =
            env::temp_dir().join(format!("codex-ornament-fork-active-{}", std::process::id()));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T12-17-11-parent-session.jsonl"),
            "{\"timestamp\":\"2026-05-31T08:00:00+08:00\",\"type\":\"session_meta\",\"payload\":{\"id\":\"parent-session\"}}\n",
        )
        .unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T17-00-13-child-session.jsonl"),
            "{\"timestamp\":\"2026-05-31T17:00:00+08:00\",\"type\":\"session_meta\",\"payload\":{\"id\":\"child-session\",\"forked_from_id\":\"parent-session\"}}\n",
        )
        .unwrap();
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                TaskEvent {
                    kind: "UserPromptSubmit".to_string(),
                    status: "running".to_string(),
                    title: "Codex running".to_string(),
                    message: "parent task".to_string(),
                    received_at: "2026-05-31T12:00:00+08:00".to_string(),
                    source: None,
                    session_id: Some("parent-session".to_string()),
                    turn_id: Some("parent-turn".to_string()),
                    cwd: None,
                    model: None,
                },
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "child-session",
                    "turn_id": "child-turn"
                })),
            );
        }

        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(
            snapshot
                .active_tasks
                .first()
                .and_then(|event| event.session_id.as_deref()),
            Some("child-session")
        );
    }

    #[test]
    fn snapshot_keeps_parent_task_started_after_child_session_fork() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-fork-parent-new-active-{}",
            std::process::id()
        ));
        let parent_started_at = recent_timestamp(2);
        let child_forked_at = recent_timestamp(4);
        let child_started_at = recent_timestamp(1);
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T12-17-11-11111111-1111-1111-1111-111111111111.jsonl"),
            format!(
                "{{\"timestamp\":\"{child_forked_at}\",\"type\":\"session_meta\",\"payload\":{{\"id\":\"11111111-1111-1111-1111-111111111111\"}}}}\n\
                 {{\"timestamp\":\"{parent_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"parent-new-turn\"}}}}\n"
            ),
        )
        .unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T17-00-13-22222222-2222-2222-2222-222222222222.jsonl"),
            format!(
                "{{\"timestamp\":\"{child_forked_at}\",\"type\":\"session_meta\",\"payload\":{{\"id\":\"22222222-2222-2222-2222-222222222222\",\"forked_from_id\":\"11111111-1111-1111-1111-111111111111\",\"timestamp\":\"{child_forked_at}\"}}}}\n\
                 {{\"timestamp\":\"{child_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"child-turn\"}}}}\n"
            ),
        )
        .unwrap();
        let state = Arc::new(Mutex::new(BridgeState::default()));

        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 2);
        assert_eq!(
            snapshot
                .active_tasks
                .iter()
                .filter_map(|event| event.session_id.as_deref())
                .collect::<Vec<_>>(),
            vec![
                "22222222-2222-2222-2222-222222222222",
                "11111111-1111-1111-1111-111111111111"
            ]
        );
    }

    #[test]
    fn session_log_parser_keeps_multiple_unfinished_turns() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-multiple-active-{}.jsonl",
            std::process::id()
        ));
        fs::write(
            &path,
            concat!(
                "{\"timestamp\":\"2026-05-31T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:01+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-2\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:02+08:00\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:03+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-3\"}}\n"
            ),
        )
        .unwrap();

        let active = active_tasks_in_session_file(&path, "session-1").unwrap();
        let _ = fs::remove_file(&path);

        assert_eq!(
            active
                .iter()
                .filter_map(|event| event.turn_id.as_deref())
                .collect::<Vec<_>>(),
            vec!["turn-2", "turn-3"]
        );
    }

    #[test]
    fn session_log_parser_closes_latest_turn_on_final_answer() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-final-answer-active-{}.jsonl",
            std::process::id()
        ));
        fs::write(
            &path,
            concat!(
                "{\"timestamp\":\"2026-05-31T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:01+08:00\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"turn one done\"}]}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:02+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-2\"}}\n"
            ),
        )
        .unwrap();

        let active = active_tasks_in_session_file(&path, "session-1").unwrap();
        let terminal = terminal_turn_in_file(&path, "turn-1").unwrap().unwrap();
        let _ = fs::remove_file(&path);

        assert_eq!(
            active
                .iter()
                .filter_map(|event| event.turn_id.as_deref())
                .collect::<Vec<_>>(),
            vec!["turn-2"]
        );
        assert_eq!(terminal.kind, "task_complete");
        assert_eq!(terminal.message.as_deref(), Some("turn one done"));
    }

    #[test]
    fn session_log_parser_keeps_turn_running_when_final_answer_is_followed_by_same_turn_context() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-final-answer-continued-{}.jsonl",
            std::process::id()
        ));
        fs::write(
            &path,
            concat!(
                "{\"timestamp\":\"2026-05-31T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:01+08:00\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"turn one provisional done\"}]}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:02+08:00\",\"payload\":{\"type\":\"turn_context\",\"turn_id\":\"turn-1\",\"cwd\":\"D:\\\\Desktop\\\\codex\",\"model\":\"gpt-5\"}}\n"
            ),
        )
        .unwrap();

        let active = active_tasks_in_session_file(&path, "session-1").unwrap();
        let terminal = terminal_turn_in_file(&path, "turn-1").unwrap();
        let _ = fs::remove_file(&path);

        assert_eq!(
            active
                .iter()
                .filter_map(|event| event.turn_id.as_deref())
                .collect::<Vec<_>>(),
            vec!["turn-1"]
        );
        assert_eq!(
            active.first().and_then(|event| event.model.as_deref()),
            Some("gpt-5")
        );
        assert!(terminal.is_none());
    }

    #[test]
    fn recent_recovery_ignores_orphaned_turn_when_newer_turn_started() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-orphan-newer-turn-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        let old_started_at = recent_timestamp(4);
        let new_started_at = recent_timestamp(2);
        let new_done_at = recent_timestamp(1);
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-019e7467-fc36-7750-8418-f2bf0397bd05.jsonl"),
            format!(
                "{{\"timestamp\":\"{old_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"turn-orphan\"}}}}\n\
                 {{\"timestamp\":\"{new_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"turn-new\"}}}}\n\
                 {{\"timestamp\":\"{new_done_at}\",\"payload\":{{\"type\":\"task_complete\",\"turn_id\":\"turn-new\"}}}}\n"
            ),
        )
        .unwrap();

        let active = active_tasks_in_recent_session_files(&codex_home);
        let _ = fs::remove_dir_all(&codex_home);

        assert!(active.is_empty(), "{active:?}");
    }

    #[test]
    fn snapshot_drops_orphaned_turn_after_final_answer_without_hook_complete() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-orphan-final-answer-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            concat!(
                "{\"timestamp\":\"2026-05-31T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:10+08:00\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"firmware flashed\"}]}}\n"
            ),
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

        let snapshot = task_snapshot(&state, &scoped_test_config("session-1", &codex_home));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
    }

    #[test]
    fn snapshot_restores_running_after_compaction_continues_same_turn() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-compaction-continues-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            concat!(
                "{\"timestamp\":\"2026-05-31T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:10+08:00\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"intermediate final answer\"}]}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:11+08:00\",\"type\":\"event_msg\",\"payload\":{\"type\":\"context_compacted\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:12+08:00\",\"payload\":{\"type\":\"turn_context\",\"turn_id\":\"turn-1\",\"cwd\":\"D:\\\\Desktop\\\\codex\",\"model\":\"gpt-5\"}}\n"
            ),
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
            remember_done_task(
                &mut state,
                TaskEvent {
                    kind: "Stop".to_string(),
                    status: "done".to_string(),
                    title: "Codex done".to_string(),
                    message: "stale done".to_string(),
                    received_at: "2026-05-31T12:00:10+08:00".to_string(),
                    source: None,
                    session_id: Some("session-1".to_string()),
                    turn_id: Some("turn-1".to_string()),
                    cwd: None,
                    model: None,
                },
            );
            state.task = state.done_tasks.back().cloned();
        }

        let snapshot = task_snapshot(&state, &scoped_test_config("session-1", &codex_home));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.status, "running");
        assert_eq!(
            snapshot.task.as_ref().map(|event| event.status.as_str()),
            Some("running")
        );
        assert_eq!(
            snapshot.task.as_ref().and_then(|event| event.turn_id.as_deref()),
            Some("turn-1")
        );
    }

    #[test]
    fn final_answer_reconcile_preserves_other_running_sessions() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-orphan-concurrent-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        let first_started_at = recent_timestamp(4);
        let second_started_at = recent_timestamp(3);
        let first_done_at = recent_timestamp(2);
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            format!(
                "{{\"timestamp\":\"{first_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}}}\n\
                 {{\"timestamp\":\"{first_done_at}\",\"type\":\"response_item\",\"payload\":{{\"type\":\"message\",\"phase\":\"final_answer\",\"content\":[{{\"type\":\"output_text\",\"text\":\"first done\"}}]}}}}\n"
            ),
        )
        .unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-10-session-2.jsonl"),
            format!(
                "{{\"timestamp\":\"{second_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"turn-2\"}}}}\n"
            ),
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
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-2",
                    "turn_id": "turn-2"
                })),
            );
        }

        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(
            snapshot
                .active_tasks
                .first()
                .and_then(|event| event.session_id.as_deref()),
            Some("session-2")
        );
        assert_eq!(snapshot.status, "done");
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
    fn task_consumer_applies_queued_events_in_order() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, receiver);

        let first = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));
        let second = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2",
            "turn_id": "turn-2"
        }));

        assert_eq!(
            dispatch_task_event(&sender, first),
            Ok(TaskDispatchResult::Applied)
        );
        assert_eq!(
            dispatch_task_event(&sender, second),
            Ok(TaskDispatchResult::Applied)
        );

        let state = state.lock().unwrap();
        assert_eq!(state.active_tasks.len(), 2);
        assert_eq!(
            state
                .active_order
                .iter()
                .map(String::as_str)
                .collect::<Vec<_>>(),
            vec![
                "source:codex:session:session-1:turn:turn-1",
                "source:codex:session:session-2:turn:turn-2"
            ]
        );
    }

    #[test]
    fn cloned_task_producers_can_submit_concurrently() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, receiver);

        let producers = (0..8)
            .map(|index| {
                let sender = sender.clone();
                std::thread::spawn(move || {
                    let event = normalize_event(&json!({
                        "hook_event_name": "UserPromptSubmit",
                        "session_id": format!("session-{index}"),
                        "turn_id": format!("turn-{index}")
                    }));
                    dispatch_task_event(&sender, event)
                })
            })
            .collect::<Vec<_>>();

        for producer in producers {
            assert_eq!(producer.join().unwrap(), Ok(TaskDispatchResult::Applied));
        }

        let state = state.lock().unwrap();
        assert_eq!(state.active_tasks.len(), 8);
        for index in 0..8 {
            assert!(state.active_tasks.contains_key(&format!(
                "source:codex:session:session-{index}:turn:turn-{index}"
            )));
        }
    }

    #[test]
    fn concurrent_task_completions_are_applied_without_losing_done_events() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, receiver);

        for index in 0..8 {
            let event = normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": format!("session-{index}"),
                "turn_id": format!("turn-{index}")
            }));
            assert_eq!(
                dispatch_task_event(&sender, event),
                Ok(TaskDispatchResult::Applied)
            );
        }

        let producers = (0..8)
            .map(|index| {
                let sender = sender.clone();
                std::thread::spawn(move || {
                    let event = normalize_event(&json!({
                        "hook_event_name": "Stop",
                        "session_id": format!("session-{index}"),
                        "turn_id": format!("turn-{index}"),
                        "message": format!("session-{index} done")
                    }));
                    dispatch_task_event(&sender, event)
                })
            })
            .collect::<Vec<_>>();

        for producer in producers {
            assert_eq!(producer.join().unwrap(), Ok(TaskDispatchResult::Applied));
        }

        let state = state.lock().unwrap();
        assert!(state.active_tasks.is_empty());
        assert_eq!(state.done_tasks.len(), 8);
        assert_eq!(state.done_seq, 8);
        for index in 0..8 {
            assert!(state.done_tasks.iter().any(|event| {
                event.session_id.as_deref() == Some(&format!("session-{index}"))
                    && event.message == format!("session-{index} done")
            }));
        }
    }

    #[test]
    fn concurrent_mixed_source_tasks_complete_independently() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, receiver);

        let starts = (0..6)
            .map(|index| {
                let sender = sender.clone();
                std::thread::spawn(move || {
                    let mut payload = json!({
                        "hook_event_name": "UserPromptSubmit",
                        "session_id": format!("shared-session-{index}"),
                        "turn_id": format!("turn-{index}"),
                        "prompt": format!("task-{index}")
                    });
                    if index % 2 == 1 {
                        payload["source"] = json!("Claude");
                    }
                    dispatch_task_event(&sender, normalize_event(&payload))
                })
            })
            .collect::<Vec<_>>();

        for start in starts {
            assert_eq!(start.join().unwrap(), Ok(TaskDispatchResult::Applied));
        }

        {
            let snapshot = task_snapshot(&state, &test_config(None));
            assert_eq!(snapshot.active_task_count, 6);
            assert_eq!(snapshot.source_tasks.codex.active_count, 3);
            assert_eq!(snapshot.source_tasks.claude.active_count, 3);
            assert_eq!(snapshot.source_tasks.codex.done_seq, 0);
            assert_eq!(snapshot.source_tasks.claude.done_seq, 0);
        }

        let stops = (0..6)
            .map(|index| {
                let sender = sender.clone();
                std::thread::spawn(move || {
                    let mut payload = json!({
                        "hook_event_name": "Stop",
                        "session_id": format!("shared-session-{index}"),
                        "turn_id": format!("turn-{index}"),
                        "message": format!("task-{index} done")
                    });
                    if index % 2 == 1 {
                        payload["source"] = json!("Claude");
                    }
                    dispatch_task_event(&sender, normalize_event(&payload))
                })
            })
            .collect::<Vec<_>>();

        for stop in stops {
            assert_eq!(stop.join().unwrap(), Ok(TaskDispatchResult::Applied));
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.done_seq, 6);
        assert_eq!(snapshot.source_tasks.codex.active_count, 0);
        assert_eq!(snapshot.source_tasks.codex.done_seq, 3);
        assert_eq!(snapshot.source_tasks.claude.active_count, 0);
        assert_eq!(snapshot.source_tasks.claude.done_seq, 3);
    }

    #[test]
    fn task_dispatch_waits_until_consumer_updates_state() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, receiver);

        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));

        assert_eq!(
            dispatch_task_event(&sender, event),
            Ok(TaskDispatchResult::Applied)
        );

        let state = state.lock().unwrap();
        assert_eq!(state.active_tasks.len(), 1);
        assert_eq!(
            state.task.as_ref().map(|event| event.status.as_str()),
            Some("running")
        );
    }

    #[test]
    fn task_consumer_filters_events_outside_tracked_session() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = scoped_test_config("session-1", ".codex-test");
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, receiver);

        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2",
            "turn_id": "turn-1"
        }));

        assert_eq!(
            dispatch_task_event(&sender, event),
            Ok(TaskDispatchResult::Filtered)
        );

        let state = state.lock().unwrap();
        assert!(state.active_tasks.is_empty());
        assert!(state.task.is_none());
    }

    #[test]
    fn dispatch_reports_full_queue_without_blocking() {
        let (sender, _receiver) = mpsc::sync_channel(1);
        let event = normalize_event(&json!({"hook_event_name": "UserPromptSubmit"}));
        let (completion, _completed) = mpsc::sync_channel(0);
        sender
            .try_send(QueuedTaskEvent {
                event: event.clone(),
                completion,
            })
            .unwrap();

        assert_eq!(
            dispatch_task_event(&sender, event),
            Err(TaskQueueError::Full)
        );
    }

    #[test]
    fn dispatch_reports_closed_consumer() {
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        drop(receiver);

        let event = normalize_event(&json!({"hook_event_name": "UserPromptSubmit"}));

        assert_eq!(
            dispatch_task_event(&sender, event),
            Err(TaskQueueError::Closed)
        );
    }

    #[test]
    fn dispatch_reports_completion_drop_when_consumer_exits_without_ack() {
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        let consumer = std::thread::spawn(move || {
            let _ = receiver.recv();
        });

        let event = normalize_event(&json!({"hook_event_name": "UserPromptSubmit"}));

        assert_eq!(
            dispatch_task_event(&sender, event),
            Err(TaskQueueError::CompletionDropped)
        );
        consumer.join().unwrap();
    }

    #[test]
    fn consumer_reports_lock_unavailable_when_state_lock_is_poisoned() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let poisoned_state = Arc::clone(&state);
        let _ = std::thread::spawn(move || {
            let _guard = poisoned_state.lock().unwrap();
            panic!("poison task state lock for test");
        })
        .join();

        let event = normalize_event(&json!({"hook_event_name": "UserPromptSubmit"}));

        assert_eq!(
            consume_task_event(&state, &test_config(None), event),
            TaskDispatchResult::LockUnavailable
        );
    }

    #[test]
    fn dispatch_reports_queued_when_consumer_ack_is_slow() {
        let (sender, receiver): (TaskEventSender, TaskEventReceiver) =
            mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        let consumer = std::thread::spawn(move || {
            let queued = receiver.recv().unwrap();
            std::thread::sleep(TASK_EVENT_ACK_TIMEOUT + Duration::from_millis(50));
            let _ = queued.completion.send(TaskDispatchResult::Applied);
        });

        let event = normalize_event(&json!({"hook_event_name": "UserPromptSubmit"}));

        assert_eq!(
            dispatch_task_event(&sender, event),
            Ok(TaskDispatchResult::Queued)
        );
        consumer.join().unwrap();
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
    fn recovers_active_turn_from_recent_session_log_without_tracked_session() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-recover-recent-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        let session_file = session_dir
            .join("rollout-2026-05-31T11-43-03-019e7467-fc36-7750-8418-f2bf0397bd05.jsonl");
        fs::write(
            &session_file,
            format!(
                "{}\n{}\n",
                json!({
                    "timestamp": now_local(),
                    "type": "event_msg",
                    "payload": {
                        "type": "task_started",
                        "turn_id": "turn-1"
                    }
                }),
                json!({
                    "timestamp": now_local(),
                    "type": "turn_context",
                    "payload": {
                        "turn_id": "turn-1",
                        "cwd": "D:\\Desktop\\codex",
                        "model": "gpt-5.5"
                    }
                })
            ),
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.status, "running");
        let task = snapshot.task.unwrap();
        assert_eq!(
            task.session_id.as_deref(),
            Some("019e7467-fc36-7750-8418-f2bf0397bd05")
        );
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
