from __future__ import annotations

import threading
import time
from typing import Any, Callable, Optional


class RadioPoller(threading.Thread):
    def __init__(
        self,
        rig_adapter: Any,
        update_callback: Callable[[dict[str, Any]], None],
        poll_interval: float = 0.5,
    ) -> None:
        super().__init__(daemon=True)
        self.rig_adapter = rig_adapter
        self.update_callback = update_callback
        self.poll_interval = poll_interval
        self._stop_event = threading.Event()
        self.last_good_state: Optional[dict[str, Any]] = None
        self.fail_count: int = 0
        self.fail_threshold: int = 3

    def run(self) -> None:
        while not self._stop_event.is_set():
            try:
                state = self.rig_adapter.get_debug_snapshot()
                self.last_good_state = state
                self.fail_count = 0
                self.update_callback(state)
            except Exception as exc:
                self.fail_count += 1
                if self.fail_count >= self.fail_threshold and self.last_good_state is not None:
                    # Pass the last known good state with an error annotation
                    # so consumers always receive a dict with the expected keys.
                    self.update_callback({**self.last_good_state, "error": str(exc)})
            time.sleep(self.poll_interval)

    def stop(self) -> None:
        self._stop_event.set()
