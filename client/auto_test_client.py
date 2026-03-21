#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from dataclasses import asdict, dataclass, field
from typing import Dict, List, Optional, Tuple

DEFAULT_TIMEOUT_MS = 1000
NO_SYNC_TIMEOUT_MS = 250
CONNECT_GAP_MS = 120
GAME_OVER_TIMEOUT_MS = 600
GAME_OVER_PACKET_TYPE = 0xFD
TEST_COMMAND_PACKET_TYPE = 0xFC
TEST_COMMAND_LOAD_PRESET = 0x01

INITIAL_STONES: Dict[int, Tuple[str, str, int, int]] = {
    0: ("black", "CHE", 0, 0),
    1: ("black", "MA", 0, 1),
    2: ("black", "XIANG", 0, 2),
    3: ("black", "SHI", 0, 3),
    4: ("black", "JIANG", 0, 4),
    5: ("black", "SHI", 0, 5),
    6: ("black", "XIANG", 0, 6),
    7: ("black", "MA", 0, 7),
    8: ("black", "CHE", 0, 8),
    9: ("black", "PAO", 2, 1),
    10: ("black", "PAO", 2, 7),
    11: ("black", "BING", 3, 0),
    12: ("black", "BING", 3, 2),
    13: ("black", "BING", 3, 4),
    14: ("black", "BING", 3, 6),
    15: ("black", "BING", 3, 8),
    16: ("red", "CHE", 9, 8),
    17: ("red", "MA", 9, 7),
    18: ("red", "XIANG", 9, 6),
    19: ("red", "SHI", 9, 5),
    20: ("red", "JIANG", 9, 4),
    21: ("red", "SHI", 9, 3),
    22: ("red", "XIANG", 9, 2),
    23: ("red", "MA", 9, 1),
    24: ("red", "CHE", 9, 0),
    25: ("red", "PAO", 7, 7),
    26: ("red", "PAO", 7, 1),
    27: ("red", "BING", 6, 8),
    28: ("red", "BING", 6, 6),
    29: ("red", "BING", 6, 4),
    30: ("red", "BING", 6, 2),
    31: ("red", "BING", 6, 0),
}

TEST_PRESETS: Dict[str, Dict[str, object]] = {
    "perpetual_check_loop": {
        "id": 1,
        "live_positions": {
            4: (0, 3),
            20: (9, 5),
            24: (2, 4),
        },
    },
    "stalemate_trap": {
        "id": 2,
        "live_positions": {
            4: (0, 4),
            3: (1, 4),
            16: (1, 3),
            24: (2, 5),
            20: (9, 4),
        },
    },
}

GAME_OVER_LABELS = {
    1: "red_win",
    2: "black_win",
    3: "draw",
}


@dataclass
class Step:
    name: str
    actor: str
    kind: str = "move"
    payload: Optional[Tuple[int, int, int]] = None
    preset_name: Optional[str] = None
    raw: Optional[bytes] = None
    chunks: List[bytes] = field(default_factory=list)
    delays_ms: List[int] = field(default_factory=list)
    expected_ack: Optional[int] = None
    expected_sync: Optional[Tuple[int, int, int]] = None
    expected_game_over: Optional[str] = None
    expect_no_sync: bool = False
    expected_violations: Optional[Dict[str, int]] = None
    exploratory: bool = False
    note: str = ""
    ack_timeout_ms: Optional[int] = None
    sync_timeout_ms: Optional[int] = None
    game_over_timeout_ms: int = GAME_OVER_TIMEOUT_MS
    no_sync_timeout_ms: int = NO_SYNC_TIMEOUT_MS


@dataclass
class Suite:
    name: str
    description: str
    steps: List[Step]
    exploratory: bool = False
    manual_notes: List[str] = field(default_factory=list)


