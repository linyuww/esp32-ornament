use quota_core::{get_quota_snapshot, notification_signature, read_state, state_path, summary_text, write_state};
use std::{
    env,
    process::Command,
    thread,
    time::Duration,
};

fn main() {
    let interval = env::var("POLL_INTERVAL_SECONDS")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .filter(|value| *value > 0)
        .unwrap_or(30);
    let state_path = state_path();
    let mut last_signature = read_state(&state_path)
        .ok()
        .flatten()
        .map(|snapshot| notification_signature(&snapshot));

    loop {
        let snapshot = get_quota_snapshot();
        let signature = notification_signature(&snapshot);

        if let Err(error) = write_state(&state_path, &snapshot) {
            eprintln!("failed to write quota state {}: {error}", state_path.display());
        }

        if let Some(previous) = &last_signature {
            if previous != &signature {
                notify_openclaw(&summary_text(&snapshot));
            }
        }
        last_signature = Some(signature);

        thread::sleep(Duration::from_secs(interval));
    }
}

fn notify_openclaw(message: &str) {
    let bin = env::var("OPENCLAW_BIN").unwrap_or_else(|_| "openclaw".to_string());
    let target = env::var("OPENCLAW_TARGET").unwrap_or_else(|_| "default".to_string());
    let channel = env::var("OPENCLAW_CHANNEL").ok().filter(|value| !value.is_empty());

    let mut command = Command::new(bin);
    command.args(["message", "send"]);
    if let Some(channel) = channel {
        command.args(["--channel", &channel]);
    }
    command.args(["--target", &target, "--message", message]);

    match command.status() {
        Ok(status) if status.success() => {}
        Ok(status) => eprintln!("openclaw message send exited with {status}"),
        Err(error) => eprintln!("failed to run openclaw message send: {error}"),
    }
}

