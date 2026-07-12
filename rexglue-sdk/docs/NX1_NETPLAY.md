# NX1 Netplay

This document describes the ReXGlue NX1 System Link netplay path. The short
version is:

- The XLive web API is the discovery and presence control plane.
- Actual game traffic is still direct UDP between players.
- ReXGlue makes WAN sessions look like normal Xbox 360 System Link sessions to
  the game by synthesizing LAN search responses and translating XNet addresses.

This is a ReXGlue implementation note for NX1. It is not a complete
specification of Xenia's netplay fork or the public web service.

The public API currently used by the runtime defaults to:

```toml
xlive_web_api_address = "https://xenia-netplay-2a0298c0e3f4.herokuapp.com/"
```

## Code Map

The netplay implementation is split across these files:

- `src/system/xlive_web_client.cpp`
  REST client, web registration, session create/search/join/leave/delete, QoS
  upload/download, stale cleanup commands.
- `src/kernel/xam/apps/xgi_app.cpp`
  XGI session task bridge. Hooks the title's `XSession*` calls and mirrors them
  to the web API.
- `src/kernel/xam/xam_net.cpp`
  XNet and NetDll shim. Handles XNADDR conversion, XNKID/XNKEY generation,
  System Link port reporting, XNet QoS, and address resolution.
- `src/system/xsocket.cpp`
  UDP socket bridge. Intercepts System Link broadcasts, synthesizes LAN
  `infoResponse` packets from web sessions, rewrites same-public-IP loopback
  joins, applies local port offsets, and disables Windows UDP reset noise.
- `src/system/xsession.cpp`
  Local session object state, generated System Link session IDs, advertised host
  address, slots, nonce, and exchange key.
- `src/system/runtime.cpp`
  Calls web cleanup on shutdown.

## Architecture

There are two separate paths.

### Control Plane: XLive Web API

The control plane is HTTP/HTTPS. It answers "who am I on the internet?", "which
players are online?", and "which sessions exist for this title?" ReXGlue uses it
for discovery and bookkeeping only.

Important API calls:

| Runtime operation | HTTP call |
| --- | --- |
| Probe public address | `GET /whoami` |
| Register local player | `POST /players` |
| Find player by public IP | `POST /players/find` |
| Create host session | `POST /title/:titleId/sessions` |
| Search sessions | `POST /title/:titleId/sessions/search` |
| Fetch one session | `GET /title/:titleId/sessions/:sessionId` |
| Join session member list | `POST /title/:titleId/sessions/:sessionId/join` |
| Prejoin non-host member list | `POST /title/:titleId/sessions/:sessionId/prejoin` |
| Leave session member list | `POST /title/:titleId/sessions/:sessionId/leave` |
| Delete one session | `DELETE /title/:titleId/sessions/:sessionId` |
| Delete stale sessions | `DELETE /DeleteSessions` or `DELETE /DeleteSessions/:mac` |
| Upload QoS payload | `POST /title/:titleId/sessions/:sessionId/qos` |
| Download QoS payload | `GET /title/:titleId/sessions/:sessionId/qos` |

### Data Plane: Direct UDP

The API does not relay gameplay packets. Once a remote session appears in the
System Link menu, the game sends normal UDP traffic to the host address and
advertised System Link port.

That means internet play still depends on:

- the host's firewall allowing inbound UDP;
- NAT/port forwarding/DMZ allowing inbound UDP to the host machine;
- both players using compatible title IDs, assets, and configs;
- both players using the same web API base address.

If the session appears in the menu but joining times out, discovery worked and
the failure is usually in the direct UDP path.

## Startup Flow

On startup, `XLiveWebClient::EnsureReadyLocked` prepares the web client:

1. Read cvars from the runtime TOML.
2. `GET /whoami` to learn the public IPv4 address.
3. `POST /players` with:
   - local XUID;
   - gamertag;
   - machine ID;
   - public host address;
   - synthetic MAC address derived from XUID.
4. Optionally delete stale sessions for this profile or public IP.

Useful log lines when this works:

```text
XLive web /whoami -> <public-ip>
XLive web /players registered <gamertag> (<xuid>)
```

You can force a status check from the in-game console:

```text
xlive_web_status
```

## Hosting Flow

When the game hosts a System Link match:

