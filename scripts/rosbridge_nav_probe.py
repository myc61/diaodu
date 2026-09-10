#!/usr/bin/env python3
"""Local rosbridge navigation probe.

Mirrors the dispatcher ROS1 actionlib path (not send_action_goal):
  advertise/publish  {action}/goal
  subscribe          {action}/feedback
  subscribe          {action}/result

Prints local time when a goal is published and when this machine receives
feedback/result, so you can compare with the vehicle `rostopic echo`.

  python3 -m pip install websocket-client
  python3 scripts/rosbridge_nav_probe.py --url ws://172.16.8.255:9090 \\
      --points 1.95,0.4,-0.0698 1.69,1.48,1.5184 0.012,1.28,0
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import threading
import time
import uuid
from datetime import datetime
from typing import Any

try:
    import websocket  # websocket-client
except ImportError:
    sys.stderr.write(
        "需要 websocket-client：python3 -m pip install websocket-client\n"
    )
    raise


def local_iso() -> str:
    now = datetime.now()
    return now.strftime("%Y-%m-%d %H:%M:%S.") + f"{now.microsecond // 1000:03d}"


def unix_ms() -> int:
    return int(time.time() * 1000)


def yaw_to_quat(yaw: float) -> tuple[float, float]:
    half = yaw / 2.0
    return math.sin(half), math.cos(half)


def parse_points(raw: list[str]) -> list[tuple[float, float, float]]:
    points: list[tuple[float, float, float]] = []
    for item in raw:
        parts = [p.strip() for p in item.split(",")]
        if len(parts) not in (2, 3):
            raise argparse.ArgumentTypeError(
                f"点位格式应为 x,y 或 x,y,yaw，收到: {item}"
            )
        x = float(parts[0])
        y = float(parts[1])
        yaw = float(parts[2]) if len(parts) == 3 else 0.0
        points.append((x, y, yaw))
    return points


def json_int(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return int(value)
    if isinstance(value, dict):
        if "value" in value:
            return json_int(value["value"])
        if "status" in value:
            return json_int(value["status"])
    return None


def goal_id_of(message: dict[str, Any]) -> str:
    status = message.get("status")
    if isinstance(status, dict):
        goal_id = status.get("goal_id")
        if isinstance(goal_id, dict):
            return str(goal_id.get("id") or "")
        if isinstance(goal_id, str):
            return goal_id
    return ""


def nav_state_of(message: dict[str, Any]) -> int | None:
    for block in (message.get("result"), message.get("feedback"), message):
        if isinstance(block, dict) and "state" in block:
            return json_int(block["state"])
    return None


def actionlib_status_of(message: dict[str, Any]) -> int | None:
    status = message.get("status")
    if isinstance(status, dict):
        return json_int(status)
    if isinstance(status, (int, float)):
        return int(status)
    return None


def stamp_text(stamp: Any) -> str:
    if not isinstance(stamp, dict):
        return ""
    secs = json_int(stamp.get("secs", stamp.get("sec")))
    nsecs = json_int(stamp.get("nsecs", stamp.get("nanosec"))) or 0
    if secs is None:
        return ""
    return f"{secs}.{nsecs:09d}"


def make_goal(goal_id: str, x: float, y: float, yaw: float, dist: float, heading: float) -> dict[str, Any]:
    qz, qw = yaw_to_quat(yaw)
    return {
        "header": {"frame_id": "map"},
        "goal_id": {"stamp": {"secs": 0, "nsecs": 0}, "id": goal_id},
        "goal": {
            "header": {"frame_id": "map"},
            "task_type": {"value": 0},
            "waypoints": [
                {
                    "pose": {
                        "position": {"x": x, "y": y, "z": 0.0},
                        "orientation": {"x": 0.0, "y": 0.0, "z": qz, "w": qw},
                    },
                    "distance_tolerance": dist,
                    "heading_tolerance": heading,
                }
            ],
            "translation": {"enable": False, "heading": 0.0},
        },
    }


class NavProbe:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.action = args.action.rstrip("/")
        self.goal_topic = (
            self.action
            if self.action.endswith("/goal")
            else f"{self.action}/goal"
        )
        base = self.goal_topic[: -len("/goal")]
        self.feedback_topic = f"{base}/feedback"
        self.result_topic = f"{base}/result"
        self.ws: websocket.WebSocketApp | None = None
        self.ready = threading.Event()
        self.closed = threading.Event()
        self.lock = threading.Lock()
        self.current_id = ""
        self.published_at_ms = 0
        self.last_fb_nav: int | None = None
        self.last_fb_lib: int | None = None
        self.done = threading.Event()
        self.outcome: dict[str, Any] | None = None
        self._id = 0

    def next_id(self, prefix: str) -> str:
        self._id += 1
        return f"{prefix}-{self._id}"

    def send(self, payload: dict[str, Any]) -> None:
        assert self.ws is not None
        self.ws.send(json.dumps(payload, separators=(",", ":")))

    def log(self, message: str, **detail: Any) -> None:
        line = f"{local_iso()} {message}"
        if detail:
            line += " " + json.dumps(detail, ensure_ascii=False)
        print(line, flush=True)

    def on_open(self, _ws: websocket.WebSocketApp) -> None:
        self.log(
            "rosbridge connected",
            url=self.args.url,
            goal=self.goal_topic,
            feedback=self.feedback_topic,
            result=self.result_topic,
        )
        self.send(
            {
                "op": "advertise",
                "topic": self.goal_topic,
                "type": "navigation/NavigationActionGoal",
            }
        )
        for topic, msg_type, sid in (
            (
                self.feedback_topic,
                "navigation/NavigationActionFeedback",
                "sub-feedback",
            ),
            (
                self.result_topic,
                "navigation/NavigationActionResult",
                "sub-result",
            ),
        ):
            self.send(
                {
                    "op": "subscribe",
                    "id": sid,
                    "topic": topic,
                    "type": msg_type,
                    "throttle_rate": 0,
                    "queue_length": 10,
                    "compression": "none",
                }
            )
        self.ready.set()

    def on_close(self, _ws: websocket.WebSocketApp, code: Any, reason: Any) -> None:
        self.log("rosbridge closed", code=code, reason=str(reason))
        self.closed.set()
        self.done.set()

    def on_error(self, _ws: websocket.WebSocketApp, error: Any) -> None:
        self.log("rosbridge error", error=str(error))

    def on_message(self, _ws: websocket.WebSocketApp, raw: str) -> None:
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            self.log("non-json message", raw=raw[:200])
            return
        op = data.get("op")
        if op == "status":
            if data.get("level") not in (None, "info", "none"):
                self.log("rosbridge status", **{k: data[k] for k in data if k != "op"})
            return
        if op != "publish":
            return
        topic = data.get("topic")
        msg = data.get("msg")
        if not isinstance(msg, dict):
            return
        incoming = goal_id_of(msg)
        with self.lock:
            expected = self.current_id
        if incoming and expected and incoming != expected:
            return
        if topic == self.feedback_topic:
            self._on_feedback(msg, incoming)
        elif topic == self.result_topic:
            self._on_result(msg, incoming)

    def _on_feedback(self, msg: dict[str, Any], incoming: str) -> None:
        nav = nav_state_of(msg)
        lib = actionlib_status_of(msg)
        stamp = stamp_text((msg.get("header") or {}).get("stamp"))
        inner = stamp_text(((msg.get("feedback") or {}).get("header") or {}).get("stamp"))
        text = ""
        status = msg.get("status")
        if isinstance(status, dict):
            text = str(status.get("text") or "")
        changed = nav != self.last_fb_nav or lib != self.last_fb_lib
        if not changed:
            return
        self.last_fb_nav = nav
        self.last_fb_lib = lib
        elapsed = unix_ms() - self.published_at_ms if self.published_at_ms else None
        self.log(
            "feedback state changed",
            goal_id=incoming or self.current_id,
            nav_state=nav,
            actionlib_status=lib,
            status_text=text,
            robot_header_stamp=stamp,
            robot_feedback_stamp=inner,
            ms_since_dispatched=elapsed,
        )
        if nav in (5, 7, 8, 9) or lib in (2, 4, 5, 8, 9):
            self._finish(False, msg, incoming, source="feedback")

    def _on_result(self, msg: dict[str, Any], incoming: str) -> None:
        nav = nav_state_of(msg)
        lib = actionlib_status_of(msg)
        header_stamp = stamp_text((msg.get("header") or {}).get("stamp"))
        inner_stamp = stamp_text(
            ((msg.get("result") or {}).get("header") or {}).get("stamp")
        )
        elapsed = unix_ms() - self.published_at_ms if self.published_at_ms else None
        self.log(
            "result received",
            goal_id=incoming or self.current_id,
            nav_state=nav,
            actionlib_status=lib,
            robot_result_header_stamp=header_stamp,
            robot_result_inner_stamp=inner_stamp,
            ms_since_dispatched=elapsed,
            distance_deviation=(msg.get("result") or {}).get("distance_deviation"),
            heading_deviation=(msg.get("result") or {}).get("heading_deviation"),
            duration=(msg.get("result") or {}).get("duration"),
        )
        if nav in (5, 7, 8, 9) or lib in (2, 4, 5, 8, 9):
            self._finish(False, msg, incoming, source="result")
            return
        if nav == 6:
            self._finish(True, msg, incoming, source="result")
            return
        self.log(
            "result is not terminal yet, keep waiting",
            nav_state=nav,
            actionlib_status=lib,
        )

    def _finish(
        self, success: bool, msg: dict[str, Any], incoming: str, source: str
    ) -> None:
        if self.done.is_set():
            return
        elapsed = unix_ms() - self.published_at_ms if self.published_at_ms else None
        decided = local_iso()
        self.outcome = {
            "success": success,
            "source": source,
            "goal_id": incoming or self.current_id,
            "decided_at": decided,
            "elapsed_ms": elapsed,
            "nav_state": nav_state_of(msg),
            "actionlib_status": actionlib_status_of(msg),
        }
        label = "SUCCEEDED" if success else "FAILED"
        self.log(
            f"task {label} at {decided}",
            dispatched_unix_ms=self.published_at_ms,
            elapsed_ms=elapsed,
            source=source,
            goal_id=incoming or self.current_id,
        )
        self.done.set()

    def publish_goal(self, x: float, y: float, yaw: float) -> str:
        goal_id = str(uuid.uuid4())
        payload = make_goal(
            goal_id, x, y, yaw, self.args.distance, self.args.heading
        )
        with self.lock:
            self.current_id = goal_id
            self.published_at_ms = unix_ms()
            self.last_fb_nav = None
            self.last_fb_lib = None
            self.outcome = None
            self.done.clear()
        dispatched = local_iso()
        self.send(
            {
                "op": "publish",
                "topic": self.goal_topic,
                "type": "navigation/NavigationActionGoal",
                "msg": payload,
            }
        )
        self.log(
            f"goal dispatched at {dispatched}",
            goal_id=goal_id,
            x=x,
            y=y,
            yaw=yaw,
            dispatched_unix_ms=self.published_at_ms,
        )
        return goal_id

    def run(self) -> int:
        self.ws = websocket.WebSocketApp(
            self.args.url,
            on_open=self.on_open,
            on_message=self.on_message,
            on_error=self.on_error,
            on_close=self.on_close,
        )
        thread = threading.Thread(target=self.ws.run_forever, daemon=True)
        thread.start()
        if not self.ready.wait(self.args.connect_timeout):
            self.log("connect timeout")
            return 2
        # Give rosbridge a moment to apply advertise/subscribe.
        time.sleep(0.3)

        code = 0
        for index, (x, y, yaw) in enumerate(self.args.points, start=1):
            self.log(f"--- waypoint {index}/{len(self.args.points)} ---")
            sent_ms = unix_ms()
            self.publish_goal(x, y, yaw)
            if not self.done.wait(self.args.timeout):
                self.log(
                    "wait timeout",
                    timeout_s=self.args.timeout,
                    goal_id=self.current_id,
                )
                code = 1
                break
            if self.closed.is_set():
                code = 2
                break
            outcome = self.outcome or {}
            if not outcome.get("success"):
                code = 1
                break
            gap = unix_ms() - sent_ms
            if index < len(self.args.points):
                self.log(
                    "handoff to next goal",
                    ms_since_this_goal_sent=gap,
                    ms_since_decided=outcome.get("elapsed_ms"),
                )
        if self.ws:
            self.ws.close()
        return code


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="用 rosbridge 话题接口探测导航 goal/feedback/result 时延"
    )
    parser.add_argument(
        "--url",
        default="ws://172.16.8.255:9090",
        help="rosbridge WebSocket，默认分拣搬运",
    )
    parser.add_argument(
        "--action",
        default="/zj_humanoid/navigation/navigation",
        help="action server 名，不要带 /goal",
    )
    parser.add_argument(
        "--points",
        nargs="+",
        default=["1.95,0.4,-0.0698", "1.69,1.48,1.5184", "0.012,1.28,0"],
        help="一个或多个 x,y[,yaw]",
    )
    parser.add_argument("--distance", type=float, default=0.04)
    parser.add_argument("--heading", type=float, default=0.04)
    parser.add_argument("--timeout", type=float, default=180.0, help="单点等待秒数")
    parser.add_argument("--connect-timeout", type=float, default=8.0)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    args.points = parse_points(args.points)
    return NavProbe(args).run()


if __name__ == "__main__":
    raise SystemExit(main())
