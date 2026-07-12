# NX1 MP — Changelog

## v0.9 — 2026-07-11

### Netplay: fixed the periodic stutter when joining another player's game

**Fixed**
- Resolved a recurring hitch (frametime dropping to ~40 FPS every 2–3 seconds) that
  occurred **only when connected to another player's session**. Hosting was unaffected.
  The game now holds a steady framerate as a client.

**What was happening**
- Every couple of seconds, the client was making a blocking call to the online session
  service (the XLive web API) on the main game thread while gameplay packets were being
  sent. That call stalled the frame long enough to cause the stutter. Because of how
  System Link ports are handled, only the *joining* player hit this path, which is why
  hosts never saw it.

**Under the hood**
- **Non-blocking session refresh:** the System Link session list now updates on a
  background thread and serves the last-known results instantly, so sending gameplay
  packets never waits on the network (stale-while-revalidate).
- **Reused web connections:** the web client now keeps a persistent HTTPS connection
  instead of doing a fresh DNS lookup + TLS handshake on every request. This makes *all*
  netplay web calls (session create/search, join, QoS) noticeably faster, not just the
  one that caused the stutter.
- **Bridge suspends in-match:** once you're in a session, the session-discovery polling
  stops entirely — a joined game's host address doesn't change mid-match, so there's
  nothing to poll. Zero web traffic during gameplay.
- **Clean shutdown:** background session refreshes are now drained on exit so they can't
  run during teardown.

**Notes**
- No configuration changes required; existing `xlive_web_*` settings still apply.
- Direct UDP connectivity requirements are unchanged (host firewall/port forwarding still
  matters for internet play).