class BoardState:
    def __init__(self) -> None:
        self.positions: Dict[int, Tuple[int, int]] = {}
        self.dead: Dict[int, bool] = {}
        self.load_initial()

    def load_initial(self) -> None:
        live_positions = {stone_id: (row, col) for stone_id, (_color, _piece, row, col) in INITIAL_STONES.items()}
        self.load_layout(live_positions)

    def load_layout(self, live_positions: Dict[int, Tuple[int, int]]) -> None:
        self.positions.clear()
        self.dead.clear()
        for stone_id, (_color, _piece, row, col) in INITIAL_STONES.items():
            self.positions[stone_id] = live_positions.get(stone_id, (row, col))
            self.dead[stone_id] = stone_id not in live_positions

    def piece_at(self, row: int, col: int) -> Optional[int]:
        for stone_id, pos in self.positions.items():
            if not self.dead[stone_id] and pos == (row, col):
                return stone_id
        return None

    def apply_move(self, stone_id: int, row: int, col: int) -> Optional[int]:
        killed = self.piece_at(row, col)
        if killed is not None and killed != stone_id:
            self.dead[killed] = True
        self.positions[stone_id] = (row, col)
        return killed


class ProtocolClient:
    def __init__(self, name: str, host: str, port: int, timeout_ms: int) -> None:
        self.name = name
        self.host = host
        self.port = port
        self.timeout_ms = timeout_ms
        self.sock: Optional[socket.socket] = None

    def connect(self) -> None:
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout_ms / 1000.0)
        self.sock.settimeout(self.timeout_ms / 1000.0)

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def send_move(self, payload: Tuple[int, int, int]) -> None:
        data = bytes(self._to_byte(v) for v in payload)
        self.send_raw(data)

    def send_raw(self, data: bytes) -> None:
        if self.sock is None:
            raise RuntimeError(f"{self.name} not connected")
        self.sock.sendall(data)

    def send_chunks(self, chunks: List[bytes], delays_ms: List[int]) -> None:
        if self.sock is None:
            raise RuntimeError(f"{self.name} not connected")
        for index, chunk in enumerate(chunks):
            self.sock.sendall(chunk)
            if index < len(delays_ms):
                time.sleep(delays_ms[index] / 1000.0)

    def recv_exact(self, size: int, timeout_ms: Optional[int] = None) -> bytes:
        if self.sock is None:
            raise RuntimeError(f"{self.name} not connected")
        deadline = time.monotonic() + ((timeout_ms or self.timeout_ms) / 1000.0)
        data = bytearray()
        while len(data) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"{self.name} recv_exact timeout waiting for {size} bytes")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(size - len(data))
            except socket.timeout as exc:
                raise TimeoutError(f"{self.name} recv_exact timeout waiting for {size} bytes") from exc
            if not chunk:
                raise ConnectionError(f"{self.name} disconnected while waiting for {size} bytes")
            data.extend(chunk)
        self.sock.settimeout(self.timeout_ms / 1000.0)
        return bytes(data)

    def recv_optional_exact(self, size: int, timeout_ms: int) -> Optional[bytes]:
        try:
            return self.recv_exact(size, timeout_ms=timeout_ms)
        except TimeoutError:
            return None

    def recv_optional_game_over(self, timeout_ms: int) -> Optional[str]:
        raw = self.recv_optional_exact(2, timeout_ms=timeout_ms)
        if raw is None:
            return None
        if len(raw) != 2 or raw[0] != GAME_OVER_PACKET_TYPE:
            raise ValueError(f"{self.name} unexpected game-over payload: {list(raw)}")
        return GAME_OVER_LABELS.get(raw[1], f"unknown:{raw[1]}")

    def drain_available(self, timeout_ms: int) -> bytes:
        if self.sock is None:
            raise RuntimeError(f"{self.name} not connected")
        deadline = time.monotonic() + (timeout_ms / 1000.0)
        collected = bytearray()
        self.sock.settimeout(max(0.05, timeout_ms / 1000.0))
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(64)
            except socket.timeout:
                break
            if not chunk:
                break
            collected.extend(chunk)
        self.sock.settimeout(self.timeout_ms / 1000.0)
        return bytes(collected)

    @staticmethod
    def _to_byte(value: int) -> int:
        if not 0 <= value <= 255:
            raise ValueError(f"byte value out of range: {value}")
        return value


