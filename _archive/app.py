# app.py — Tank Water Triage Dashboard
# Single-file Flask backend. In-memory deque only. No DB. No ORM. No async.
# ESP32 (or mock) POSTs JSON over Wi-Fi; serial thread is gone entirely.

import os
import smtplib
import socket
import threading
import traceback
from collections import deque
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText

from flask import Flask, jsonify, render_template, request
from zeroconf import ServiceInfo, Zeroconf

app = Flask(__name__)

# ── Canonical field set ────────────────────────────────────────────────────────
# Declared once so validation and docs never drift apart.
REQUIRED_FIELDS = {
    "tdsppm", "turbv", "deltatds", "deltaturb",
    "anomaly", "label", "baselinetds", "baselineturb",
}

# ── Shared state ───────────────────────────────────────────────────────────────
# deque enforces a hard cap without manual pop(); anomaly_count only climbs.
readings: deque = deque(maxlen=100)
lock = threading.Lock()
anomaly_count: int = 0  # global; mutated inside lock only

# ── Per-label action text for email alerts ─────────────────────────────────────
# Kept here rather than inside sendHtmlAlert so updates stay in one place.
ACTION_TEXT = {
    "TURBID": (
        "High turbidity detected. "
        "Do <strong>not</strong> use water for drinking or cooking. "
        "Check and clean the tank filter."
    ),
    "HIGH-TDS": (
        "High dissolved-solid concentration detected. "
        "Water may not be safe. "
        "Schedule tank cleaning and test the supply line."
    ),
    "BOTH": (
        "<strong>Critical:</strong> both turbidity AND TDS are elevated. "
        "<strong>Stop all water use immediately.</strong> "
        "Contact your municipality."
    ),
    "CLEAN": "All parameters normal. No action needed.",  # guard; never normally reached
}

# ── Demo fixtures ──────────────────────────────────────────────────────────────
# Values mirror the three demo bottles used in field testing plus a worst-case
# combined sample; "hightds" (no underscore) matches the URL path segment.
DEMO_FIXTURES = {
    "clean": {
        "tdsppm": 112.0,  "turbv": 0.30,
        "deltatds": 1.5,  "deltaturb": 0.05,
        "anomaly": False, "label": "CLEAN",
        "baselinetds": 110.5, "baselineturb": 0.25,
    },
    "turbid": {
        "tdsppm": 248.0,  "turbv": 6.90,
        "deltatds": 3.2,  "deltaturb": 6.65,
        "anomaly": True,  "label": "TURBID",
        "baselinetds": 110.5, "baselineturb": 0.25,
    },
    "hightds": {
        "tdsppm": 813.0,  "turbv": 0.80,
        "deltatds": 702.5, "deltaturb": 0.55,
        "anomaly": True,  "label": "HIGH-TDS",
        "baselinetds": 110.5, "baselineturb": 0.25,
    },
    "both": {
        "tdsppm": 790.0,  "turbv": 7.20,
        "deltatds": 679.5, "deltaturb": 6.95,
        "anomaly": True,  "label": "BOTH",
        "baselinetds": 110.5, "baselineturb": 0.25,
    },
}


# ── Alert helper ───────────────────────────────────────────────────────────────
def sendHtmlAlert(entry: dict, count: int) -> None:
    """Send an HTML fault-report email via Gmail SMTP_SSL (port 465).

    Called only from daemon threads so a failure here must never propagate
    back to Flask — we print the traceback and return quietly.
    Credentials come exclusively from environment variables so this file can
    live in version control without leaking secrets.
    """
    sender   = os.environ.get("ALERT_SENDER", "")
    password = os.environ.get("ALERT_PASSWORD", "")
    recipient = os.environ.get("ALERT_TO", "")
    dashboard = os.environ.get("DASHBOARD_URL", "#")

    if not all([sender, password, recipient]):
        # Warn once; avoid crashing the daemon thread.
        print("[alert] SMTP credentials missing — set ALERT_SENDER, ALERT_PASSWORD, ALERT_TO")
        return

    label  = entry.get("label", "UNKNOWN")
    action = ACTION_TEXT.get(label, ACTION_TEXT["CLEAN"])

    subject = f"[TANK ALERT #{count}] {label} detected \u2014 action required"

    html_body = f"""\
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <style>
    body  {{ font-family: Arial, sans-serif; background: #f4f4f4; padding: 24px; }}
    .card {{ background: #fff; border-radius: 8px; max-width: 560px;
             margin: auto; padding: 28px; box-shadow: 0 2px 8px rgba(0,0,0,.12); }}
    h2    {{ margin: 0 0 4px; color: #c0392b; }}
    .sub  {{ color: #888; font-size: 13px; margin-bottom: 20px; }}
    table {{ width: 100%; border-collapse: collapse; margin-bottom: 20px; }}
    th    {{ background: #2c3e50; color: #fff; text-align: left;
             padding: 9px 12px; font-size: 13px; }}
    td    {{ padding: 8px 12px; border-bottom: 1px solid #eee; font-size: 14px; }}
    tr:last-child td {{ border-bottom: none; }}
    .action {{ background: #fff8e1; border-left: 4px solid #f39c12;
               padding: 12px 16px; border-radius: 4px;
               font-size: 14px; margin-bottom: 20px; }}
    .btn  {{ display: inline-block; background: #2980b9; color: #fff;
             padding: 10px 20px; border-radius: 5px; text-decoration: none;
             font-size: 14px; }}
    .foot {{ font-size: 11px; color: #aaa; margin-top: 20px; }}
  </style>
</head>
<body>
<div class="card">
  <h2>&#128680; Tank Water Triage Alert</h2>
  <p class="sub">Anomaly #{count} &mdash; Label: <strong>{label}</strong></p>

  <table>
    <tr><th colspan="2">Fault Readings</th></tr>
    <tr><td>&Delta;TDS (ppm)</td>          <td>{entry['deltatds']}</td></tr>
    <tr><td>&Delta;Turbidity (NTU)</td>    <td>{entry['deltaturb']}</td></tr>
    <tr><td>Baseline TDS (ppm)</td>        <td>{entry['baselinetds']}</td></tr>
    <tr><td>Baseline Turbidity (NTU)</td>  <td>{entry['baselineturb']}</td></tr>
  </table>

  <div class="action">
    <strong>&#9888; ACTION REQUIRED</strong><br>{action}
  </div>

  <a class="btn" href="{dashboard}">View Dashboard &rarr;</a>
  <p class="foot">Sent automatically by the Tank Water Triage system. Do not reply.</p>
</div>
</body>
</html>"""

    msg = MIMEMultipart("alternative")
    msg["Subject"] = subject
    msg["From"]    = sender
    msg["To"]      = recipient
    msg.attach(MIMEText(html_body, "html"))

    try:
        with smtplib.SMTP_SSL("smtp.gmail.com", 465) as smtp:
            smtp.login(sender, password)
            smtp.sendmail(sender, recipient, msg.as_string())
        print(f"[alert] Email sent — anomaly #{count} ({label})")
    except Exception:
        # Print but don't re-raise: a failed email must not bring down Flask.
        print(f"[alert] Failed to send email for anomaly #{count}:")
        traceback.print_exc()


