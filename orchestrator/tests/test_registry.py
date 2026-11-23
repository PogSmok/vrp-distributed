from vrp_orchestrator import vrp_pb2
from vrp_orchestrator.registry import QUEUE_SIZE, Registry


# A worker is never left connected to a session that gets nothing (ADR 0021).
def test_a_session_ends_when_replaced_or_when_its_queue_fills() -> None:
    registry = Registry()
    old = registry.open("w1", "w1:9100")
    new = registry.open("w1", "w1:9100")
    assert old.closed
    assert old.queue.get_nowait() is None

    # The old stream ending later must not forget the new session.
    registry.close("w1", old)
    assert registry.count == 1
    assert registry.targets == ("w1:9100",)

    for _ in range(QUEUE_SIZE + 1):
        registry.send("w1", vrp_pb2.OrchestratorMessage())
    assert new.closed