class SuiteRunner:
    def __init__(self, host: str, port: int, timeout_ms: int, strict_exploratory: bool) -> None:
        self.host = host
        self.port = port
        self.timeout_ms = timeout_ms
        self.strict_exploratory = strict_exploratory

    def run_suite(self, suite: Suite) -> Tuple[bool, List[dict]]:
        print(f"=== Running suite: {suite.name} ===")
        print(suite.description)
        for note in suite.manual_notes:
            print(f"[NOTE] {note}")

        red = ProtocolClient("red", self.host, self.port, self.timeout_ms)
        black = ProtocolClient("black", self.host, self.port, self.timeout_ms)
        board = BoardState()
        violations = {"red": 0, "black": 0}
        results: List[dict] = []
        ok = True

        try:
            red.connect()
            time.sleep(CONNECT_GAP_MS / 1000.0)
            black.connect()

            for step in suite.steps:
                result = self._run_step(suite, step, red, black, board, violations)
                results.append(result)
                self._print_step_result(step, result)
                if result["status"] == "FAIL":
                    ok = False
                    break
        finally:
            red.close()
            black.close()

        summary = f"Suite {suite.name}: {'PASS' if ok else 'FAIL'}"
        print(summary)
        return ok, results

    def _run_step(
        self,
        suite: Suite,
        step: Step,
        red: ProtocolClient,
        black: ProtocolClient,
        board: BoardState,
        violations: Dict[str, int],
    ) -> dict:
        actor = red if step.actor == "red" else black
        peer = black if step.actor == "red" else red
        payload_repr = None
        ack_value: Optional[int] = None
        sync_value: Optional[Tuple[int, int, int]] = None
        game_over: Optional[str] = None
        note = step.note
        status = "PASS"

        try:
            if step.note:
                print(f"[STEP NOTE] {step.note}")

            if step.kind == "move":
                if step.payload is None:
                    raise ValueError(f"step {step.name} missing payload")
                payload_repr = list(step.payload)
                actor.send_move(step.payload)
                status, ack_value, sync_value, game_over, note = self._handle_move_step(
                    step, actor, peer, board, violations, note
                )
            elif step.kind == "preset":
                if not step.preset_name:
                    raise ValueError(f"step {step.name} missing preset name")
                payload_repr = [TEST_COMMAND_PACKET_TYPE, TEST_COMMAND_LOAD_PRESET, TEST_PRESETS[step.preset_name]["id"]]
                status, ack_value, note = self._handle_preset_step(step, actor, board, violations, note)
            elif step.kind == "raw":
                raw = step.raw or b""
                payload_repr = list(raw)
                actor.send_raw(raw)
                status, ack_value, sync_value, game_over, note = self._handle_exploratory_step(step, actor, peer, note)
            elif step.kind == "partial":
                payload_repr = [list(chunk) for chunk in step.chunks]
                actor.send_chunks(step.chunks, step.delays_ms)
                status, ack_value, sync_value, game_over, note = self._handle_exploratory_step(step, actor, peer, note)
            else:
                raise ValueError(f"unknown step kind: {step.kind}")
        except Exception as exc:  # noqa: BLE001
            note = f"{note} | exception={exc}".strip(" |")
            if step.exploratory or suite.exploratory:
                status = "FAIL" if self.strict_exploratory else "OBSERVE"
            else:
                status = "FAIL"

        if step.expected_violations is not None and status == "PASS":
            if violations != step.expected_violations:
                status = "FAIL"
                note = self._merge_note(note, f"expected_violations={step.expected_violations}, actual={violations}")

        return {
            "suite": suite.name,
            "step": step.name,
            "actor": step.actor,
            "kind": step.kind,
            "payload": payload_repr,
            "ack": ack_value,
            "sync": list(sync_value) if sync_value is not None else None,
            "game_over": game_over,
            "violations": dict(violations),
            "status": status,
            "note": note,
        }

    def _handle_move_step(
        self,
        step: Step,
        actor: ProtocolClient,
        peer: ProtocolClient,
        board: BoardState,
        violations: Dict[str, int],
        note: str,
    ) -> Tuple[str, Optional[int], Optional[Tuple[int, int, int]], Optional[str], str]:
        ack_timeout = step.ack_timeout_ms or self.timeout_ms
        ack_raw = actor.recv_exact(1, timeout_ms=ack_timeout)
        ack_value = int.from_bytes(ack_raw, byteorder="little", signed=True)
        if step.expected_ack is not None and ack_value != step.expected_ack:
            return "FAIL", ack_value, None, None, self._merge_note(note, f"expected_ack={step.expected_ack}, actual={ack_value}")

        if ack_value == 1:
            sync_raw = peer.recv_exact(3, timeout_ms=step.sync_timeout_ms or self.timeout_ms)
            sync_value = tuple(sync_raw[i] for i in range(3))
            expected_sync = step.expected_sync or step.payload
            if expected_sync is not None and sync_value != expected_sync:
                return "FAIL", ack_value, sync_value, None, self._merge_note(note, f"expected_sync={expected_sync}, actual={sync_value}")
            board.apply_move(*sync_value)
            game_over, note = self._check_game_over(step, actor, peer, note)
            if step.expected_game_over and game_over != step.expected_game_over:
                return "FAIL", ack_value, sync_value, game_over, self._merge_note(
                    note, f"expected_game_over={step.expected_game_over}, actual={game_over}"
                )
            return "PASS", ack_value, sync_value, game_over, note

        if ack_value == -1:
            violations[step.actor] += 1
            if step.expect_no_sync:
                unexpected_sync = peer.recv_optional_exact(3, timeout_ms=step.no_sync_timeout_ms)
                if unexpected_sync is not None:
                    sync_value = tuple(unexpected_sync[i] for i in range(3))
                    return "FAIL", ack_value, sync_value, None, self._merge_note(note, f"unexpected_sync={sync_value}")
            game_over, note = self._check_game_over(step, actor, peer, note)
            if step.expected_game_over and game_over != step.expected_game_over:
                return "FAIL", ack_value, None, game_over, self._merge_note(
                    note, f"expected_game_over={step.expected_game_over}, actual={game_over}"
                )
            return "PASS", ack_value, None, game_over, note

        return "FAIL", ack_value, None, None, self._merge_note(note, "unexpected_ack_value")

    def _handle_preset_step(
        self,
        step: Step,
        actor: ProtocolClient,
        board: BoardState,
        violations: Dict[str, int],
        note: str,
    ) -> Tuple[str, Optional[int], str]:
        preset = TEST_PRESETS[step.preset_name]
        payload = bytes([TEST_COMMAND_PACKET_TYPE, TEST_COMMAND_LOAD_PRESET, int(preset["id"])])
        actor.send_raw(payload)
        ack_raw = actor.recv_exact(1, timeout_ms=step.ack_timeout_ms or self.timeout_ms)
        ack_value = int.from_bytes(ack_raw, byteorder="little", signed=True)
        if ack_value != 1:
            return "FAIL", ack_value, self._merge_note(note, f"preset_ack_failed={ack_value}")
        board.load_layout(dict(preset["live_positions"]))
        violations["red"] = 0
        violations["black"] = 0
        return "PASS", ack_value, self._merge_note(note, f"preset={step.preset_name}")

    def _handle_exploratory_step(
        self,
        step: Step,
        actor: ProtocolClient,
        peer: ProtocolClient,
        note: str,
    ) -> Tuple[str, Optional[int], Optional[Tuple[int, int, int]], Optional[str], str]:
        actor_bytes = actor.drain_available(step.ack_timeout_ms or 600)
        peer_bytes = peer.drain_available(step.sync_timeout_ms or 600)
        ack_value = None
        sync_value = None
        game_over = None
        if len(actor_bytes) >= 1:
            ack_value = int.from_bytes(actor_bytes[:1], byteorder="little", signed=True)
        if len(peer_bytes) >= 3:
            sync_value = tuple(peer_bytes[:3][i] for i in range(3))
        if len(actor_bytes) >= 3 and actor_bytes[1] == GAME_OVER_PACKET_TYPE:
            game_over = GAME_OVER_LABELS.get(actor_bytes[2], f"unknown:{actor_bytes[2]}")
        elif len(peer_bytes) >= 5 and peer_bytes[3] == GAME_OVER_PACKET_TYPE:
            game_over = GAME_OVER_LABELS.get(peer_bytes[4], f"unknown:{peer_bytes[4]}")
        note = self._merge_note(
            note,
            f"actor_bytes={list(actor_bytes) if actor_bytes else []}, peer_bytes={list(peer_bytes) if peer_bytes else []}",
        )
        return ("OBSERVE" if not self.strict_exploratory else "PASS"), ack_value, sync_value, game_over, note

    def _check_game_over(
        self,
        step: Step,
        actor: ProtocolClient,
        peer: ProtocolClient,
        note: str,
    ) -> Tuple[Optional[str], str]:
        timeout_ms = step.game_over_timeout_ms if step.expected_game_over else min(120, step.game_over_timeout_ms)
        actor_result = actor.recv_optional_game_over(timeout_ms)
        peer_result = peer.recv_optional_game_over(timeout_ms)

        game_over = actor_result or peer_result
        if actor_result and peer_result and actor_result != peer_result:
            note = self._merge_note(note, f"actor_game_over={actor_result}, peer_game_over={peer_result}")
            return None, note
        if step.expected_game_over and game_over is None:
            note = self._merge_note(note, "game_over_packet_missing")
        return game_over, note

    @staticmethod
    def _merge_note(base: str, extra: str) -> str:
        if not base:
            return extra
        if not extra:
            return base
        return f"{base} | {extra}"

    @staticmethod
    def _print_step_result(step: Step, result: dict) -> None:
        violations = result["violations"]
        print(
            f"[{result['status']}] {step.name} actor={step.actor} payload={result['payload']} "
            f"ack={result['ack']} sync={result['sync']} game_over={result['game_over']} "
            f"violations=red:{violations['red']}/3 black:{violations['black']}/3"
        )
        if result["note"]:
            print(f"        note: {result['note']}")


