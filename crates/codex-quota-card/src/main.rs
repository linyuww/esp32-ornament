use quota_core::{get_quota_snapshot, read_state, state_path, write_state, QuotaSnapshot, SnapshotStatus};
use std::{env, fs, path::PathBuf};

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut refresh = false;
    let mut output = PathBuf::from("/tmp/codex-quota.svg");

    for arg in env::args().skip(1) {
        match arg.as_str() {
            "--refresh" | "-r" => refresh = true,
            "--help" | "-h" => {
                print_help();
                return Ok(());
            }
            value => output = PathBuf::from(value),
        }
    }

    let snapshot = if refresh {
        let snapshot = get_quota_snapshot();
        let _ = write_state(state_path(), &snapshot);
        snapshot
    } else {
        read_state(state_path())?.unwrap_or_else(|| {
            let snapshot = get_quota_snapshot();
            let _ = write_state(state_path(), &snapshot);
            snapshot
        })
    };

    let svg = render_svg(&snapshot);
    if let Some(parent) = output.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)?;
        }
    }
    fs::write(&output, svg)?;
    println!("{}", output.display());
    Ok(())
}

fn print_help() {
    println!("Usage: codex-quota-card [--refresh] [output.svg]");
    println!("Default output: /tmp/codex-quota.svg");
}

fn render_svg(snapshot: &QuotaSnapshot) -> String {
    let status_ok = snapshot.status == SnapshotStatus::Ok;
    let status = match snapshot.status {
        SnapshotStatus::Ok => "已同步",
        SnapshotStatus::NoData => "无额度数据",
        SnapshotStatus::ParseError => "解析异常",
        SnapshotStatus::AuthRequired => "需要登录",
        SnapshotStatus::RequestFailed => "请求失败",
    };
    let primary_remaining = snapshot.primary_remaining_percent.unwrap_or(0.0).clamp(0.0, 100.0);
    let dot = if status_ok { "#58d39c" } else { "#eeb85c" };
    let pill_bg = if status_ok { "#1f4e3f" } else { "#5a4425" };
    let pill_fg = if status_ok { "#c6f5df" } else { "#ffe0ad" };

    format!(
        r##"<svg xmlns="http://www.w3.org/2000/svg" width="720" height="420" viewBox="0 0 720 420">
  <defs>
    <style>
      @font-face {{
        font-family: 'QuotaCJK';
        src: url('file:///usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc') format('truetype');
        font-weight: 400;
      }}
      @font-face {{
        font-family: 'QuotaCJK';
        src: url('file:///usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc') format('truetype');
        font-weight: 700 900;
      }}
    </style>
    <linearGradient id="bg" x1="0" x2="1" y1="0" y2="1">
      <stop offset="0" stop-color="#12161c"/>
      <stop offset="1" stop-color="#20272d"/>
    </linearGradient>
    <linearGradient id="bar" x1="0" x2="1">
      <stop offset="0" stop-color="#58d39c"/>
      <stop offset="1" stop-color="#6ab8ff"/>
    </linearGradient>
    <filter id="shadow" x="-10%" y="-10%" width="120%" height="120%">
      <feDropShadow dx="0" dy="18" stdDeviation="18" flood-color="#000000" flood-opacity="0.35"/>
    </filter>
  </defs>
  <rect x="22" y="22" width="676" height="376" rx="24" fill="url(#bg)" stroke="#3d4650" filter="url(#shadow)"/>
  <circle cx="62" cy="62" r="7" fill="{dot}"/>
  <text x="80" y="68" fill="#dce4ee" font-family="QuotaCJK, Noto Sans CJK SC, Noto Sans CJK, Segoe UI, Arial, sans-serif" font-size="20" font-weight="700">Codex Quota</text>
  <rect x="558" y="42" width="102" height="36" rx="18" fill="{pill_bg}" stroke="#4f5963"/>
  <text x="609" y="66" text-anchor="middle" fill="{pill_fg}" font-family="QuotaCJK, Noto Sans CJK SC, Noto Sans CJK, Segoe UI, Arial, sans-serif" font-size="16" font-weight="700">{status}</text>

  <text x="54" y="132" fill="#9ba9b9" font-family="QuotaCJK, Noto Sans CJK SC, Noto Sans CJK, Segoe UI, Arial, sans-serif" font-size="18">5 小时剩余</text>
  <text x="54" y="196" fill="#f8fbff" font-family="QuotaCJK, Noto Sans CJK SC, Noto Sans CJK, Segoe UI, Arial, sans-serif" font-size="62" font-weight="800">{primary_remaining_text}</text>
  <rect x="54" y="226" width="612" height="18" rx="9" fill="#303841"/>
  <rect x="54" y="226" width="{primary_width}" height="18" rx="9" fill="url(#bar)"/>

  <g font-family="QuotaCJK, Noto Sans CJK SC, Noto Sans CJK, Segoe UI, Arial, sans-serif" font-size="17">
    <text x="54" y="292" fill="#9ba9b9">5 小时已用</text>
    <text x="226" y="292" fill="#f0f5fb" font-weight="700">{primary_used}</text>
    <text x="390" y="292" fill="#9ba9b9">重置</text>
    <text x="474" y="292" fill="#f0f5fb" font-weight="700">{primary_reset}</text>

    <text x="54" y="332" fill="#9ba9b9">周额度剩余</text>
    <text x="226" y="332" fill="#f0f5fb" font-weight="700">{secondary_remaining_text}</text>
    <text x="390" y="332" fill="#9ba9b9">周重置</text>
    <text x="474" y="332" fill="#f0f5fb" font-weight="700">{secondary_reset}</text>

    <text x="54" y="372" fill="#9ba9b9">计划</text>
    <text x="226" y="372" fill="#f0f5fb" font-weight="700">{plan}</text>
    <text x="390" y="372" fill="#9ba9b9">更新</text>
    <text x="474" y="372" fill="#f0f5fb" font-weight="700">{updated}</text>
  </g>
</svg>
"##,
        dot = dot,
        pill_bg = pill_bg,
        pill_fg = pill_fg,
        status = escape(status),
        primary_remaining_text = format_percent(snapshot.primary_remaining_percent),
        primary_width = (612.0 * primary_remaining / 100.0).round(),
        primary_used = format_percent(snapshot.primary_used_percent),
        primary_reset = escape(short_time(snapshot.primary_resets_at.as_deref()).as_str()),
        secondary_remaining_text = format_percent(snapshot.secondary_remaining_percent),
        secondary_reset = escape(short_time(snapshot.secondary_resets_at.as_deref()).as_str()),
        plan = escape(snapshot.plan_type.as_deref().unwrap_or("--")),
        updated = escape(short_time(snapshot.captured_at.as_deref().or(snapshot.observed_at.as_deref())).as_str()),
    )
}

fn format_percent(value: Option<f64>) -> String {
    value
        .map(|number| {
            if number < 10.0 {
                format!("{number:.1}%")
            } else {
                format!("{number:.0}%")
            }
        })
        .unwrap_or_else(|| "--".to_string())
}

fn short_time(value: Option<&str>) -> String {
    value
        .map(|value| value.replace('T', " ").chars().take(16).collect())
        .unwrap_or_else(|| "--".to_string())
}

fn escape(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}
