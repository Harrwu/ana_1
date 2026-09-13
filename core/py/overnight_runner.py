#!/usr/bin/env python3
"""
overnight_runner.py

Production-oriented supervisor for the ANA C++ trading pipeline.

Important integration detail:
The current ai_server in this codebase does NOT implement --ingest-only (or a
runtime "enable trading" command). It decides whether broker trading is enabled
once, at startup, solely from APCA_API_KEY_ID/APCA_API_SECRET_KEY.

Therefore Phase A starts ai_server with the Alpaca credentials deliberately
removed from its child environment. It still scores with Ollama and appends
trading_signals.csv, but the broker object is absent so no orders can be sent.
At market open the runner briefly SIGSTOPs the crawler, terminates the
ingest-only ai_server, starts a fresh ai_server with credentials, waits for
58888, and SIGCONT's the crawler. This gives an actual broker-routing gate
without modifying the C++ binary.

Usage:
    python3 overnight_runner.py
    python3 overnight_runner.py --now
    python3 overnight_runner.py --dry-run
    python3 overnight_runner.py --now --dry-run
    python3 overnight_runner.py --crypto

--crypto runs the pipeline 24/7 against Alpaca crypto instead of the US
equity session. Crypto markets never close, so the whole clock-driven
schedule is bypassed: there is no warmup window, no open gate, and no
next_close to wait for. The broker is live from the first second and the
process runs until interrupted.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import pty
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Optional
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
from zoneinfo import ZoneInfo


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

ROOT = Path(__file__).resolve().parent
LOG_DIR = ROOT / "logs"

OLLAMA_HOST = "127.0.0.1"
OLLAMA_PORT = 11434
AI_HOST = "127.0.0.1"
AI_PORT = 58888

OLLAMA_MODEL = "llama3.1"
ALPACA_TRADING_URL = "https://paper-api.alpaca.markets"
ALPACA_CLOCK_PATH = "/v2/clock"
ALPACA_ACCOUNT_PATH = "/v2/account"

MARKET_TZ = ZoneInfo("America/New_York")
WARMUP_SECONDS = 10 * 60

PROCESS_START_TIMEOUT = 15.0
SHUTDOWN_TIMEOUT = 20.0
AI_SHUTDOWN_TIMEOUT = 10.0
CLOCK_REFRESH_SECONDS = 60.0
SLEEP_GRANULARITY_SECONDS = 1.0

BUILD_PARALLELISM = max(1, os.cpu_count() or 2)

AI_CANDIDATES = [
    ROOT / "build" / "aiserv",
    ROOT / "build" / "ai_server",
    ROOT / "aiserv",
    ROOT / "ai_server",
]

CRAWLER_CANDIDATES = [
    ROOT / "build" / "main_client",
    ROOT / "build" / "test_client",
    ROOT / "build" / "ana_crawler",
    ROOT / "main_client",
    ROOT / "test_client",
    ROOT / "ana_crawler",
]

SIGNALS_FILE = ROOT / "trading_signals.csv"
ORDERS_FILE = ROOT / "orders.log"
HISTORY_FILE = ROOT / "history.txt"


# ---------------------------------------------------------------------------
# Runtime state
# ---------------------------------------------------------------------------

stop_event = threading.Event()
state_lock = threading.Lock()

session_log: Optional[Path] = None
log_lock = threading.Lock()

ollama_proc: Optional[subprocess.Popen] = None
ai_proc: Optional[subprocess.Popen] = None
crawler_proc: Optional[subprocess.Popen] = None

crawler_was_stopped = False

stats = {
    "articles_scraped": 0,
    "signals_generated": 0,
    "trades_placed": 0,
    "ai_batches": 0,
    "crawler_visits": 0,
}


# ---------------------------------------------------------------------------
# Logging / telemetry
# ---------------------------------------------------------------------------

def setup_logging() -> None:
    global session_log
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    session_log = LOG_DIR / f"session_{dt.datetime.now(MARKET_TZ):%Y-%m-%d}.log"


def emit(message: str) -> None:
    """Write one line to terminal and the session log."""
    line = f"{dt.datetime.now(MARKET_TZ):%Y-%m-%d %H:%M:%S %Z} {message}"
    print(line, flush=True)
    if session_log is not None:
        with log_lock:
            with session_log.open("a", encoding="utf-8") as fh:
                fh.write(line + "\n")


def classify_ai_line(line: str) -> str:
    upper = line.upper()
    if (
        "[AI BROKER]" in upper
        or "[BROKER]" in upper
        or "FILLED" in upper
        or "REJECTED" in upper
        or "ORDER" in upper
    ):
        return "[BROKER TRADE]"
    return "[AI SERVER]"


def stream_process_output(proc: subprocess.Popen, prefix: str) -> None:
    """Tee a child's combined stdout/stderr into terminal + session log."""
    if proc.stdout is None:
        return

    try:
        for raw in iter(proc.stdout.readline, ""):
            if raw == "":
                break

            line = raw.rstrip("\r\n")
            if not line:
                continue

            if prefix == "[AI SERVER]":
                actual_prefix = classify_ai_line(line)
            else:
                actual_prefix = prefix

            emit(f"{actual_prefix} {line}")

            upper = line.upper()
            with state_lock:
                if prefix == "[CRAWLER]":
                    if "[CRAWLER] VISITING:" in upper:
                        stats["crawler_visits"] += 1
                    if "[DEBUG] URL:" in upper:
                        stats["articles_scraped"] += 1
                elif prefix == "[AI SERVER]":
                    if "[AI BATCH START]" in upper:
                        stats["ai_batches"] += 1
                    if " FILLED " in f" {upper} " or "| FILLED |" in upper:
                        stats["trades_placed"] += 1
    except (ValueError, OSError):
        pass
    finally:
        try:
            proc.stdout.close()
        except Exception:
            pass


