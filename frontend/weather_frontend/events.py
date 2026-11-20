"""In-process broadcaster from the MQTT ingestion to the SSE streams (FR-4.3)."""

from __future__ import annotations

import asyncio
import logging

log = logging.getLogger(__name__)

QUEUE_SIZE = 200


class Subscriber:
    def __init__(self) -> None:
        self.queue: asyncio.Queue = asyncio.Queue(maxsize=QUEUE_SIZE)
        self.dropped = False


class Broadcaster:
    def __init__(self) -> None:
        self._subscribers: set[Subscriber] = set()
        self.mqtt_connected = False  # NFR-2.2, NFR-2.4

    def subscribe(self) -> Subscriber:
        sub = Subscriber()
        self._subscribers.add(sub)
        return sub

    def unsubscribe(self, sub: Subscriber) -> None:
        self._subscribers.discard(sub)

    def publish(self, event: dict) -> None:
        """Hands an event to every open stream. A stream that does not keep up is
        closed; its browser reconnects and reloads everything (FR-4.3)."""
        for sub in list(self._subscribers):
            try:
                sub.queue.put_nowait(event)
            except asyncio.QueueFull:
                sub.dropped = True
                self._subscribers.discard(sub)
                log.warning("Event stream too slow, closed")

    def set_mqtt_connected(self, connected: bool) -> None:
        if connected != self.mqtt_connected:
            self.mqtt_connected = connected
            self.publish({"type": "mqtt", "connected": connected})

    @property
    def subscriber_count(self) -> int:
        return len(self._subscribers)