def move_step(
    name: str,
    actor: str,
    payload: Tuple[int, int, int],
    expected_ack: int,
    expected_violations: Optional[Dict[str, int]] = None,
    expected_game_over: Optional[str] = None,
    note: str = "",
    ack_timeout_ms: Optional[int] = None,
) -> Step:
    return Step(
        name=name,
        actor=actor,
        kind="move",
        payload=payload,
        expected_ack=expected_ack,
        expected_sync=payload if expected_ack == 1 else None,
        expected_game_over=expected_game_over,
        expect_no_sync=expected_ack == -1,
        expected_violations=expected_violations,
        note=note,
        ack_timeout_ms=ack_timeout_ms,
    )


def preset_step(name: str, actor: str, preset_name: str, note: str = "") -> Step:
    return Step(
        name=name,
        actor=actor,
        kind="preset",
        preset_name=preset_name,
        note=note,
    )


def build_suites() -> Dict[str, Suite]:
    suites = {
        "smoke": Suite(
            name="smoke",
            description="验证双连接、红黑顺序、合法 ACK、合法同步和基本回合切换。",
            steps=[
                move_step("red pawn forward", "red", (29, 5, 4), 1),
                move_step("black pawn forward", "black", (13, 4, 4), 1),
            ],
        ),
        "horse_leg": Suite(
            name="horse_leg",
            description="验证别马腿：马腿被堵时失败，放开后成功。",
            steps=[
                move_step("red horse blocked by elephant leg", "red", (23, 8, 3), -1, {"red": 1, "black": 0}),
                move_step("red side pawn forward", "red", (31, 5, 0), 1, {"red": 1, "black": 0}),
                move_step("black pawn forward", "black", (13, 4, 4), 1, {"red": 1, "black": 0}),
                move_step("red elephant clears horse leg", "red", (22, 7, 4), 1, {"red": 1, "black": 0}),
                move_step("black pawn 12 forward", "black", (12, 4, 2), 1, {"red": 1, "black": 0}),
                move_step("red horse succeeds after leg cleared", "red", (23, 8, 3), 1, {"red": 1, "black": 0}),
            ],
        ),
        "elephant_eye": Suite(
            name="elephant_eye",
            description="验证卡象眼：象眼被堵时失败，放开后成功。",
            steps=[
                move_step("red elephant diag out", "red", (22, 7, 4), 1),
                move_step("black pawn forward", "black", (13, 4, 4), 1),
                move_step("red horse blocks elephant eye", "red", (23, 8, 3), 1),
                move_step("black pawn 12 forward", "black", (12, 4, 2), 1),
                move_step("red elephant blocked by eye", "red", (22, 9, 2), -1, {"red": 1, "black": 0}),
                move_step("red side pawn forward", "red", (31, 5, 0), 1, {"red": 1, "black": 0}),
                move_step("black pawn 11 forward", "black", (11, 4, 0), 1, {"red": 1, "black": 0}),
                move_step("red horse leaves elephant eye", "red", (23, 7, 5), 1, {"red": 1, "black": 0}),
                move_step("black pawn 14 forward", "black", (14, 4, 6), 1, {"red": 1, "black": 0}),
                move_step("red elephant succeeds after eye opens", "red", (22, 9, 2), 1, {"red": 1, "black": 0}),
            ],
        ),
        "guard_rules": Suite(
            name="guard_rules",
            description="验证回合错误、操作对方棋子、非法 id、同色吃子。",
            steps=[
                move_step("black moves first", "black", (13, 4, 4), -1, {"red": 0, "black": 1}),
                move_step("red operates black piece", "red", (13, 4, 4), -1, {"red": 1, "black": 1}),
                move_step("black invalid id 255", "black", (255, 0, 0), -1, {"red": 1, "black": 2}),
                move_step("red self capture", "red", (24, 9, 1), -1, {"red": 2, "black": 2}),
            ],
        ),
        "movement_rules": Suite(
            name="movement_rules",
            description="验证车被挡、兵未过河横走、炮隔山规则。",
            steps=[
                move_step("red pawn forward", "red", (29, 5, 4), 1),
                move_step("black pawn forward", "black", (13, 4, 4), 1),
                move_step("red rook blocked", "red", (24, 5, 0), -1, {"red": 1, "black": 0}),
                move_step("red pawn horizontal before river", "red", (30, 6, 3), -1, {"red": 2, "black": 0}),
                move_step("red side pawn forward", "red", (31, 5, 0), 1, {"red": 2, "black": 0}),
                move_step("black rook blocked", "black", (0, 4, 0), -1, {"red": 2, "black": 1}),
                move_step("black cannon invalid capture", "black", (10, 7, 7), -1, {"red": 2, "black": 2}),
            ],
        ),
        "palace_elephant": Suite(
            name="palace_elephant",
            description="验证士出九宫、象过河。",
            steps=[
                move_step("red advisor diag 1", "red", (21, 8, 4), 1),
                move_step("black pawn forward 12", "black", (13, 4, 4), 1),
                move_step("red advisor diag 2", "red", (21, 7, 5), 1),
                move_step("black pawn 12 forward", "black", (12, 4, 2), 1),
                move_step("red advisor out of palace", "red", (21, 6, 6), -1, {"red": 1, "black": 0}),
                move_step("red elephant diag 1", "red", (22, 7, 4), 1, {"red": 1, "black": 0}),
                move_step("black pawn 11 forward", "black", (11, 4, 0), 1, {"red": 1, "black": 0}),
                move_step("red elephant diag 2", "red", (22, 5, 2), 1, {"red": 1, "black": 0}),
                move_step("black pawn 14 forward", "black", (14, 4, 6), 1, {"red": 1, "black": 0}),
                move_step("red elephant crosses river", "red", (22, 3, 0), -1, {"red": 2, "black": 0}),
            ],
        ),
        "capture_state": Suite(
            name="capture_state",
            description="验证合法吃子后，死子不可再动。",
            steps=[
                move_step("red cannon captures black horse", "red", (26, 0, 1), 1),
                move_step("black pawn forward", "black", (13, 4, 4), 1),
                move_step("red general forward", "red", (20, 8, 4), 1),
                move_step("black dead horse moves", "black", (1, 2, 2), -1, {"red": 0, "black": 1}),
                move_step("black general out of palace", "black", (4, 3, 4), -1, {"red": 0, "black": 2}),
            ],
        ),
        "self_check": Suite(
            name="self_check",
            description="验证送将：走子后让己方将帅受攻击应被拒绝。",
            steps=[
                move_step("red pawn forward", "red", (29, 5, 4), 1),
                move_step("black pawn forward", "black", (13, 4, 4), 1),
                move_step("red pawn captures center pawn", "red", (29, 4, 4), 1),
                move_step("black pawn 12 forward", "black", (12, 4, 2), 1),
                move_step("red pawn exposes flying generals", "red", (29, 4, 5), -1, {"red": 1, "black": 0}),
            ],
        ),
        "three_strikes": Suite(
            name="three_strikes",
            description="验证同一方累计 3 次违规后立即判负，并广播终局结果。",
            steps=[
                move_step("black first invalid", "black", (13, 4, 4), -1, {"red": 0, "black": 1}),
                move_step("black invalid id 255", "black", (255, 0, 0), -1, {"red": 0, "black": 2}),
                move_step(
                    "black third strike",
                    "black",
                    (0, 4, 0),
                    -1,
                    {"red": 0, "black": 3},
                    expected_game_over="red_win",
                ),
            ],
        ),
        "repetition_draw": Suite(
            name="repetition_draw",
            description="验证普通重复局面判和。",
            steps=[
                move_step("red horse out", "red", (23, 7, 2), 1),
                move_step("black horse out", "black", (7, 2, 6), 1),
                move_step("red horse back", "red", (23, 9, 1), 1),
                move_step("black horse back", "black", (7, 0, 7), 1),
                move_step("red horse out repeat", "red", (23, 7, 2), 1),
                move_step("black horse out repeat", "black", (7, 2, 6), 1),
                move_step("red horse back repeat", "red", (23, 9, 1), 1),
                move_step("black horse back repeat", "black", (7, 0, 7), 1, expected_game_over="draw"),
            ],
        ),
        "perpetual_chase": Suite(
            name="perpetual_chase",
            description="验证长捉：持续追捉的一方判负。",
            steps=[
                move_step("red cannon chase starts", "red", (26, 8, 1), 1),
                move_step("black horse out", "black", (7, 2, 6), 1),
                move_step("red cannon chase returns", "red", (26, 7, 1), 1),
                move_step("black horse back", "black", (7, 0, 7), 1),
                move_step("red cannon chase starts repeat", "red", (26, 8, 1), 1),
                move_step("black horse out repeat", "black", (7, 2, 6), 1),
                move_step("red cannon chase returns repeat", "red", (26, 7, 1), 1),
                move_step("black horse back repeat", "black", (7, 0, 7), 1, expected_game_over="black_win"),
            ],
        ),
        "perpetual_check": Suite(
            name="perpetual_check",
            description="验证长将：持续将军的一方判负。",
            steps=[
                preset_step("load perpetual check preset", "red", "perpetual_check_loop"),
                move_step("red rook checks on file 3", "red", (24, 2, 3), 1),
                move_step("black general sidesteps", "black", (4, 0, 4), 1),
                move_step("red rook checks on file 4", "red", (24, 2, 4), 1),
                move_step("black general returns", "black", (4, 0, 3), 1),
                move_step("red rook checks on file 3 repeat", "red", (24, 2, 3), 1),
                move_step("black general sidesteps repeat", "black", (4, 0, 4), 1),
                move_step("red rook checks on file 4 repeat", "red", (24, 2, 4), 1),
                move_step("black general returns repeat", "black", (4, 0, 3), 1, expected_game_over="black_win"),
            ],
        ),
        "stalemate_or_no_legal_move": Suite(
            name="stalemate_or_no_legal_move",
            description="验证困毙：一方无合法着法时立即判负。",
            steps=[
                preset_step("load stalemate preset", "red", "stalemate_trap"),
                move_step("red rook closes final escape", "red", (24, 1, 5), 1, expected_game_over="red_win"),
            ],
        ),
        "protocol_concat": Suite(
            name="protocol_concat",
            description="探索 TCP 粘包：一次发送两步原始字节，记录 ACK、同步和连接状态。",
            exploratory=True,
            steps=[
                Step(
                    name="black sends two moves in one packet",
                    actor="black",
                    kind="raw",
                    raw=bytes([13, 4, 4, 12, 4, 2]),
                    exploratory=True,
                    note="原始 6 字节：[13,4,4][12,4,2]",
                    ack_timeout_ms=800,
                    sync_timeout_ms=800,
                )
            ],
        ),
        "protocol_partial": Suite(
            name="protocol_partial",
            description="探索 TCP 半包：拆开发送一个请求，记录 ACK、同步和连接状态。",
            exploratory=True,
            steps=[
                Step(
                    name="red sends split packet",
                    actor="red",
                    kind="partial",
                    chunks=[bytes([29, 5]), bytes([4])],
                    delays_ms=[200],
                    exploratory=True,
                    note="先发前 2 字节，再延迟 200ms 发第 3 字节。",
                    ack_timeout_ms=900,
                    sync_timeout_ms=900,
                )
            ],
        ),
    }
    return suites