def attach_telemetry(proc: subprocess.Popen, prefix: str) -> None:
    thread = threading.Thread(
        target=stream_process_output,
        args=(proc, prefix),
        daemon=True,
        name=f"telemetry-{prefix.strip('[]').lower()}",
    )
    thread.start()


# ---------------------------------------------------------------------------
# Process helpers
# ---------------------------------------------------------------------------

def popen_env(with_broker_credentials: bool) -> dict[str, str]:
    env = os.environ.copy()

    if not with_broker_credentials:
        # The C++ server uses the *presence* of these variables to instantiate
        # AlpacaBroker. Remove them rather than setting dummy values.
        env.pop("APCA_API_KEY_ID", None)
        env.pop("APCA_API_SECRET_KEY", None)

    return env


def process_group_signal(proc: Optional[subprocess.Popen], sig: int) -> bool:
    if proc is None or proc.poll() is not None:
        return False

    try:
        os.killpg(proc.pid, sig)
        return True
    except ProcessLookupError:
        return False
    except PermissionError as exc:
        emit(f"[ORCHESTRATOR] Permission error signalling PID {proc.pid}: {exc}")
        return False


def process_running(proc: Optional[subprocess.Popen]) -> bool:
    return proc is not None and proc.poll() is None


def wait_for_port(
    host: str,
    port: int,
    timeout: float,
    *,
    want_open: bool = True,
) -> bool:
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        open_now = False
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        try:
            sock.connect((host, port))
            open_now = True
        except OSError:
            open_now = False
        finally:
            sock.close()

        if open_now == want_open:
            return True

        time.sleep(0.2)

    return False


def start_process(
    argv: list[str],
    *,
    env: Optional[dict[str, str]],
    prefix: str,
) -> subprocess.Popen:
    """Start a child with terminal-like stdout/stderr buffering.

    The crawler and AI server are C++ programs. When stdout is connected to
    subprocess.PIPE, libc/libstdc++ may buffer output differently than when
    running directly in a terminal. A PTY makes the child believe it is
    writing to a terminal, so normal line-oriented output appears immediately
    instead of arriving in a burst when the process shuts down.
    """
    command = [str(x) for x in argv]
    emit(f"[ORCHESTRATOR] Starting: {' '.join(command)}")

    master_fd, slave_fd = pty.openpty()
    try:
        proc = subprocess.Popen(
            command,
            cwd=str(ROOT),
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=slave_fd,
            stderr=slave_fd,
            start_new_session=True,
            close_fds=True,
        )
    except Exception:
        os.close(master_fd)
        os.close(slave_fd)
        raise
    finally:
        # The parent only needs the PTY master. The child owns the slave.
        try:
            os.close(slave_fd)
        except OSError:
            pass

    # stream_process_output() expects proc.stdout to expose readline().
    # os.fdopen gives us a normal text stream over the PTY master.
    proc.stdout = os.fdopen(
        master_fd,
        "r",
        encoding="utf-8",
        errors="replace",
        buffering=1,
    )

    attach_telemetry(proc, prefix)
    return proc