1. The game calls XNet key functions.
2. `NetDll_XNetCreateKey` generates a System Link XNKID.
3. `NetDll_XNetRegisterKey` stores it as the active System Link session ID.
4. The game creates a host session through XGI.
5. `xgi_app.cpp` handles task `0x000B0010`.
6. `XSession::CreateHostSession` fills the local `XSESSION_INFO`:
   - generated session ID;
   - local IP address;
   - advertised System Link port;
   - synthetic MAC from XUID;
   - exchange key;
   - nonce.
7. `XLiveWebClient::CreateSession` posts the session to the API.
8. The host XUID is also posted to the session's member list.

The web payload includes title/media/version identity, slots, flags, host
address, MAC address, session ID, and port.

The advertised port is:

```text
systemlink_base_port + systemlink_port_offset
```

By default this is `1001`.

## Search Flow

NX1 still behaves like a LAN System Link game. It expects to find matches by
sending UDP `getinfo` probes. Those probes do not cross the internet, so ReXGlue
bridges them.

When the game sends a System Link broadcast:

1. `XSocket::SendTo` detects a UDP broadcast or `getinfo` probe.
2. `GetBroadcastBridgeSessions` searches the web API for active sessions.
3. Sessions with the local synthetic MAC are filtered out.
4. If `xlive_web_bridge_synthesize_lan_info` is true, ReXGlue builds a fake LAN
   `infoResponse` packet for each remote web session.
5. The packet is queued into the socket with `XSocket::QueuePacket`.
6. `NetDll_select` reports the socket readable because it has queued packets.
7. `NetDll_recvfrom` returns the synthetic packet to the game.
8. The System Link menu sees the remote internet session as if it were a LAN
   server.

The synthetic `infoResponse` includes:

- `xnaddr`
- `xnkid`
- `xnkey`
- slot counts
- nonce
- basic server metadata

This is why a session can appear in the System Link list even though no actual
LAN broadcast reached the remote host.

## Join Flow

When the player selects a listed session:

1. The game parses `xnaddr`, `xnkid`, and `xnkey` from the synthetic
   `infoResponse`.
2. The game asks XNet to convert the XNADDR to an IP address.
3. `NetDll_XNetXnAddrToInAddr` stores the resolved XNADDR in a cache.
4. `NetDll_XNetInAddrToXnAddr` can later recover the same XNADDR from that
   cache, or resolve it through `/players/find`.
5. If the remote session is on the same public IP and
   `xlive_web_bridge_loopback_same_public_ip` is true, the address is rewritten
   to `127.0.0.1`.
6. `XSocket::Connect` and `XSocket::SendTo` rewrite System Link ports when the
   remote session uses a different `systemlink_port_offset`.
7. UDP packets are sent directly to the resolved host and port.
8. XGI session join tasks post member updates to the API.

For two local instances, use different directories and different
`systemlink_port_offset` values. Example:

```toml
# Instance A
systemlink_port_offset = 0

# Instance B
systemlink_port_offset = 100
```

With the default base port, instance B advertises `1101` and binds the System
Link companion ports at `1100`, `1101`, and `1113`.

## Ports

The base System Link port is controlled by:

```toml
systemlink_base_port = 1001
```

The bridge treats these guest ports as System Link ports:

- `systemlink_base_port - 1`
- `systemlink_base_port`
- `systemlink_base_port + 12`

With the default base port, that is:

```text
1000/udp
1001/udp
1013/udp
```

The native bind port is:

```text
guest_port + systemlink_port_offset
```

For internet hosting, the host should allow/forward the native UDP ports. For a
single default instance that means `1000`, `1001`, and `1013`. For an offset of
`100`, that means `1100`, `1101`, and `1113`.

## QoS

NX1 uses XNet QoS calls while deciding how to join and how to treat peers.

Implemented behavior:

- `XNetQosListen` caches local QoS data and posts it to the web API.
- `XNetQosLookup` fetches QoS data for remote sessions when available.
- Lookup results are marked complete and contacted.
- RTT and fallback bandwidth are configurable because the current web API path
  does not measure true peer-to-peer latency.

Relevant cvars:

```toml
xlive_web_qos_rtt_min_ms = 35
xlive_web_qos_rtt_median_ms = 70
xlive_web_qos_up_bits_per_second = 8388608
xlive_web_qos_down_bits_per_second = 8388608
```

If internet play works but rubberbands badly, try increasing
`xlive_web_qos_rtt_median_ms` to `100` or `120`. Reporting a perfect LAN-like
ping can make the game assume timing that a WAN connection cannot actually
deliver.

