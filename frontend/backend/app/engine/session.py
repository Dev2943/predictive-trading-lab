"""The session driver: one writer, snapshot readers.

THE CONCURRENCY MODEL, WHICH IS THE WHOLE POINT OF THIS FILE.

FastAPI serves requests concurrently. The engine is single-threaded and stateful,
and that is not incidental -- it is what buys deterministic replay and
replay/live parity. Two request handlers touching one session concurrently would
corrupt it.

So:

    request handlers                    driver thread (exactly one)
          |                                     |
          |-- enqueue command --> Queue -->  drains between steps
          |                                     |
          |<-- read ------------ snapshot <-- published after each step

  * WRITES are commands, not calls. A handler never touches the session.
  * READS come from an immutable snapshot dict the driver publishes. Readers
    never touch the session either, so no lock sits on the trading path.
  * The engine is stepped by ONE thread for its whole life. Determinism is
    preserved exactly: a command applied between steps is indistinguishable
    from one issued at that instant.

There is no mutex around the engine, and there must never be one. A lock there
would serialise readers against the trading loop, so opening a dashboard would
slow trading -- an observability layer changing trading behaviour, which the
engine's own Phase 16 was built to avoid.
"""

from __future__ import annotations

import enum
import json
import queue
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Protocol


class SessionState(str, enum.Enum):
    """The lifecycle the API promises.

    Deliberately distinct from the engine's own `SessionPhase`: that describes
    engine internals, this describes the API contract. Keeping them separate
    means an engine rename is not an API break.
    """

    STOPPED = "STOPPED"
    STARTING = "STARTING"
    RUNNING = "RUNNING"
    STOPPING = "STOPPING"
    ERROR = "ERROR"


# Which transitions are legal. Anything absent is a 409.
#
# Note what is missing: RUNNING -> STARTING. Starting a session that is already
# running would abandon the first one's book without closing it out.
_LEGAL: dict[SessionState, set[SessionState]] = {
    SessionState.STOPPED: {SessionState.STARTING},
    SessionState.STARTING: {SessionState.RUNNING, SessionState.ERROR},
    SessionState.RUNNING: {SessionState.STOPPING},
    SessionState.STOPPING: {SessionState.STOPPED, SessionState.ERROR},
    SessionState.ERROR: {SessionState.STARTING, SessionState.STOPPED},
}


def transition_allowed(current: SessionState, target: SessionState) -> bool:
    return target in _LEGAL.get(current, set())


class IllegalTransition(RuntimeError):
    """A lifecycle request the current state does not permit.

    Routes turn this into 409. It is distinct from a failure: the request was
    understood and refused because of *when* it arrived.
    """

    def __init__(self, current: SessionState, requested: str) -> None:
        super().__init__(
            f"cannot {requested} while the session is {current.value}"
        )
        self.current = current
        self.requested = requested


class SessionBackend(Protocol):
    """What the driver needs from the engine.

    A protocol, so the driver is testable without a compiled module -- and so a
    remote engine can back it later without the driver noticing.
    """

    def session_start(self, **kwargs: Any) -> bool: ...
    def session_stop(self) -> bool: ...
    def session_step(self, max_events: int) -> int: ...
    def session_state(self) -> str: ...
    def session_snapshot(self) -> str: ...
    def session_submit_order(self, **kwargs: Any) -> int: ...
    def session_cancel_order(self, order_id: int) -> bool: ...
    def session_cancel_all(self) -> int: ...
    def session_flatten(self) -> int: ...
    def session_set_halted(self, halted: bool) -> bool: ...


@dataclass
class _Command:
    name: str
    kwargs: dict[str, Any] = field(default_factory=dict)
    done: threading.Event = field(default_factory=threading.Event)
    error: str | None = None
    result: Any = None