def graceful_stop(
    proc: Optional[subprocess.Popen],
    name: str,
    *,
    timeout: float = SHUTDOWN_TIMEOUT,
) -> None:
    if proc is None or proc.poll() is not None:
        return

    emit(f"[ORCHESTRATOR] Sending SIGTERM to {name} (pid={proc.pid})")
    process_group_signal(proc, signal.SIGTERM)

    try:
        proc.wait(timeout=timeout)
        emit(
            f"[ORCHESTRATOR] {name} exited with code {proc.returncode}"
        )
        return
    except subprocess.TimeoutExpired:
        emit(
            f"[ORCHESTRATOR] {name} did not exit after {timeout:.0f}s; "
            "sending SIGKILL"
        )

    process_group_signal(proc, signal.SIGKILL)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        emit(f"[ORCHESTRATOR] WARNING: {name} still has not exited")


# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------

def require_credentials() -> None:
    missing = [
        name
        for name in ("APCA_API_KEY_ID", "APCA_API_SECRET_KEY")
        if not os.environ.get(name)
    ]
    if missing:
        raise RuntimeError(
            "Missing required environment variable(s): "
            + ", ".join(missing)
            + ". Export your Alpaca PAPER credentials before starting."
        )


def ensure_ollama() -> None:
    global ollama_proc

    if wait_for_port(OLLAMA_HOST, OLLAMA_PORT, 0.5):
        emit("[ORCHESTRATOR] Ollama is already listening on 127.0.0.1:11434")
        return

    ollama = shutil_which("ollama")
    if ollama is None:
        raise RuntimeError(
            "Ollama is not listening on 127.0.0.1:11434 and the "
            "'ollama' executable was not found in PATH."
        )

    emit("[ORCHESTRATOR] Ollama is not listening; spawning 'ollama serve'")
    ollama_proc = start_process(
        [ollama, "serve"],
        env=os.environ.copy(),
        prefix="[OLLAMA]",
    )

    if not wait_for_port(OLLAMA_HOST, OLLAMA_PORT, PROCESS_START_TIMEOUT):
        graceful_stop(ollama_proc, "Ollama", timeout=5)
        ollama_proc = None
        raise RuntimeError(
            "Started 'ollama serve' but 127.0.0.1:11434 did not open "
            f"within {PROCESS_START_TIMEOUT:.0f}s."
        )

    emit("[ORCHESTRATOR] Ollama is ready")


def shutil_which(command: str) -> Optional[str]:
    # Kept local so the script has no third-party dependency.
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        if not directory:
            continue
        candidate = Path(directory) / command
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


def verify_ollama_model() -> None:
    """Fail early if llama3.1 is not installed instead of discovering at open."""
    try:
        data = http_json(
            f"http://{OLLAMA_HOST}:{OLLAMA_PORT}/api/tags",
            method="GET",
            timeout=5,
        )
    except Exception as exc:
        raise RuntimeError(f"Could not query Ollama /api/tags: {exc}") from exc

    models = data.get("models", [])
    names = set()
    for model in models:
        name = str(model.get("name", ""))
        if name:
            names.add(name)
            names.add(name.split(":")[0])

    if OLLAMA_MODEL not in names and f"{OLLAMA_MODEL}:latest" not in names:
        raise RuntimeError(
            f"Ollama is running, but model '{OLLAMA_MODEL}' is not installed. "
            f"Installed models: {', '.join(sorted(names)) or '(none)'}"
        )

    emit(f"[ORCHESTRATOR] Ollama model '{OLLAMA_MODEL}' is installed")


def find_binary(candidates: tuple[Path, ...]) -> Optional[Path]:
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def build_if_missing() -> tuple[Path, Path]:
    ai = find_binary(AI_CANDIDATES)
    crawler = find_binary(CRAWLER_CANDIDATES)

    if ai and crawler:
        emit(
            f"[ORCHESTRATOR] C++ binaries ready: AI={ai.relative_to(ROOT)}, "
            f"CRAWLER={crawler.relative_to(ROOT)}"
        )
        return ai, crawler

    cmake = shutil_which("cmake")
    if cmake is None:
        missing = []
        if ai is None:
            missing.append("ai_server")
        if crawler is None:
            missing.append("crawler")
        raise RuntimeError(
            "Missing C++ executable(s): "
            + ", ".join(missing)
            + " and CMake is not installed/in PATH."
        )

    cmake_file = ROOT / "CMakeLists.txt"
    if not cmake_file.is_file():
        missing = []
        if ai is None:
            missing.append("ai_server")
        if crawler is None:
            missing.append("test_client/ana_crawler")
        raise RuntimeError(
            "Missing C++ executable(s): "
            + ", ".join(missing)
            + ". No CMakeLists.txt was found at "
            + str(cmake_file)
            + ", so the runner cannot safely infer how this repository "
              "should be compiled."
        )

    build_dir = ROOT / "build"

    emit("[ORCHESTRATOR] C++ binary missing; configuring CMake")
    run_checked(
        [cmake, "-S", str(ROOT), "-B", str(build_dir)],
        "CMake configure",
    )

    emit(
        f"[ORCHESTRATOR] Building C++ pipeline with {BUILD_PARALLELISM} jobs"
    )
    run_checked(
        [
            cmake,
            "--build",
            str(build_dir),
            "--parallel",
            str(BUILD_PARALLELISM),
        ],
        "C++ build",
    )

    ai = find_binary(AI_CANDIDATES)
    crawler = find_binary(CRAWLER_CANDIDATES)

    if ai is None or crawler is None:
        raise RuntimeError(
            "CMake completed, but expected executables are still missing. "
            f"AI candidates: {', '.join(str(x) for x in AI_CANDIDATES)}; "
            f"crawler candidates: {', '.join(str(x) for x in CRAWLER_CANDIDATES)}"
        )

    emit(
        f"[ORCHESTRATOR] Build complete: AI={ai.relative_to(ROOT)}, "
        f"CRAWLER={crawler.relative_to(ROOT)}"
    )
    return ai, crawler


