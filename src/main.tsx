import React, { useCallback, useEffect, useMemo, useState } from "react";
import { createRoot } from "react-dom/client";
import { invoke } from "@tauri-apps/api/core";
import { getCurrentWindow } from "@tauri-apps/api/window";
import "./styles.css";

type SnapshotStatus =
  | "ok"
  | "no_data"
  | "parse_error"
  | "auth_required"
  | "request_failed";

type QuotaSnapshot = {
  status: SnapshotStatus;
  source: "codex-wham";
  sourceLabel?: string;
  webUrl?: string;
  limitId?: string;
  planType?: string;
  primaryUsedPercent?: number;
  primaryRemainingPercent?: number;
  primaryWindowMinutes?: number;
  primaryResetsAt?: string;
  secondaryUsedPercent?: number;
  secondaryRemainingPercent?: number;
  secondaryWindowMinutes?: number;
  secondaryResetsAt?: string;
  credits?: unknown;
  observedAt?: string;
  capturedAt?: string;
  error?: string;
};

const REFRESH_MS = 60_000;

function formatPercent(value?: number) {
  if (typeof value !== "number" || Number.isNaN(value)) return "--";
  return `${value.toFixed(value < 10 ? 1 : 0)}%`;
}

function formatWindow(minutes?: number) {
  if (!minutes) return "--";
  if (minutes < 60) return `${minutes} 分钟`;
  if (minutes % 1440 === 0) return `${minutes / 1440} 天`;
  if (minutes % 60 === 0) return `${minutes / 60} 小时`;
  return `${minutes} 分钟`;
}

function formatTime(value?: string) {
  if (!value) return "--";
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return value;
  return new Intl.DateTimeFormat("zh-CN", {
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
  }).format(date);
}

function statusMessage(snapshot: QuotaSnapshot | null) {
  if (!snapshot) return "读取中";

  switch (snapshot.status) {
    case "ok":
      return "已同步";
    case "auth_required":
      return "需登录";
    case "no_data":
      return "无额度数据";
    case "request_failed":
      return "请求失败";
    case "parse_error":
      return "解析异常";
  }
}

function App() {
  const [snapshot, setSnapshot] = useState<QuotaSnapshot | null>(null);
  const [loading, setLoading] = useState(true);
  const [lastRefresh, setLastRefresh] = useState<Date | null>(null);

  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      const next = await invoke<QuotaSnapshot>("get_quota_snapshot");
      setSnapshot(next);
    } catch (error) {
      setSnapshot({
        status: "request_failed",
        source: "codex-wham",
        sourceLabel: "ChatGPT usage API",
        error: error instanceof Error ? error.message : String(error),
      });
    } finally {
      setLastRefresh(new Date());
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void refresh();
    const timer = window.setInterval(() => void refresh(), REFRESH_MS);
    return () => window.clearInterval(timer);
  }, [refresh]);

  const appWindow = getCurrentWindow();
  const remaining = snapshot?.primaryRemainingPercent;
  const weekly = snapshot?.secondaryRemainingPercent;
  const statusLabel = useMemo(() => statusMessage(snapshot), [snapshot]);
  const progress = Math.max(0, Math.min(100, remaining ?? 0));
  const capturedAt = snapshot?.capturedAt ?? snapshot?.observedAt;

  return (
    <main className="widget">
      <header className="titlebar" data-tauri-drag-region>
        <div className="identity" data-tauri-drag-region>
          <span className={`dot status-${snapshot?.status ?? "loading"}`} />
          <span data-tauri-drag-region>Codex Quota</span>
        </div>
        <div className="window-actions">
          <button title="刷新" onClick={() => void refresh()} disabled={loading}>
            ↻
          </button>
          <button title="最小化" onClick={() => void appWindow.minimize()}>
            _
          </button>
          <button title="关闭" onClick={() => void appWindow.close()}>
            ×
          </button>
        </div>
      </header>

      <section className={`quota-card status-${snapshot?.status ?? "loading"}`}>
        <div className="summary-row">
          <div>
            <div className="label">5 小时剩余</div>
            <div className="remaining">{formatPercent(remaining)}</div>
          </div>
          <div className="status-pill">{loading ? "刷新中" : statusLabel}</div>
        </div>

        <div className="meter" aria-label="Codex quota remaining">
          <div style={{ width: `${progress}%` }} />
        </div>

        <div className="details">
          <div>
            <span>短窗口</span>
            <strong>
              {formatPercent(remaining)} / {formatWindow(snapshot?.primaryWindowMinutes)}
            </strong>
          </div>
          <div>
            <span>周额度</span>
            <strong>{formatPercent(weekly)}</strong>
          </div>
          <div>
            <span>计划</span>
            <strong>{snapshot?.planType ?? "--"}</strong>
          </div>
          <div>
            <span>下次重置</span>
            <strong>{formatTime(snapshot?.primaryResetsAt)}</strong>
          </div>
        </div>

        {snapshot?.status !== "ok" && (
          <p className="message">
            {snapshot?.error ?? "无法读取 Codex 用量。请确认 Codex 已登录 ChatGPT。"}
          </p>
        )}

        <footer>
          <span>{snapshot?.sourceLabel ?? "ChatGPT usage API"}</span>
          <span>
            {capturedAt
              ? formatTime(capturedAt)
              : lastRefresh
                ? formatTime(lastRefresh.toISOString())
                : "--"}
          </span>
        </footer>
      </section>
    </main>
  );
}

createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