## Cleanup

Stale sessions are cleaned up in several places:

- startup can prune sessions for the current profile;
- `XSessionEnd` and host session delete call `DeleteSessionOnEnd`;
- shutdown can prune all sessions from the current public IP;
- the console command `xlive_web_prune_sessions` deletes stale sessions for the
  current public IP.

Relevant cvars:

```toml
xlive_web_delete_stale_on_startup = true
xlive_web_delete_stale_for_public_ip = false
xlive_web_delete_session_on_end = true
xlive_web_prune_profile_on_session_end = true
xlive_web_prune_public_ip_on_shutdown = true
```

## Runtime Configuration

Common user identity settings:

```toml
user_gamertag = "User"
user_xuid = ""
```

If `user_xuid` is empty, ReXGlue derives one from the gamertag and machine.
Players should use distinct gamertags/XUIDs.

Web client settings:

```toml
xlive_web_enabled = true
xlive_web_api_address = "https://xenia-netplay-2a0298c0e3f4.herokuapp.com/"
xlive_web_timeout_ms = 2000
xlive_web_log_requests = true
xlive_web_probe_on_startup = true
xlive_web_advertise_systemlink = true
```

Bridge settings:

```toml
xlive_web_bridge_systemlink_broadcast = true
xlive_web_bridge_broadcast_cache_ms = 2000
xlive_web_bridge_broadcast_max_hosts = 32
xlive_web_bridge_log_packets = false
xlive_web_bridge_synthesize_lan_info = true
xlive_web_bridge_loopback_same_public_ip = true
systemlink_base_port = 1001
systemlink_port_offset = 0
```

Keep `xlive_web_bridge_log_packets` off during real play. It is useful for
debugging discovery, but it can add avoidable jitter if left on while packets
are flowing.

## Verification

Useful console commands:

```text
xlive_web_status
xlive_web_sessions
xlive_web_prune_sessions
```

Expected signs of a healthy setup:

- `xlive_web_status` logs `connected`.
- The log shows `/whoami` and `/players registered`.
- The host logs a web session creation.
- The web site shows the host's gamertag, not just `Player 1`.
- The joining player can see the session in the System Link menu.
- Joining player sends UDP directly to the host's advertised address and port.

## Troubleshooting

### The System Link list is empty

Check:

- both players have `xlive_web_enabled = true`;
- both players use the same `xlive_web_api_address`;
- both builds report the same title ID;
- `xlive_web_bridge_systemlink_broadcast = true`;
- `xlive_web_bridge_synthesize_lan_info = true`;
- `xlive_web_sessions` returns the host session;
- the host is not being filtered as the local MAC because both instances share
  the same XUID.

### The session appears, but joining times out

Discovery worked. The problem is likely direct UDP.

Check:

- host firewall allows inbound UDP;
- router forwards or DMZs the native System Link ports;
- the session's advertised port matches the host's forwarded port;
- local multi-instance offsets are not colliding;
- both players are using the same rebuilt executable and compatible cfgs.

Remember that the public API does not relay gameplay packets.

### Two local instances cannot see or join each other

Use separate runtime directories and separate configs. Give one instance a
non-zero port offset:

```toml
systemlink_port_offset = 100
```

The bridge rewrites same-public-IP sessions to loopback when:

```toml
xlive_web_bridge_loopback_same_public_ip = true
```

### Rubberbanding or poor ping

Check:

- `xlive_web_bridge_log_packets = false`;
- no antivirus/firewall is inspecting every UDP packet;
- the host is not using a congested VPN path;
- `xlive_web_qos_rtt_median_ms` is not unrealistically low;
- both players have stable direct UDP reachability.

Try:

```toml
xlive_web_qos_rtt_median_ms = 100
```

or:

```toml
xlive_web_qos_rtt_median_ms = 120
```

### Stale games remain on the web site

Run:

```text
xlive_web_prune_sessions
```

Also keep these enabled:

```toml
xlive_web_delete_session_on_end = true
xlive_web_prune_public_ip_on_shutdown = true
```

## Current Limitations

- The web API is not a packet relay.
- NAT traversal is limited to whatever direct UDP reachability the players
  already have.
- QoS RTT values are configurable estimates, not measured peer-to-peer latency.
- The synthetic `infoResponse` currently uses generic server metadata where the
  API does not expose exact map/gametype state.
- The bridge is tailored for NX1's System Link query behavior and known ports.