def run_checked(argv: list[str], name: str) -> None:
    try:
        result = subprocess.run(
            [str(x) for x in argv],
            cwd=str(ROOT),
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except OSError as exc:
        raise RuntimeError(f"{name} failed to start: {exc}") from exc

    if result.stdout:
        for line in result.stdout.splitlines():
            emit(f"[ORCHESTRATOR] {line}")

    if result.returncode != 0:
        raise RuntimeError(
            f"{name} failed with exit code {result.returncode}"
        )


# ---------------------------------------------------------------------------
# Alpaca clock / account API
# ---------------------------------------------------------------------------

def http_json(
    url: str,
    *,
    method: str = "GET",
    timeout: float = 10,
) -> dict:
    key = os.environ.get("APCA_API_KEY_ID")
    secret = os.environ.get("APCA_API_SECRET_KEY")

    if not key or not secret:
        raise RuntimeError("Alpaca credentials are not available")

    req = Request(
        url,
        method=method,
        headers={
            "APCA-API-KEY-ID": key,
            "APCA-API-SECRET-KEY": secret,
            "Accept": "application/json",
            "User-Agent": "overnight_runner/1.0",
        },
    )

    try:
        with urlopen(req, timeout=timeout) as response:
            raw = response.read().decode("utf-8")
            return json.loads(raw)
    except HTTPError as exc:
        body = ""
        try:
            body = exc.read().decode("utf-8", errors="replace")
        except Exception:
            pass
        raise RuntimeError(
            f"Alpaca HTTP {exc.code} for {url}: {body[:300]}"
        ) from exc
    except URLError as exc:
        raise RuntimeError(f"Alpaca request failed for {url}: {exc}") from exc


def parse_rfc3339(value: str) -> dt.datetime:
    # Alpaca emits ISO/RFC3339 timestamps. Normalize Z for datetime.fromisoformat.
    return dt.datetime.fromisoformat(value.replace("Z", "+00:00"))


def get_alpaca_clock() -> dict:
    data = http_json(ALPACA_TRADING_URL + ALPACA_CLOCK_PATH)

    required = ("is_open", "next_open", "next_close")
    missing = [key for key in required if key not in data]
    if missing:
        raise RuntimeError(
            "Alpaca /v2/clock response missing: " + ", ".join(missing)
        )

    return {
        "is_open": bool(data["is_open"]),
        "next_open": parse_rfc3339(str(data["next_open"])).astimezone(MARKET_TZ),
        "next_close": parse_rfc3339(str(data["next_close"])).astimezone(MARKET_TZ),
    }


def get_final_equity() -> Optional[float]:
    try:
        account = http_json(ALPACA_TRADING_URL + ALPACA_ACCOUNT_PATH)
        value = account.get("equity")
        return float(value) if value is not None else None
    except Exception as exc:
        emit(f"[ORCHESTRATOR] Could not read final Alpaca equity: {exc}")
        return None


# ---------------------------------------------------------------------------
# Schedule
# ---------------------------------------------------------------------------

def fmt_market_time(value: dt.datetime) -> str:
    return value.astimezone(MARKET_TZ).strftime("%-I:%M %p %Z")


def sleep_until(target: dt.datetime, label: str) -> bool:
    """Low-overhead interruptible countdown loop."""
    last_print_second: Optional[int] = None

    while not stop_event.is_set():
        now = dt.datetime.now(MARKET_TZ)
        remaining = (target - now).total_seconds()

        if remaining <= 0:
            return True

        total_seconds = int(remaining)
        if total_seconds != last_print_second and (
            total_seconds < 60 or total_seconds % 30 == 0
        ):
            hours, rem = divmod(total_seconds, 3600)
            minutes, seconds = divmod(rem, 60)

            if hours:
                human = f"{hours} hours, {minutes} minutes"
            elif minutes:
                human = f"{minutes} minutes, {seconds} seconds"
            else:
                human = f"{seconds} seconds"

            emit(
                f"[ORCHESTRATOR] [WAIT] Sleeping until {label} at "
                f"{fmt_market_time(target)} ({human} remaining)..."
            )
            last_print_second = total_seconds

        stop_event.wait(
            min(SLEEP_GRANULARITY_SECONDS, max(0.05, remaining))
        )

    return False


def wait_for_warmup_or_open(clock: dict) -> str:
    """
    Return:
      'warmup'  -> start Phase A now
      'open'    -> market is already open
      'stopped' -> shutdown requested
    """
    if clock["is_open"]:
        return "open"

    next_open = clock["next_open"]
    warmup = next_open - dt.timedelta(seconds=WARMUP_SECONDS)
    now = dt.datetime.now(MARKET_TZ)

    if now < warmup:
        if not sleep_until(
            warmup,
            "pre-market warmup",
        ):
            return "stopped"
        return "warmup"

    if now < next_open:
        emit(
            "[ORCHESTRATOR] Current time is already inside the "
            "10-minute pre-market window; starting warmup immediately."
        )
        return "warmup"

    # The clock should normally say is_open=True here. Refresh to handle a
    # clock rollover/race rather than guessing.
    refreshed = get_alpaca_clock()
    if refreshed["is_open"]:
        return "open"

    return "warmup"


# ---------------------------------------------------------------------------
# Session statistics
# ---------------------------------------------------------------------------

def file_size(path: Path) -> int:
    try:
        return path.stat().st_size
    except FileNotFoundError:
        return 0


def count_new_csv_signals(start_offset: int) -> int:
    if not SIGNALS_FILE.is_file():
        return 0

    try:
        with SIGNALS_FILE.open("rb") as fh:
            fh.seek(start_offset)
            data = fh.read()
    except OSError:
        return 0

    count = 0
    for line in data.decode("utf-8", errors="replace").splitlines():
        if line.strip():
            # Header can only occur at offset zero, but ignore it defensively.
            if line.lower().startswith("timestamp,"):
                continue
            parts = next(csv.reader([line]), [])
            if len(parts) >= 3:
                count += 1
    return count


def count_new_filled_orders(start_offset: int) -> int:
    if not ORDERS_FILE.is_file():
        return 0

    try:
        with ORDERS_FILE.open("rb") as fh:
            fh.seek(start_offset)
            data = fh.read()
    except OSError:
        return 0

    return sum(
        1
        for line in data.decode("utf-8", errors="replace").splitlines()
        if "| FILLED |" in line.upper()
    )


# ---------------------------------------------------------------------------
# Pipeline phases
# ---------------------------------------------------------------------------

def start_ai(ai_binary: Path, *, active_broker: bool, dry_run: bool) -> None:
    global ai_proc

    if process_running(ai_proc):
        raise RuntimeError("AI server is already running")

    # Dry-run always disables broker credentials in the C++ child.
    enable_broker = active_broker and not dry_run
    env = popen_env(enable_broker)

    if active_broker:
        mode = "ACTIVE BROKER ROUTING" if enable_broker else "DRY-RUN"
    else:
        mode = "INGEST-ONLY (broker credentials withheld)"

    emit(f"[ORCHESTRATOR] Phase AI mode: {mode}")

    ai_proc = start_process(
        [ai_binary],
        env=env,
        prefix="[AI SERVER]",
    )

    if not wait_for_port(AI_HOST, AI_PORT, PROCESS_START_TIMEOUT):
        graceful_stop(ai_proc, "AI server", timeout=AI_SHUTDOWN_TIMEOUT)
        ai_proc = None
        raise RuntimeError(
            "AI server did not open 127.0.0.1:58888 within "
            f"{PROCESS_START_TIMEOUT:.0f}s."
        )

    emit("[ORCHESTRATOR] AI server is listening on 127.0.0.1:58888")


def start_crawler(crawler_binary: Path) -> None:
    global crawler_proc

    if process_running(crawler_proc):
        raise RuntimeError("Crawler is already running")

    crawler_proc = start_process(
        [crawler_binary],
        env=os.environ.copy(),
        prefix="[CRAWLER]",
    )
    emit(f"[ORCHESTRATOR] Crawler started (pid={crawler_proc.pid})")


def activate_broker_at_open(
    ai_binary: Path,
    *,
    dry_run: bool,
) -> None:
    """
    Atomically-ish switch the C++ server from no-broker scoring to broker mode.

    The crawler is SIGSTOP'd during the small restart window because the current
    crawler sends articles directly to TCP/58888 and has no retry queue for an
    unavailable AI server.
    """
    global crawler_was_stopped, ai_proc

    if process_running(crawler_proc):
        emit(
            "[ORCHESTRATOR] Pausing crawler briefly while AI routing is "
            "switched at market open"
        )
        if process_group_signal(crawler_proc, signal.SIGSTOP):
            crawler_was_stopped = True

    graceful_stop(ai_proc, "AI server (warmup instance)", timeout=AI_SHUTDOWN_TIMEOUT)
    ai_proc = None

    if not wait_for_port(AI_HOST, AI_PORT, 5.0, want_open=False):
        raise RuntimeError(
            "Warmup AI server did not release 127.0.0.1:58888 promptly."
        )

    start_ai(
        ai_binary,
        active_broker=True,
        dry_run=dry_run,
    )

    if crawler_was_stopped and process_running(crawler_proc):
        emit("[ORCHESTRATOR] Resuming crawler at market open")
        try:
            os.killpg(crawler_proc.pid, signal.SIGCONT)
        except ProcessLookupError:
            emit(
                "[ORCHESTRATOR] Crawler exited during the open transition; "
                "it will not be restarted automatically."
            )
        crawler_was_stopped = False


def wait_for_market_close(initial_clock: dict) -> None:
    """
    Stay alive through the trading session and refresh Alpaca's clock so an
    exchange early close or schedule change is not silently ignored.
    """
    next_close = initial_clock["next_close"]
    next_refresh = time.monotonic() + CLOCK_REFRESH_SECONDS

    emit(
        f"[ORCHESTRATOR] Active session until {fmt_market_time(next_close)}"
    )

    while not stop_event.is_set():
        now = dt.datetime.now(MARKET_TZ)
        if now >= next_close:
            emit("[ORCHESTRATOR] Alpaca next_close reached")
            return

        # If either core process dies unexpectedly, fail closed instead of
        # pretending the overnight supervisor is still healthy.
        if ai_proc is not None and ai_proc.poll() is not None:
            raise RuntimeError(
                f"AI server exited unexpectedly with code {ai_proc.returncode}"
            )

        if crawler_proc is not None and crawler_proc.poll() is not None:
            emit(
                "[ORCHESTRATOR] WARNING: crawler exited unexpectedly with "
                f"code {crawler_proc.returncode}"
            )

        if time.monotonic() >= next_refresh:
            try:
                refreshed = get_alpaca_clock()
                next_close = refreshed["next_close"]

                if not refreshed["is_open"] and now < next_close:
                    emit(
                        "[ORCHESTRATOR] WARNING: Alpaca reports market closed "
                        "before the original close; shutting down safely."
                    )
                    return

                emit(
                    f"[ORCHESTRATOR] Clock refresh: close at "
                    f"{fmt_market_time(next_close)}"
                )
            except Exception as exc:
                # A transient clock failure must not immediately kill a live
                # paper session. The original close remains the fail-safe.
                emit(
                    f"[ORCHESTRATOR] WARNING: clock refresh failed: {exc}; "
                    "using last known close"
                )

            next_refresh = time.monotonic() + CLOCK_REFRESH_SECONDS

        remaining = (next_close - now).total_seconds()
        stop_event.wait(min(1.0, max(0.05, remaining)))


# ---------------------------------------------------------------------------
# Shutdown / summary
# ---------------------------------------------------------------------------

def shutdown_pipeline() -> None:
    global crawler_proc, ai_proc, ollama_proc, crawler_was_stopped

    emit("[ORCHESTRATOR] Beginning graceful session shutdown")

    # Crawler first. Its SIGTERM handler requests crawlEngine::stop(), which
    # wakes workers and lets run() join them; finishShutdown() force-saves
    # history.txt. Give it enough time for slow HTTP requests to return.
    if crawler_proc is not None:
        if crawler_was_stopped and process_running(crawler_proc):
            try:
                os.killpg(crawler_proc.pid, signal.SIGCONT)
            except ProcessLookupError:
                pass
            crawler_was_stopped = False

        graceful_stop(
            crawler_proc,
            "crawler",
            timeout=SHUTDOWN_TIMEOUT,
        )
        crawler_proc = None

    # Then AI. Any detached per-article worker threads die with this process.
    if ai_proc is not None:
        graceful_stop(
            ai_proc,
            "AI server",
            timeout=AI_SHUTDOWN_TIMEOUT,
        )
        ai_proc = None

    # Ollama is a local service and is intentionally left running. If the
    # runner spawned it, it is still useful to other local workloads and can
    # be supervised by systemd/user services independently.
    if ollama_proc is not None and ollama_proc.poll() is not None:
        emit(
            f"[ORCHESTRATOR] Runner-spawned Ollama exited with "
            f"code {ollama_proc.returncode}"
        )


def print_summary(
    signals_offset: int,
    orders_offset: int,
    *,
    dry_run: bool,
) -> None:
    csv_signals = count_new_csv_signals(signals_offset)
    filled_orders = count_new_filled_orders(orders_offset)

    # Prefer durable files for the final totals; in-memory telemetry catches
    # lines that may not yet have made it to the files.
    with state_lock:
        articles = stats["articles_scraped"]
        batches = stats["ai_batches"]
        live_trades = stats["trades_placed"]

    signals = csv_signals
    trades = max(filled_orders, live_trades)

    with state_lock:
        stats["signals_generated"] = signals
        stats["trades_placed"] = trades

    equity = get_final_equity()

    emit("[ORCHESTRATOR] ================= SESSION SUMMARY =================")
    emit(f"[ORCHESTRATOR] Articles scraped : {articles}")
    emit(f"[ORCHESTRATOR] AI batches       : {batches}")
    emit(f"[ORCHESTRATOR] Signals generated: {signals}")
    emit(f"[ORCHESTRATOR] Trades placed    : {trades}")
    if dry_run:
        emit("[ORCHESTRATOR] Trading mode     : DRY-RUN (no orders sent)")
    else:
        emit("[ORCHESTRATOR] Trading mode     : ALPACA PAPER")
    if equity is None:
        emit("[ORCHESTRATOR] Final equity     : unavailable")
    else:
        emit(f"[ORCHESTRATOR] Final equity     : ${equity:,.2f}")
    emit(
        f"[ORCHESTRATOR] history.txt      : "
        f"{file_size(HISTORY_FILE):,} bytes"
    )
    emit("[ORCHESTRATOR] =====================================================")


# ---------------------------------------------------------------------------
# Signals
# ---------------------------------------------------------------------------

def handle_signal(signum: int, _frame) -> None:
    # Async-signal-safe action: only set an Event. No logging, joins, sockets,
    # or subprocess calls from the POSIX signal handler.
    stop_event.set()


def install_signal_handlers() -> None:
    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Unattended ANA C++ trading pipeline supervisor"
    )
    parser.add_argument(
        "--now",
        action="store_true",
        help="Skip Alpaca scheduling and start the active pipeline immediately",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Never give broker credentials to ai_server; no orders can be placed",
    )
    parser.add_argument(
        "--crypto",
        action="store_true",
        help=(
            "Run 24/7 against Alpaca crypto. Bypasses market-clock "
            "synchronization entirely (no warmup, no open gate, no close); "
            "broker routing is live from startup."
        ),
    )
    return parser.parse_args()


