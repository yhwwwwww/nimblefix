#!/usr/bin/env python3

import argparse
import pathlib
import shutil
import sys
import threading
import time

import quickfix as fix


SOH = "\x01"
DICTIONARY_PATH = pathlib.Path(__file__).with_name("FastFixEchoFIX44.xml")


def render_fix(message: fix.Message) -> str:
    return message.toString().replace(SOH, "|")


class EchoApplication(fix.Application):
    def __init__(self, mode: str, outbound_party_id: str, expected_party_id: str):
        super().__init__()
        self.mode = mode
        self.outbound_party_id = outbound_party_id
        self.expected_party_id = expected_party_id
        self.session_id = None
        self.logon_seen = threading.Event()
        self.app_complete = threading.Event()
        self.logout_seen = threading.Event()
        self.failed = threading.Event()
        self.failure_text = ""
        self.sent_application = False

    def onCreate(self, session_id):
        self.session_id = session_id
        print(f"onCreate {session_id.toString()}", flush=True)

    def onLogon(self, session_id):
        self.session_id = session_id
        self.logon_seen.set()
        print(f"onLogon {session_id.toString()}", flush=True)
        if self.mode == "initiator" and not self.sent_application:
            self.sent_application = True
            self.send_application(session_id, self.outbound_party_id)

    def onLogout(self, session_id):
        print(f"onLogout {session_id.toString()}", flush=True)
        self.logout_seen.set()

    def toAdmin(self, message, session_id):
        print(f"toAdmin {render_fix(message)}", flush=True)

    def fromAdmin(self, message, session_id):
        print(f"fromAdmin {render_fix(message)}", flush=True)

    def toApp(self, message, session_id):
        print(f"toApp {render_fix(message)}", flush=True)

    def fromApp(self, message, session_id):
        rendered = render_fix(message)
        print(f"fromApp {rendered}", flush=True)
        if "|35=D|" not in rendered:
            self.fail(f"unexpected application message: {rendered}")
            return
        if "|453=1|" not in rendered:
            self.fail(f"echoed application message missing NoPartyIDs=1: {rendered}")
            return
        if self.expected_party_id and f"|448={self.expected_party_id}|" not in rendered:
            self.fail(
                f"echoed application message missing PartyID={self.expected_party_id}: {rendered}")
            return

        if self.mode == "acceptor":
            try:
                fix.Session.sendToTarget(message, session_id)
            except Exception as exc:  # pragma: no cover - QuickFIX exception wrappers vary.
                self.fail(f"failed to echo application message: {exc}")
                return

        self.app_complete.set()
        self.request_logout(session_id)

    def fail(self, text: str):
        if self.failed.is_set():
            return
        self.failure_text = text
        self.failed.set()
        print(text, file=sys.stderr, flush=True)

    def request_logout(self, session_id):
        session = fix.Session.lookupSession(session_id)
        if session is None:
            self.fail("failed to look up QuickFIX session for logout")
            return
        session.logout("external-interoperability-complete")

    def send_application(self, session_id, party_id: str):
        message = fix.Message()
        header = message.getHeader()
        header.setField(fix.MsgType("D"))
        group = fix.Group(453, 448)
        group.setField(fix.StringField(448, party_id))
        group.setField(fix.StringField(447, "D"))
        group.setField(fix.IntField(452, 3))
        message.addGroup(group)
        try:
            fix.Session.sendToTarget(message, session_id)
        except Exception as exc:  # pragma: no cover - QuickFIX exception wrappers vary.
            self.fail(f"failed to send application message: {exc}")


def write_settings(args, work_dir: pathlib.Path) -> pathlib.Path:
    store_dir = work_dir / "store"
    log_dir = work_dir / "log"
    shutil.rmtree(work_dir, ignore_errors=True)
    store_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)

    lines = [
        "[DEFAULT]",
        f"ConnectionType={args.mode}",
        "StartTime=00:00:00",
        "EndTime=00:00:00",
        "NonStopSession=Y",
        "HeartBtInt=5",
        "UseDataDictionary=Y",
        f"DataDictionary={DICTIONARY_PATH}",
        "ValidateIncomingMessage=N",
        "ValidateUserDefinedFields=N",
        "ResetOnLogon=Y",
        f"FileStorePath={store_dir}",
        f"FileLogPath={log_dir}",
        "",
        "[SESSION]",
        "BeginString=FIX.4.4",
        f"SenderCompID={args.sender}",
        f"TargetCompID={args.target}",
    ]

    if args.mode == "initiator":
        lines.extend([
            f"SocketConnectHost={args.host}",
            f"SocketConnectPort={args.port}",
            "ReconnectInterval=1",
        ])
    else:
        lines.extend([
            f"SocketAcceptAddress={args.host}",
            f"SocketAcceptPort={args.port}",
        ])

    settings_path = work_dir / "session.cfg"
    settings_path.write_text("\n".join(lines) + "\n", encoding="ascii")
    return settings_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run a minimal QuickFIX/Python echo peer")
    parser.add_argument("--mode", choices=("initiator", "acceptor"), required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--sender", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--outbound-party-id", default="QF-PARTY")
    parser.add_argument("--expected-party-id", default="")
    parser.add_argument("--timeout-seconds", type=float, default=20.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    work_dir = pathlib.Path(args.work_dir)
    settings_path = write_settings(args, work_dir)
    settings = fix.SessionSettings(str(settings_path))
    application = EchoApplication(args.mode, args.outbound_party_id, args.expected_party_id)
    store_factory = fix.FileStoreFactory(settings)
    log_factory = fix.FileLogFactory(settings)

    if args.mode == "initiator":
        engine = fix.SocketInitiator(application, store_factory, settings, log_factory)
    else:
        engine = fix.SocketAcceptor(application, store_factory, settings, log_factory)

    engine.start()
    deadline = time.monotonic() + args.timeout_seconds
    try:
        while time.monotonic() < deadline:
            if application.failed.is_set():
                return 1
            if application.app_complete.is_set() and application.logout_seen.is_set():
                return 0
            time.sleep(0.05)
        print("QuickFIX peer timed out waiting for completion", file=sys.stderr, flush=True)
        return 1
    finally:
        engine.stop()


if __name__ == "__main__":
    sys.exit(main())