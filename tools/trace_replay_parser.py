#!/usr/bin/env python3

import argparse
import collections
import pathlib
import re
import sys

LINE_RE = re.compile(
    r"^\[(?P<ts>[^\]]+)\]\s+\[mono_ms=(?P<mono>\d+)\]\s+\[frame=(?P<frame>\d+)\]\s+\[tid=(?P<tid>\d+)\]\s+(?P<msg>.*)$"
)


def parse_log_file(path: pathlib.Path):
    events = []
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            match = LINE_RE.match(line)
            if not match:
                continue
            events.append(
                {
                    "file": path.name,
                    "ts": match.group("ts"),
                    "mono_ms": int(match.group("mono")),
                    "frame": int(match.group("frame")),
                    "tid": int(match.group("tid")),
                    "msg": match.group("msg"),
                }
            )
    return events


def summarize(events):
    by_file = collections.Counter()
    by_frame = collections.Counter()
    anomalies = []
    for event in events:
        by_file[event["file"]] += 1
        by_frame[event["frame"]] += 1
        msg = event["msg"]
        if "anomaly_detector" in msg or "stall_warning" in msg or "memory_growth_warning" in msg:
            anomalies.append(event)

    print(f"total_events={len(events)}")
    print("events_by_component_log:")
    for name, count in by_file.most_common():
        print(f"  {name}: {count}")

    print("top_frames_by_event_count:")
    for frame, count in by_frame.most_common(10):
        print(f"  frame={frame} events={count}")

    print("anomaly_events:")
    if not anomalies:
        print("  (none)")
    else:
        for event in anomalies[-50:]:
            print(
                f"  ts={event['ts']} frame={event['frame']} file={event['file']} msg={event['msg']}"
            )


def main():
    parser = argparse.ArgumentParser(
        description="Offline replay parser for debug trace logs."
    )
    parser.add_argument(
        "trace_dir",
        help="Trace session directory (for example logs/debug/<session-id>)",
    )
    args = parser.parse_args()

    trace_dir = pathlib.Path(args.trace_dir)
    if not trace_dir.exists() or not trace_dir.is_dir():
        print(f"error: trace_dir not found: {trace_dir}", file=sys.stderr)
        return 2

    log_files = sorted(trace_dir.glob("*.log"))
    if not log_files:
        print(f"error: no .log files found in {trace_dir}", file=sys.stderr)
        return 2

    events = []
    for file_path in log_files:
        events.extend(parse_log_file(file_path))

    events.sort(key=lambda e: (e["mono_ms"], e["frame"], e["file"]))
    summarize(events)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