def main() -> int:
    global ai_proc, crawler_proc

    args = parse_args()

    setup_logging()
    install_signal_handlers()

    signals_offset = file_size(SIGNALS_FILE)
    orders_offset = file_size(ORDERS_FILE)

    emit("[ORCHESTRATOR] =====================================================")
    emit("[ORCHESTRATOR] ANA overnight runner starting")
    emit(f"[ORCHESTRATOR] Repository root: {ROOT}")
    emit(f"[ORCHESTRATOR] Log file: {session_log}")
    emit(
        "[ORCHESTRATOR] Mode: "
        + ("DRY-RUN" if args.dry_run else "ALPACA PAPER")
        + (" [CRYPTO 24/7]" if args.crypto else "")
    )

    try:
        # Required even for --dry-run because Alpaca's authenticated clock and
        # account endpoints are the source of truth for session timing. The
        # account endpoint is ALSO what the C++ sizing math reads for equity,
        # so credentials are needed on the crypto path too.
        require_credentials()

        ensure_ollama()
        verify_ollama_model()
        ai_binary, crawler_binary = build_if_missing()

        if args.crypto:
            # -----------------------------------------------------------------
            # CRYPTO: fully clock-independent
            #
            # /v2/clock describes the US EQUITY session. Crypto trades 24/7,
            # so consulting it would be actively wrong -- it would park the
            # pipeline until a 09:30 ET bell that means nothing to BTC/USD,
            # and shut it down at a 16:00 close that likewise doesn't apply.
            #
            # There is therefore no warmup phase, no open gate, and no close.
            # The broker is live immediately: the Phase A / Phase B split
            # exists only to defer equity exposure until the open, and with no
            # open to wait for it is pure delay.
            # -----------------------------------------------------------------
            emit(
                "[ORCHESTRATOR] --crypto supplied: bypassing US market clock "
                "and running 24/7"
            )
            emit(
                "[ORCHESTRATOR] NOTE: crypto orders carry no broker-side "
                "bracket; stops are software-managed via stop_limit"
            )

            start_ai(
                ai_binary,
                active_broker=True,
                dry_run=args.dry_run,
            )
            start_crawler(crawler_binary)

            # No session boundary to wait for, so the only exit conditions are
            # an explicit interrupt or a core process dying. Fail closed on
            # either rather than idling on a dead child.
            while not stop_event.is_set():
                if ai_proc is not None and ai_proc.poll() is not None:
                    raise RuntimeError(
                        f"AI server exited unexpectedly with code "
                        f"{ai_proc.returncode}"
                    )
                if crawler_proc is not None and crawler_proc.poll() is not None:
                    emit(
                        "[ORCHESTRATOR] WARNING: crawler exited with code "
                        f"{crawler_proc.returncode}; restarting is left to a "
                        "supervisor since crawl continuity matters less for "
                        "crypto than for a fixed equity session"
                    )
                    crawler_proc = None
                stop_event.wait(1.0)

        elif args.now:
            emit(
                "[ORCHESTRATOR] --now supplied: skipping schedule and starting "
                "the active pipeline immediately"
            )

            start_ai(
                ai_binary,
                active_broker=True,
                dry_run=args.dry_run,
            )
            start_crawler(crawler_binary)

            # For --now there is no warmup/open transition to perform. Keep
            # running until Ctrl+C/SIGTERM because this is explicitly a test
            # mode, not a wall-clock market-session simulation.
            while not stop_event.is_set():
                if ai_proc is not None and ai_proc.poll() is not None:
                    raise RuntimeError(
                        f"AI server exited unexpectedly with code "
                        f"{ai_proc.returncode}"
                    )
                if crawler_proc is not None and crawler_proc.poll() is not None:
                    emit(
                        "[ORCHESTRATOR] WARNING: crawler exited with code "
                        f"{crawler_proc.returncode}"
                    )
                    crawler_proc = None
                stop_event.wait(1.0)

        else:
            clock = get_alpaca_clock()

            emit(
                f"[ORCHESTRATOR] Alpaca clock: "
                f"is_open={clock['is_open']}, "
                f"next_open={fmt_market_time(clock['next_open'])}, "
                f"next_close={fmt_market_time(clock['next_close'])}"
            )

            phase = wait_for_warmup_or_open(clock)

            if phase == "stopped":
                return 0

            if phase == "open":
                emit(
                    "[ORCHESTRATOR] Market is already open; starting active "
                    "AI server + crawler now"
                )
                start_ai(
                    ai_binary,
                    active_broker=True,
                    dry_run=args.dry_run,
                )
                start_crawler(crawler_binary)
                wait_for_market_close(clock)
            else:
                # Phase A: T-10m. Broker credentials are withheld from the C++
                # child, which makes its broker unique_ptr remain null.
                emit(
                    "[ORCHESTRATOR] ================= PHASE A ================="
                )
                emit(
                    "[ORCHESTRATOR] T-10m warmup: news ingestion + Ollama scoring"
                )

                start_ai(
                    ai_binary,
                    active_broker=False,
                    dry_run=True,
                )
                start_crawler(crawler_binary)

                next_open = clock["next_open"]
                if not sleep_until(
                    next_open,
                    "market open",
                ):
                    return 0

                # Phase B: exact open. Pause crawler for the tiny AI restart
                # window, then resume it after broker routing is live.
                emit(
                    "[ORCHESTRATOR] ================= PHASE B ================="
                )
                emit(
                    f"[ORCHESTRATOR] Market open reached at "
                    f"{fmt_market_time(next_open)}; enabling broker routing"
                )

                activate_broker_at_open(
                    ai_binary,
                    dry_run=args.dry_run,
                )

                # Use the original clock's next_close, then refresh periodically.
                wait_for_market_close(clock)

    except KeyboardInterrupt:
        stop_event.set()
        emit("[ORCHESTRATOR] Ctrl+C received")
    except Exception as exc:
        stop_event.set()
        emit(f"[ORCHESTRATOR] FATAL: {exc}")
        return_code = 1
    else:
        return_code = 0
    finally:
        shutdown_pipeline()
        print_summary(
            signals_offset,
            orders_offset,
            dry_run=args.dry_run,
        )

    emit(
        "[ORCHESTRATOR] Session complete "
        f"(exit={return_code})"
    )
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())

