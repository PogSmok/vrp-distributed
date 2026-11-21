# 0022. Persist state transitions before sending messages

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** 0013, 0014, 0016, 0021

## Context

Every scheduling decision both changes the database and sends a message. A
crash between the two would make recovery wrong, in either direction.

## Decision

- **Outgoing.** The orchestrator commits first, then applies the change in
  memory and sends its messages (0021). Every message that starts work
  carries the task ID and attempt number, and receivers ignore repeats. A
  lost message is covered by the lease timeout.
- **Incoming.** Workers keep reports that are not yet confirmed (only the
  latest best solution), and resend them after reconnecting. Heartbeats need
  no confirmation.
- **Progress.** Clients poll about once a second with the last convergence
  index they have. They receive the phase, the newer points and the current
  best cost; a finished job points to its result (0016).

## Alternatives considered

- **Send first, commit afterwards:** a crash leaves work that the database
  does not know about, and it is assigned twice.
- **A transactional outbox:** designed for message brokers; our streams are
  gone after a crash anyway.

## Consequences

- Gain: recovery needs only the database, and duplicate messages are
  harmless.
- Cost: each assignment and confirmation waits for a commit (milliseconds).

## Confirmation

Kill the orchestrator between a commit and its send, and again between
receiving a completion and committing it. Each task is assigned exactly
once, and the resent report is recorded.