def list_suites(suites: Dict[str, Suite]) -> None:
    print("Available suites:")
    for name, suite in suites.items():
        tag = "exploratory" if suite.exploratory else "deterministic"
        print(f"- {name}: {suite.description} [{tag}]")


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ChineseChess network auto test client")
    parser.add_argument("--host", default="127.0.0.1", help="server host")
    parser.add_argument("--port", type=int, default=12345, help="server port")
    parser.add_argument(
        "--suite",
        required=True,
        choices=[
            "smoke",
            "horse_leg",
            "elephant_eye",
            "guard_rules",
            "movement_rules",
            "palace_elephant",
            "capture_state",
            "self_check",
            "three_strikes",
            "repetition_draw",
            "perpetual_chase",
            "perpetual_check",
            "stalemate_or_no_legal_move",
            "protocol_concat",
            "protocol_partial",
            "list",
        ],
        help="suite name or list",
    )
    parser.add_argument("--timeout-ms", type=int, default=DEFAULT_TIMEOUT_MS, help="socket timeout in ms")
    parser.add_argument("--strict-exploratory", action="store_true", help="treat exploratory exceptions as failures")
    parser.add_argument("--json-out", help="optional json report path")
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    suites = build_suites()
    if args.suite == "list":
        list_suites(suites)
        return 0

    suite = suites[args.suite]
    runner = SuiteRunner(args.host, args.port, args.timeout_ms, args.strict_exploratory)
    ok, results = runner.run_suite(suite)

    if args.json_out:
        report = {
            "suite": suite.name,
            "description": suite.description,
            "host": args.host,
            "port": args.port,
            "timeout_ms": args.timeout_ms,
            "strict_exploratory": args.strict_exploratory,
            "results": results,
        }
        with open(args.json_out, "w", encoding="utf-8") as fp:
            json.dump(report, fp, ensure_ascii=False, indent=2)
        print(f"JSON report written to {args.json_out}")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