class SessionDriver:
    """Owns the one session and the one thread allowed to touch it."""

    def __init__(self, backend: SessionBackend, *, step_size: int = 25) -> None:
        self._backend = backend
        self._step_size = step_size
        self._commands: queue.Queue[_Command] = queue.Queue()
        self._thread: threading.Thread | None = None
        self._stop_thread = threading.Event()

        self._state = SessionState.STOPPED
        self._error: str | None = None
        # Replaced wholesale, never mutated in place. Rebinding a name is
        # atomic under the GIL, so a reader always sees a complete snapshot
        # rather than one caught mid-update.
        self._snapshot: dict[str, Any] = {"state": {"state": "STOPPED"}}
        self._exhausted = False

    # --- reader side: never touches the session ---------------------------

    @property
    def state(self) -> SessionState:
        return self._state

    def snapshot(self) -> dict[str, Any]:
        return self._snapshot

    def status(self) -> dict[str, Any]:
        snapshot = self._snapshot
        return {
            "state": self._state.value,
            "error": self._error,
            "replay_exhausted": self._exhausted,
            "engine": snapshot.get("state", {}),
        }

    # --- writer side: commands only ---------------------------------------

    def start(self, **kwargs: Any) -> None:
        self._require(SessionState.STARTING, "start")
        self._ensure_thread()
        self._submit(_Command("start", kwargs))

    def stop(self) -> None:
        self._require(SessionState.STOPPING, "stop")
        self._submit(_Command("stop"))

    def reset(self, **kwargs: Any) -> None:
        """Stop, then start a new session.

        NOT an in-place reset. Discarding a book where it stands would skip the
        close-out that reconciles the journal, so this is two lifecycle steps
        with the same guarantees as doing them by hand.
        """
        if self._state is SessionState.RUNNING:
            self.stop()
            self._await_state(SessionState.STOPPED)
        self.start(**kwargs)

    # --- trading commands --------------------------------------------------
    #
    # Same queue, same single writer. An order entered here is applied by the
    # driver between steps and reaches the engine at the next event, exactly as
    # a lifecycle command does. A request handler never touches the session.

    def submit_order(self, **kwargs: Any) -> int:
        self._require_running("submit an order")
        command = _Command("submit_order", kwargs)
        self._submit(command)
        return int(command.result or 0)

    def cancel_order(self, order_id: int) -> None:
        self._require_running("cancel an order")
        self._submit(_Command("cancel_order", {"order_id": order_id}))

    def cancel_all(self) -> int:
        self._require_running("cancel orders")
        command = _Command("cancel_all")
        self._submit(command)
        return int(command.result or 0)

    def flatten(self) -> int:
        self._require_running("flatten")
        command = _Command("flatten")
        self._submit(command)
        return int(command.result or 0)

    def set_halted(self, halted: bool) -> bool:
        """Suppress or resume strategy order generation.

        A FLAG, not a lifecycle transition. The session stays RUNNING, data
        keeps flowing and the book keeps marking -- halting is the control
        between `flatten` (acts on the book) and `stop` (tears the session
        down), which F6 left missing.
        """
        self._require_running("halt the strategy")
        command = _Command("set_halted", {"halted": halted})
        self._submit(command)
        return bool(command.result)

    def _require_running(self, verb: str) -> None:
        """Trading commands need a RUNNING session.

        Raised as IllegalTransition so routes answer 409 -- the request was
        well-formed and refused for when it arrived, which is the same shape as
        a double start.
        """
        if self._state is not SessionState.RUNNING:
            raise IllegalTransition(self._state, verb)

    def shutdown(self) -> None:
        """Stop the driver thread. For process teardown and tests.

        Stops from ERROR as well as RUNNING. The C++ host is one instance per
        process, so a driver that gave up here would leave a session behind and
        the next start would be refused -- a failure in one place surfacing as a
        confusing error somewhere unrelated.
        """
        if self._state in (SessionState.RUNNING, SessionState.ERROR):
            try:
                if self._state is SessionState.ERROR:
                    # ERROR -> STOPPED is legal; force the transition so the
                    # underlying host is released.
                    self._state = SessionState.RUNNING
                self.stop()
                self._await_state(SessionState.STOPPED, timeout=5.0)
            except (IllegalTransition, RuntimeError):
                pass
        self._stop_thread.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)
            self._thread = None

    # --- internals ---------------------------------------------------------

    def _require(self, target: SessionState, verb: str) -> None:
        if not transition_allowed(self._state, target):
            raise IllegalTransition(self._state, verb)

    def _submit(self, command: _Command) -> None:
        self._commands.put(command)
        # Wait for the driver to apply it, so the HTTP response reflects the
        # outcome rather than the request merely having been queued. A client
        # that gets 200 and then sees STOPPED would have no way to tell whether
        # its command was accepted.
        if not command.done.wait(timeout=30.0):
            raise RuntimeError(f"the session driver did not apply '{command.name}'")
        if command.error is not None:
            raise RuntimeError(command.error)

    def _await_state(self, target: SessionState, timeout: float = 10.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._state is target:
                return
            time.sleep(0.005)
        raise RuntimeError(f"timed out waiting for {target.value}")

    def _ensure_thread(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop_thread.clear()
        self._thread = threading.Thread(
            target=self._run, name="ptl-session-driver", daemon=True
        )
        self._thread.start()

    def _run(self) -> None:
        """The ONLY thread that touches the session."""
        while not self._stop_thread.is_set():
            # Commands are drained BETWEEN steps, never during one. That is what
            # makes a pause or a stop land at an event boundary rather than
            # halfway through processing one.
            try:
                command = self._commands.get(timeout=0.02)
            except queue.Empty:
                self._advance()
                continue

            try:
                self._apply(command)
            except Exception as exc:  # noqa: BLE001 - reported, never swallowed
                command.error = str(exc)
                # A FAILED TRADING COMMAND IS NOT A FAILED SESSION.
                #
                # An unknown symbol or a bad quantity is the caller's mistake,
                # and the session is still perfectly healthy. Driving it to
                # ERROR would let one mistyped order halt trading and force a
                # restart -- and it would leave the book unattended for a reason
                # that had nothing to do with the book.
                #
                # Lifecycle failures are different: if start or stop failed, the
                # session really is in an unknown state.
                if command.name in ("start", "stop"):
                    self._state = SessionState.ERROR
                    self._error = str(exc)
            finally:
                command.done.set()

    def _apply(self, command: _Command) -> None:
        if command.name == "start":
            self._state = SessionState.STARTING
            self._error = None
            self._exhausted = False
            self._backend.session_start(**command.kwargs)
            self._state = SessionState.RUNNING
            self._publish()
        elif command.name == "stop":
            self._state = SessionState.STOPPING
            self._backend.session_stop()
            self._state = SessionState.STOPPED
            self._publish()
        elif command.name == "submit_order":
            command.result = self._backend.session_submit_order(**command.kwargs)
        elif command.name == "cancel_order":
            self._backend.session_cancel_order(command.kwargs["order_id"])
        elif command.name == "cancel_all":
            command.result = self._backend.session_cancel_all()
        elif command.name == "flatten":
            command.result = self._backend.session_flatten()
        elif command.name == "set_halted":
            command.result = self._backend.session_set_halted(command.kwargs["halted"])
        else:  # pragma: no cover - unreachable by construction
            raise RuntimeError(f"unknown command '{command.name}'")

    def _advance(self) -> None:
        if self._state is not SessionState.RUNNING:
            return
        try:
            processed = self._backend.session_step(self._step_size)
        except Exception as exc:  # noqa: BLE001
            self._state = SessionState.ERROR
            self._error = str(exc)
            return

        if processed == 0:
            # The replay is exhausted. NOT an error, and not a reason to stop:
            # the book stays as it is and remains readable, exactly as a live
            # session between market events.
            self._exhausted = True
            time.sleep(0.05)
        self._publish()

    def _publish(self) -> None:
        try:
            self._snapshot = json.loads(self._backend.session_snapshot())
        except Exception as exc:  # noqa: BLE001
            self._error = f"snapshot failed: {exc}"