# ── Internal helper ────────────────────────────────────────────────────────────
def _ingest(entry: dict) -> None:
    """Append a validated entry to the deque and fire an alert if anomalous.

    Extracted so POST /data and GET /demo/<sample> share identical behaviour.
    anomaly_count is mutated inside lock to prevent race conditions under
    concurrent POSTs from multiple sensors or mock clients.
    """
    global anomaly_count

    fire_alert = False
    with lock:
        readings.append(entry)
        if entry.get("anomaly") is True and entry.get("label") != "CLEAN":
            anomaly_count += 1
            fire_alert = True
        snapshot_count = anomaly_count  # capture before releasing lock

    if fire_alert:
        # Daemon thread so it can't keep the process alive after Flask exits.
        t = threading.Thread(
            target=sendHtmlAlert,
            args=(entry, snapshot_count),
            daemon=True,
            name=f"alert-{snapshot_count}",
        )
        t.start()


# ── Routes ─────────────────────────────────────────────────────────────────────
@app.route("/")
def index():
    return render_template("dashboard.html")


@app.route("/data", methods=["POST"])
def data():
    """Accept a JSON reading from the ESP32 or a mock client.

    Returns 422 listing every missing field so the caller can self-diagnose
    without inspecting server logs.
    """
    payload = request.get_json(silent=True) or {}

    missing = sorted(REQUIRED_FIELDS - payload.keys())
    if missing:
        return jsonify({"error": "missing fields", "missing": missing}), 422

    _ingest(payload)

    with lock:
        count = len(readings)
    return jsonify({"status": "ok", "count": count}), 200


@app.route("/history", methods=["GET"])
def history():
    """Return the full reading history as a JSON array, newest entry last.

    The dashboard polls this to draw its timeline; no filtering or pagination
    is needed at current scale (deque hard-capped at 100 entries).
    """
    with lock:
        snapshot = list(readings)
    return jsonify(snapshot), 200


@app.route("/demo/<sample>", methods=["GET"])
def demo(sample):
    """Inject a labelled fixture reading for offline demos and development.

    Anomalous fixtures (turbid, hightds, both) trigger the same alert path
    as a live POST so the email template can be verified without hardware.
    """
    fixture = DEMO_FIXTURES.get(sample)
    if fixture is None:
        return jsonify({"error": "unknown sample", "sample": sample}), 404

    _ingest(fixture.copy())  # copy guards the master fixture against mutation
    return jsonify({"status": "ok", "sample": sample}), 200


# ── mDNS Discovery ─────────────────────────────────────────────────────────────
def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
    except Exception:
        local_ip = "127.0.0.1"
    finally:
        s.close()
    return local_ip

def start_mdns():
    local_ip = get_local_ip()
    info = ServiceInfo(
        "_http._tcp.local.",
        "tanktriage._http._tcp.local.",
        addresses=[socket.inet_aton(local_ip)],
        port=5000,
        properties={"path": "/data"}
    )
    zc = Zeroconf()
    zc.register_service(info)
    print(f"[mdns] Announced as tanktriage.local -> {local_ip}:5000")

# ── Startup ────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    # use_reloader=False required by RULES.md — reloader would spawn a second
    # process that also tries to bind port 5000, causing an address-in-use error.
    threading.Thread(target=start_mdns, daemon=True).start()
    app.run(host="0.0.0.0", port=5000, debug=False, use_reloader=False)
