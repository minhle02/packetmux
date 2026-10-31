# M9 — SDK gaps: design

Status: implemented, except the removal check in §7, which runs with the final testing pass.
Plan: `docs/requirements/IMPROVEMENTS.md`, requests 5, 7 and 8.

Three small additions from the RemoteDevice integration. None of them touches the wire, the core or
the C ABI. The pseudocode is Python, and TypeScript follows it with its usual naming.

| Change | Python | TypeScript |
|---|---|---|
| Cancelling `connect` stops the client | Code change | Works today, gains the same scenarios |
| Hook for a session that ended | `on_session_closed` | `onSessionClosed` |
| Active transport of a server session | `session.transport` | `session.transport` |

## 1. Cancelling `connect`

Today `connect()` handles only `TimeoutError`. A cancelled call leaves the client alive with nobody
holding it, and its dial loop retries forever (D15). TypeScript already handles this through `signal`.

```python
async def connect(dial, *, handlers=None, capabilities=b"", options=None) -> Client:
    ...
    try:
        await wait_for(shield(client._first_outcome), options.connect_timeout)
    except TimeoutError:
        ...                                       # unchanged
    except CancelledError:
        client._first_outcome.cancel()
        client._fail(NoConnection("connect was cancelled"))
        raise
    ...
```

`_fail` closes the session, the client enters `_ClosedState`, and leaving the dialing state cancels the
dial loop. No other code changes.

- The caller sees `CancelledError`, as with any cancelled coroutine. TypeScript rejects with the abort
  error, as it does today.
- If the handshake finished just before the cancel, the session is still closed. The caller never
  received the client, so nothing else could close it.
- `connect` does not wait for the close to finish, because a cancelled task should end promptly. The
  connection shuts down in the background within `drain_timeout`.

## 2. Hook for a session that ended

A consumer that keeps per-session state has to sweep `server.sessions()` to notice one has gone. It
cannot wait on each session, because `authorize` runs before a new session exists. So the hook is on
the server, and it is one callback for all sessions.

```python
OnSessionClosed = Callable[[ServerSession, Reason], None]

server = Server(handlers=..., authorize=..., on_session_closed=forget_session)
```

```ts
const server = new Server({ handlers, authorize, onSessionClosed: (session, code) => forget(session) })
```

```python
class ServerSession:
    def on_closed(self, code, connection):        # from the host, once per session
        self._cancel_grace()
        self._server._sessions.pop(self.session_id, None)
        self._server._notify_closed(self, code)

class Server:
    def _notify_closed(self, session, code):
        if self._on_session_closed is None:
            return
        try:
            self._on_session_closed(session, code)
        except Exception:
            log.exception("session closed listener failed")
```

- It fires exactly once per session, for every ending: `GOAWAY` from the client, grace expiry
  (`TIMEOUT`), `session.close()`, and `server.close()`.
- The session has already left `server.sessions()`, and its handlers have been told to stop.
- A connection that was refused never made a session, so it never fires the hook.
- It is synchronous, like `on_state_change`, because it runs inside the host's event drain. A consumer
  with async cleanup starts its own task. An exception is logged and goes no further.
- `session.data` is still readable, which is how a consumer finds its own state.

## 3. Active transport of a server session

`Client.transport` exists and `ServerSession` has no equivalent. The logic moves into the host, and
both read it from there.

```python
class SessionHost:
    @property
    def active_transport(self) -> Optional[Transport]:
        return self.connection.transport if self.connected and self.connection else None

class Client:
    transport = property(lambda self: self._host.active_transport)

class ServerSession:
    transport = property(lambda self: self._host.active_transport)
```

The rule matches FR-CLI-5: `None` until the handshake finishes and while disconnected. After a
re-dial it is the new Transport, never the replaced one.

## 4. Requirements

To add to `INTENT.md`:

- **FR-CLI-7** Cancelling `connect` MUST close the client and stop its dial loop, whether or not the
  handshake has finished.
- **FR-SRV-10** A consumer MUST be able to register one callback that runs exactly once for each
  session that ends, after the session has left the registry. It receives the session and the
  reason code.
- **FR-SRV-11** A server session MUST expose its active Transport, under the same rule as FR-CLI-5.

## 5. Conformance

All of this goes in the async layer, so both SDKs run the same scenarios. Four new ops, listed in
`conformance/README.md`:

| Op | Meaning |
|---|---|
| `cancel_connect` | Starts `connect`, and cancels it after 5 ms. TypeScript uses `signal` |
| `expect_session_closed` | The hook fired once, with this `code` |
| `expect_no_session_closed` | The hook has not fired |
| `expect_session_transport` | `current` or `none` |

| Scenario | Requirement |
|---|---|
| Cancelling while dials fail stops further dials | FR-CLI-7 |
| Cancelling during a hung dial leaves no session, even when the dial finishes later | FR-CLI-7 |
| The hook fires once for client `GOAWAY`, grace expiry and local close, each with its code | FR-SRV-10 |
| Inside the hook, `sessions()` no longer lists the session (the harness checks on every call) | FR-SRV-10 |
| A hook that raises does not disturb the server | FR-SRV-10 |
| A refused connection does not fire the hook | FR-SRV-10 |
| Transport is the current one when connected, none after loss, and the new one after a re-dial | FR-SRV-11 |
| Transport is the replacement when a re-dial replaces a live connection | FR-SRV-11 |

A cancel cannot be timed to land inside the handshake, because the network has no way to hold a link
before it exists. That path is the same `_fail` the connect timeout uses.

## 6. Files

| Area | Files |
|---|---|
| Python | `client.py`, `server.py`, `host.py` |
| TypeScript | `client.js`, `server.js`, `host.js`, `index.d.ts` |
| Specs and docs | `INTENT.md`, `BINDING.md`, both binding READMEs, `conformance/README.md`, `manifest.json` |
| Tests | `conformance/async/*.json`, `test_sdk_conformance.py`, `sdkConformance.test.js` |

## 7. Exit criteria

- All conformance layers pass in C++, Python and TypeScript.
- Each change, removed on its own, makes a new scenario fail.
- The new API is in the binding spec and both READMEs.
