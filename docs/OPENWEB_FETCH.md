# openweb.fetch/1 (protocol v2)

HTTP and HTTPS fetches for an Amiga: the network and TLS run on the x86 or
ARM64 cores, or on the Amiga itself (the native transport, below), and the
Amiga gets the status, the headers and the body. It is
the first bridge service for the browser (AmigaChrome's
`MAGIC_FUNCTIONS_AND_BRIDGE.md`, section 12), and any other program may use
it. Host code: `host/openweb_fetch.c`, built into `host/libopenwebfetch.so`
(entry `wf_call`, and `wf_start` and `wf_cancel` for the services card), with
OpenSSL 3, zlib and libbrotlidec. On the Amiga: the client
`src/webfetch.c` (`include/openweb/webfetch.h`) and the `OWFetch` command
(`src/owfetch.c`).

Status: v2, 23 October 2026: the native transport on the Amiga's own
TCP/IP and OpenTLS, with HTTP/2, WebSocket (ops 8 to 10), proxies and HTTP
auth; asynchronous use from one task (`wf_async_start`, `wf_wait`). The
service's name stays `openweb.fetch/1`: v1's messages are unchanged, and
v2 only adds ops, flags and info lines. Before that, v1, 12 October 2026. Offered on the services card. Not yet on the
LAN (below). Later the same day: a waiting HEAD or READ is answered later
from the library's own threads instead of holding a card worker
(`wf_start`, below), and connections are kept per host (below). The
messages are unchanged.

## The shape

A fetch is a handle on the host side. OPEN starts it and returns at once;
the connection, the TLS handshake and the request run on a thread of its
own. The Amiga then asks for the head, and reads the body in chunks into
its own buffers, while the host reads ahead into a window. A body to upload
goes with OPEN, or in chunks with SEND. CANCEL stops one fetch; CLOSE ends
it. The connection goes back to a pool when the answer is done, for the
next fetch to the same place.

The card's completions are the rings, and openservice.device turns each
into a reply on the caller's port, so a program waits on a signal bit and
can have many fetches in flight, each with its own handle. A HEAD or READ
that has to wait is held by the library, not by one of the card's workers,
so any number can wait at once.

## Ops

| Op | Name | Request | Answer |
| --- | --- | --- | --- |
| 1 | OPEN | buf0 the request text (below); buf1 the body or its first part (with `WF_BODY`); `extra[0]` flags; `extra[1]` the body's whole length, `$FFFFFFFF` when not known; `extra[2]` the timeout in seconds (0: 30) | result the fetch's handle |
| 2 | SEND | `arg` the handle; buf0 the next part of the body; `extra[0]` bit 0: the last part; `extra[1]` the wait in ms | result the bytes taken: all of buf0, or 0 when the host's queue is full (send it again) |
| 3 | HEAD | `arg` the handle; buf0 (out) room for the head (below); `extra[0]` the wait in ms | result the HTTP status, aux the error (below). Result 0 and aux 0: the head has not come yet, ask again |
| 4 | READ | `arg` the handle; buf0 (out) room for body bytes; `extra[0]` the wait in ms | result the bytes written; aux 0 more to come, 1 the end; on a failure, 1 with the error in bits 8-15 |
| 5 | CANCEL | `arg` the handle | 0. The fetch stops now: its connection is shut, and a HEAD or READ waiting on it answers with error 6 |
| 6 | CLOSE | `arg` the handle | 0. Cancels it if it still runs; the handle is gone |
| 7 | STATS | | result the fetches open, aux their threads still running (native: connections open) |
| 8 | WS_OPEN | as OPEN, with a `ws://` or `wss://` URL and no body | result the handle; HEAD then gives the 101, or the answer that refused it |
| 9 | WS_SEND | `arg` the handle; buf0 the payload; `extra[0]` the opcode (1 text, 2 binary, 8 close, 9 ping, 10 pong), bit 7 more parts of this message follow; `extra[1]` the wait in ms | result the bytes taken: all, or 0 when the queue is full (send it again); aux 1 with the error in bits 8-15 when the connection has ended |
| 10 | WS_RECV | `arg` the handle; buf0 (out) room for payload; `extra[0]` the wait in ms | result the bytes written; aux the opcode (bits 0-3), bit 4 the message ends with these bytes, bit 5 the connection has ended (the error in bits 8-15), and for a close its code in bits 16-31. Result 0 and aux 0: nothing yet |

The services card answers -2 to ops 8 to 10 until its host library has
them (left for later); the native transport has them all.

OPEN flags (`extra[0]`):

- bit 0 `WF_BODY`: the request has a body. Its bytes are buf1, then each
  SEND, up to `extra[1]` bytes when that is known (then sent with
  `Content-Length`), or up to a SEND marked last (then sent chunked).
- bit 1 `WF_DECODE`: the host asks for `gzip, deflate, br` and undoes them
  (`br`, Brotli, when the library was built with libbrotlidec, as
  `build.sh` does when its headers are there; otherwise `gzip, deflate`).
  It drops `Content-Encoding` and the sender's `Content-Length`; HEAD gives
  the length after decoding only when the whole body is in by then. On the
  native transport see "Compressed bodies" below.
- bit 2 `WF_NOPROXY` (native): never through a proxy, whatever the
  environment says.
- bit 3 `WF_HTTP1` (native): offer only `http/1.1` by ALPN, so never HTTP/2.

A wait is how long the call may wait for something to happen before it
answers: 0 answers at once, and more than 2000 is cut to 2000.

### The request text

ISO-8859-1 or UTF-8, lines ending in LF (a CR before it is dropped):

```
GET https://example.com/path?q=1
Cookie: a=1; b=2
Accept: text/html
```

The first line is the method and the absolute URL (`http://` or
`https://`, with an optional port; an IPv6 address in brackets). Then one
header per line, as it should be sent. The host writes `Host`,
`Connection` (`keep-alive`), `Content-Length` or `Transfer-Encoding`, and with
`WF_DECODE` `Accept-Encoding`; the Amiga's own copies of those are dropped.
It adds `User-Agent: OpenWeb-Fetch/1` (native: `OpenWeb-Fetch/2`) when there is none. Everything else
goes as it is: the Amiga keeps its own cookie jar and sends `Cookie`
itself.

On the native transport, two more things in the request text:

- Credentials for HTTP auth go in the URL, `https://user:password@host/`
  (percent-encoded as needed). They are sent only to answer a 401 (below).
- A line `:proxy URL` (`http://[user:password@]host[:port]`, port 1080 when
  none is given, as curl has it; `:proxy none` for none) names the proxy
  for this fetch, in place of the environment's. Lines starting with `:`
  are never sent.

### The head

A 32-byte block of big-endian u32s, then two texts:

| Offset | Field |
| --- | --- |
| +0 | `'OWF1'` |
| +4 | error: 0, or why the fetch failed (below) |
| +8 | the HTTP status (0 on an error) |
| +12 | flags: bit 0 TLS, bit 1 the certificate was checked and trusted, bit 2 the body was decoded, bit 3 the length is known, bit 4 there is no body (HEAD, 204, 304) |
| +16 | the body's length, when bit 3 |
| +20 | the bytes of header text that follow |
| +24 | the bytes of info text after the header text |
| +28 | the HTTP version times ten (10, 11, or 20 for HTTP/2) |

The header text is the status line, then each header as the server sent it
(`Name: value`), each line ending in LF, in order, repeats kept: every
`Set-Cookie` comes back to the Amiga, and the host keeps none of them.
`Transfer-Encoding` is dropped, because the body comes de-chunked, and so
are `Content-Encoding` and `Content-Length` when the body was decoded.

The info text is `key=value` lines. After a TLS handshake:

```
tls=TLSv1.3
cipher=TLS_AES_256_GCM_SHA384
subject=CN=example.com
issuer=CN=DigiCert Global G3 TLS ECC SHA384 2020 CA1,O=DigiCert Inc,C=US
expires=Jan 15 23:59:59 2027 GMT
verified=yes
```

A fetch that went on a kept connection (below) ends its info text with
`reused=yes`; over HTTPS the lines before it are those of the connection's
own handshake.

The native transport's info text starts with `transport=native`, and adds
`alpn=h2` for HTTP/2, `proxy=host:port` through a proxy, `auth=server`,
`auth=proxy` or `auth=both` when a challenge was answered, and with
`WF_DECODE` the datatypes that undid the body (`decode=gzip.datatype`) or
`decode=datatypes` when it was left coded (below). Over HTTP/2 the status line is `HTTP/2 200`
and header names come as the server sent them, in lower case.

On an error, `error=` says what went wrong, and for a certificate the
host does not trust, the summary of the certificate the server sent:

```
error=self-signed certificate
subject=CN=localhost
issuer=CN=localhost
expires=Oct 13 10:00:00 2026 GMT
```

When buf0 is too small, HEAD answers -4 with the bytes it needs in result;
the head stays, so ask again with more room.

### Errors

Errors are apart from HTTP statuses: a 404 is a response, with error 0.

| Error | Name | Meaning |
| --- | --- | --- |
| 1 | DNS | The name did not resolve |
| 2 | CONNECT | No connection: refused, or no route |
| 3 | TLS | The handshake failed, or the certificate is not trusted (info says why) |
| 4 | TIMEOUT | No answer within the timeout |
| 5 | PROTOCOL | The answer was not HTTP/1.x, or its head was over 64 KB |
| 6 | CANCELLED | CANCEL or CLOSE stopped it |
| 7 | CLOSED | The connection closed before the body ended, or during the upload |
| 8 | DECODE | The gzip, deflate or Brotli data was bad |
| 9 | HOST | No memory or no thread for it |

Statuses: 0; -2 for a request the service cannot use (a bad request text,
an unknown handle or op); -4 above; -5 when 64 fetches are open already.

## What it does, against section 12

- **Request:** method, absolute URL and headers in OPEN; the body in OPEN
  and SEND, streamed.
- **Response:** status and headers in HEAD; the body in READ, chunk by
  chunk into the Amiga's buffers. `Set-Cookie` comes back, and is not kept.
- **Errors apart:** HEAD's error field, with the words in the info text;
  TLS says what was wrong with the certificate.
- **Redirects** are never followed: a 3xx comes back with its `Location`,
  for the caller to follow.
- **gzip, deflate and Brotli** are undone when the Amiga asks
  (`WF_DECODE`), with `Content-Encoding` dropped and the length fixed as
  above.
- **Many at once:** each fetch has its own handle and thread; completions
  come through the card's rings, so the Amiga waits on a signal bit.
  CANCEL ends one fetch.
- **TLS** is checked against the trust store of the system the service
  runs on (OpenSSL's default paths), with the name checked against the
  certificate, TLS 1.2 or later; a short summary of the certificate comes
  back in every HTTPS head.
- **Connections kept** per scheme, host and port (below), so a page's
  many small fetches to one host skip the connect and the TLS handshake.

## Answering later on the services card

The card runs a library call on one of its four workers. A HEAD or READ
with a wait would hold that worker for up to 2 s, so the library has a
second entry for the card, `wf_start` (`host/openweb_fetch.h`), with
`wf_cancel` beside it (AmigaChrome's `services.c` and `SERVICES_CARD.md`,
"Held requests"):

- `wf_start` is `wf_call` with the card's id for the request and the card's
  completion, `done(id, status, result, aux)`. Every op but HEAD and READ,
  and a HEAD or READ with a wait of 0, or with its answer there already,
  is answered at once, as `wf_call` answers it.
- A HEAD or READ that would wait returns `WF_LATER` (1) and is held on its
  fetch: the library keeps the id and buf0's room (the card's copy, its to
  write until it answers). It is answered, once, by whichever thread
  changes what it waits for: the fetch's own thread when the head or body
  bytes come or the fetch ends, a CANCEL or CLOSE of the fetch (error 6),
  or the library's timer thread when the wait runs out ("not yet": result
  0 and aux 0, as `wf_call` answers then). Calls held on one fetch are
  answered in the order they came.
- `wf_cancel(id)`: the guest cancelled the request (`AbortIO`), or the card
  reset. A held call is answered at once with -3, which the card drops;
  an id the library does not hold is ignored.
- The answer is the same as `wf_call` would have given: the same messages,
  the same waits. The Amiga's side does not change.

So no worker waits on a fetch: a page can keep a READ waiting on each of
its fetches, and the card's workers stay free for other calls. A card
without `wf_start`, such as one built before it, runs every call through
`wf_call` as before, and this library still keeps connections there.

## Connections kept

After an answer framed by its `Content-Length` or its chunks, read to its
end, with neither side asking to close (HTTP/1.1, or HTTP/1.0 with
`Connection: keep-alive`), the connection goes to a pool, and the next
fetch to the same scheme, host and port takes it instead of connecting:

- At most 6 idle connections for one scheme, host and port, and 32 in all;
  over that the oldest is closed.
- One is closed after 30 s idle, or a second before the server's
  `Keep-Alive: timeout=` if that is shorter. The library's timer thread
  closes them.
- A kept connection is checked before use (nothing waiting on it to read,
  or for TLS, only records with no data in them); one the server closed
  meanwhile is dropped and the next taken, or a new one made.
- If a kept connection fails before a byte of the answer has come (the
  server closed it as the request went out), the request goes again, once,
  on a new connection. One that fails after that is the fetch's error, as
  on a new connection.
- A request with a body never takes a kept connection, since the body
  could not be sent again; its connection is kept after it like any other.
- Not kept: a close-delimited answer, one cut off, one with
  `Connection: close`, one whose fetch was cancelled or closed before its
  body was all read, a failed fetch, and any connection with bytes left
  over after the answer.
- A fetch on a kept connection gives the connection's own TLS lines and
  `reused=yes` in its info text.
- `OPENWEB_FETCH_KEEP=0` in the environment turns keeping off: every fetch
  connects, sends `Connection: close`, and closes, as in v1.

A server that sends its head and its body in two small writes without
`TCP_NODELAY` stalls each answer on a kept connection by the client's
delayed acknowledgement (about 40 ms on Linux). Servers in use set
`TCP_NODELAY` (nginx does on kept connections), and the tests' servers do;
the library does not work round it.

## Limits

- 64 fetches open at once.
- The request text up to 64 KB; a response head up to 64 KB.
- The host reads at most 1 MB ahead of the Amiga for each fetch, and stops
  reading the connection while that is full; it queues at most 1 MB of the
  upload.
- A wait is at most 2 s. The timeout (default 30 s, at most 300 s) covers
  connecting, and then each wait for the server: the handshake, the head,
  each pause in the body, and each pause in the upload from the Amiga. A
  name that takes long to resolve is not cut short by it.
- A fetch the Amiga has not touched for 10 minutes (a program that ended
  without CLOSE, a reset) is closed by the host when the next fetch opens.
- The card's buffer rules apply: at most 16 MB written per request, and
  64 MB for all requests in flight, held ones too: a READ's buffer is held
  for its wait, so many large READs waiting at once can make the card
  answer -5 (busy) until some come back.
- On a card with `wf_start`, a waiting HEAD or READ holds no worker. A
  SEND whose upload queue is full still waits on a worker, for at most its
  wait. On a card without `wf_start`, every wait holds one of its four
  workers: keep waits short there (250 to 1000 ms), one waiting call per
  fetch.
- A card CANCEL (AbortIO) answers a waiting call at once and lets the
  library drop it; CANCEL or CLOSE the fetch to stop its connection too.
- Kept connections: 6 for one scheme, host and port, 32 in all, 30 s idle.

## Why C and OpenSSL

The library runs inside the emulator's process, on the card's workers. A
fetch needs a thread that can block on its connection and be stopped from
another thread. In C each fetch owns its socket, and CANCEL shuts it, which
ends any read at once. OpenSSL 3 and zlib are already on every system that
builds `libopentlskey.so` and `libmediadecode.so`, and their system trust
store is the one the rest of the system uses. A Python helper would be one
more process to start, watch and stop from inside the emulator.

## The client on the Amiga

`src/webfetch.c` holds the calls, so any program can link it: open the
service, start a fetch, read its head (parsed into status, flags, headers
and info), read and send body parts, cancel, close. Each call is one
request through openservice.device. `OWFetch` is the command built on it:

```
OWFetch URL [TO file] [HEAD] [HEADER "Name: value" ...] [METHOD m] [FROM file]
        [TIMEOUT s] [RAW] [QUIET] [INFO] [NATIVE]
```

It prints the status line and the headers (the TLS summary too with
`INFO`), and writes the body to `TO`, or to the console. `HEADER` takes
one or more `"Name: value"` strings. `HEAD` asks for the head only, `FROM`
sends a file as the body (POST unless `METHOD` says otherwise), `RAW` keeps
the body as the server sent it (otherwise it is decoded: natively by the
datatypes, see "Compressed bodies"), `QUIET` prints nothing but errors, and
`NATIVE` uses this Amiga's own TCP/IP and OpenTLS even with a card there
(`INFO` says which is used). Ctrl-C cancels. It returns 0 for an answer below 400, 5 for 400 and above,
10 when the fetch failed (DNS, TLS, ...), and 20 when it could not start.

## The native transport

When no services card or paired Cradle offers `openweb.fetch/1` (or the
program asks with `wf_service_open_mode(s, WF_MODE_NATIVE)`, or
`ENV:OpenWeb/Fetch` is `native`, or `OWFetch ... NATIVE`), `src/webfetch.c`
answers every call itself: the same ops, buffers, heads and errors, over
the Amiga's own bsdsocket.library (any stack) and opentls.library. Its
`where` is `WF_WHERE_NATIVE`. No curl, nghttp2 or libpsl: the protocol code
is our own, written from the RFCs.

- `src/wfnet.c`: fetches and connections, HTTP/1.1, proxies, auth,
  WebSocket; `src/wfnet_h2.c`: HTTP/2 (RFC 9113); `src/wfnet_hpack.c`:
  HPACK (RFC 7541) with the static table, a dynamic table each way and the
  Huffman code; `src/wfnet_auth.c`: MD5, SHA-1, SHA-256, base64, the
  WebSocket accept key and the Basic and Digest answers; `src/wfnet_dec.c`:
  compressed bodies through the datatypes.
- Portable C99 with no writable globals and nothing of the C library but
  `<string.h>`: the machine is a table of calls, `struct wfn_io`
  (`include/openweb/wfnet.h`): connect, send, receive, close, wait, TLS
  over a socket, time, memory, random bytes, the environment, and the
  decoders. `src/wfnet_amiga.c` fills it with bsdsocket.library and
  opentls.library (their bases in a struct of their own, never in
  globals); `host/wfnet_posix.c` with POSIX sockets and OpenSSL, for the
  tests on Linux.
- Integers only. 68k code (the 68040, GCC 16.2, `-Os`): the protocol code
  about 36.0 KB (`wfnet.c` 19.0, `wfnet_h2.c` 6.0, `wfnet_auth.c` 5.6,
  `wfnet_hpack.c` 4.2, `wfnet_dec.c` 1.2), the Amiga side 3.3 KB,
  `webfetch.c` 3.7 KB; no data or bss.
- Memory: each fetch has its window (64 KB) and about 2 KB; each
  connection about 18 KB, and OpenTLS's 33 KB with TLS. An HTTP/2 header
  block is decoded into 64 KB that is freed straight after.
- Nothing runs in the background: each call moves every fetch of its
  `wf_service` on, then waits on all their sockets for at most its wait.

### HTTP/1.1

Kept connections as above: 6 for one place, 32 idle in all, 30 s idle
(less when the server's `Keep-Alive: timeout=` says so), checked before
use; a kept connection that fails before the answer begins sends the
request again on a new one (twice at most), when it has no body or the
whole body came with OPEN, and the method is one that may be repeated
(GET, HEAD, OPTIONS, TRACE, PUT, DELETE). Bodies framed by `Content-Length`, chunked
(extensions and trailers taken and dropped) or by the connection's end;
uploads with a length or chunked. `100 Continue` and other 1xx are
skipped. Redirects are left to the caller, as before.

### HTTP/2

Over `https://` the transport offers `h2,http/1.1` by ALPN
(`OT_SetALPN`, and `OT_GetALPN` for the answer); when the server takes
`h2`, the connection is HTTP/2 and every fetch to the same place is a
stream on it, up to the server's `SETTINGS_MAX_CONCURRENT_STREAMS`. While
the first connection's handshake runs, other fetches to the same place
wait for it, so a page's fetches share one connection.

- SETTINGS: push off, each stream's window the fetch's window; the
  connection's window 16 times that, given back as data comes. A stream's
  window is given back as the program reads, so the server never sends a
  fetch more than it can hold. Our DATA keeps to the server's windows.
- Our headers are HPACK literals never indexed (the static table's names,
  Huffman where shorter); the server's are decoded with a 4 KB dynamic
  table. Connection headers (`Connection`, `Keep-Alive`, `Upgrade`,
  `Transfer-Encoding`, `Host`) are not sent; `:authority` is the host.
- PING answered; GOAWAY: streams the server did not take go again on a new
  connection (when they can be sent again), the rest end with error 7;
  RST_STREAM ends that stream (error 7), REFUSED_STREAM sends it again;
  CANCEL and CLOSE send RST_STREAM CANCEL and the connection stays.
- A `content-length` is checked against the DATA: a stream that ends short
  fails with error 7, one that sends more fails with error 5. A connection
  that fails sends its streams again only for the methods HTTP/1.1 would.
- Trailers are read and dropped. `http://` is never HTTP/2 (no h2c), and
  WebSocket is always HTTP/1.1.

### WebSocket

WS_OPEN sends the Upgrade (`Sec-WebSocket-Key` from 16 random bytes,
version 13) on a connection of its own, and checks `Sec-WebSocket-Accept`
(SHA-1 and base64): a wrong one is error 5. After the 101, WS_SEND sends
masked frames (a fresh mask for each), and WS_RECV gives messages in the
order they came, in parts as large as the room given (bit 4 on the last).
A message larger than the window comes in parts too. Pings are answered
with pongs at once (and not shown); pongs are shown. A close from the
server is shown with its code and answered; after ours, the server's
answer is shown, and then the end (bit 5, error 0). An end without a close
is error 7.

### Proxies

From the request's `:proxy` line, else the environment: `http_proxy` (or
`HTTP_PROXY`, then `all_proxy`) for `http://` and `ws://`, `https_proxy`
(or `HTTPS_PROXY`, then `all_proxy`) for `https://` and `wss://`, read with
`GetVar()` (so `ENV:http_proxy`), with `no_proxy` (names and domains by
commas, a leading dot or not, or `*`). `WF_NOPROXY` turns them off. Only
HTTP proxies:

- `http://` goes to the proxy in absolute form (`GET http://host/path`),
  with `Proxy-Authorization` once asked; one kept connection to the proxy
  serves every host.
- `https://`, `ws://` and `wss://` go through `CONNECT host:port`, then
  TLS (and HTTP/2) inside the tunnel, which is kept for that host only.
- A 407 is answered once when the proxy URL carries credentials;
  otherwise it comes back as the answer, with its body.

### HTTP auth

With credentials in the URL, a 401 is answered once: with Digest (RFC
7616: `SHA-256` or `MD5`, with or without `-sess`, `qop=auth`, `userhash`)
when the server offers it, the strongest first, else Basic; then the
request goes again (on the same connection when it was kept). Its 401 body
is read and dropped. A request whose body came in parts with SEND cannot
be sent again: its 401 comes back. Credentials are never sent before a
challenge. A second 401 comes back as the answer.

### Compressed bodies

The transport has no compression code of its own: decoding is the
datatypes' work, stream.datatype with gzip.datatype (`gzip`, and
`deflate` zlib-wrapped or raw, told apart by its first two bytes) and
brotli.datatype (`br`). Without `WF_DECODE` the body comes as the server
sent it, `Content-Encoding` kept, and no `Accept-Encoding` is added.

With `WF_DECODE`:

- `Accept-Encoding` offers only what can be undone now: `gzip, deflate,
  br` with both datatypes, `gzip, deflate` or `br` with one, and
  `identity` with neither (or no datatypes.library). The first such fetch
  looks for them, by making and disposing an object of each, once per
  `wf_service`. The caller's own `Accept-Encoding` is dropped, as on the
  card.
- `src/wfnet_dec.c` reads `Content-Encoding` and makes a streaming object
  of each coding's datatype (`NewDTObject()` with `DTST_RAM`,
  `DTA_BaseName` `gzip` or `brotli`, `SDTA_Wrapper` `SDTW_GZIP` for gzip
  and x-gzip, `SDTW_AUTO` for deflate). It feeds the body in with
  `SDTM_FEED` as it comes, takes the plain bytes with `SDTM_TAKE`, and
  sends `SDTM_END` when the body ends. Coded bytes are fed only when the
  plain side wants more, so about one piece of the body waits in it.
- Several codings (`Content-Encoding: br, gzip`) are chained: up to
  three, the last applied undone first, an object each.
- The head drops `Content-Encoding` and `Content-Length` and sets bit 2,
  as the card does, and the info text names the datatypes that did it:
  `decode=gzip.datatype`, or `decode=gzip.datatype, brotli.datatype`.
- Damaged or cut-short data fails the fetch with error 8 (DECODE). A body
  of no bytes (HEAD, 304) is no error.
- A coding with no datatype for it (`compress`, `zstd`, more than three
  codings, or a datatype that is not installed) leaves the body as the
  server sent it: `Content-Encoding` kept, bit 2 clear, and the info text
  says `decode=datatypes`, for the program to decode it some other way.

datatypes.library is opened the first time it is wanted and kept until
the `wf_service` closes; when it is missing it is not looked for again.
`src/wfnet_dec.c` is portable C: the machine gives it `NewDTObject()`, the
three methods and `DisposeDTObject()` (`src/wfnet_amiga.c`), and the host
test gives it opendatatypes' own portable core instead (`host/wfnet_dt.c`,
below). `host/wfnet_posix.c` alone plugs zlib into the hook.

What an Amiga needs, an OpenBrowser build too: stream.datatype,
gzip.datatype and brotli.datatype in `SYS:Classes/DataTypes`, and
z.library (OpenZlib) in `LIBS:` for gzip.datatype. Without gzip.datatype
or z.library, gzip and deflate are not asked for; without brotli.datatype,
br is not. Whether OS 3.2's own datatypes.library takes stream.datatype's
new group (`GID_DATA`) is still to be checked on the bench.

### TLS through OpenTLS

opentls.library 1.x: TLS 1.2 and 1.3, the certificate checked against
OpenTLS's trust store and the host's name (`OTV_FULL`), so every
handshake that succeeds sets bit 1. The info text gives `tls=`, `cipher=`,
`verified=yes` and `subject=` (OpenTLS's `OT_GetPeerName`); on a failure
`error=` with `OT_GetErrorText`. ALPN is OpenTLS's (`OT_SetALPN`,
`OT_GetALPN`), so nothing was missing for HTTP/2. What OpenTLS could still
give: SHA-1 and MD5 through opencrypto.library (wfnet carries its own,
5.6 KB with SHA-256 and the auth code, used for a few hashes per request),
the issuer and expiry of the server's certificate for the info text, and
saying whether a non-blocking `OT_Handshake` waits to read or to write
(wfnet waits to read).

## One task, many fetches

A program such as the browser's network backend runs all its fetches from
one task, and must never block on one (include/openweb/webfetch.h):

```
wf_open(&s, ..., &f1); wf_open(&s, ..., &f2);         start them (each answers at once)
wf_async_start(&s, &a1, WF_OP_HEAD, f1, buf1, room, 2000);
wf_async_start(&s, &a2, WF_OP_READ, f2, buf2, room, 2000);
for (;;) {
    got = wf_wait(&s, 100, windowSignals);            sleeps on the network and the task's own signals
    if (wf_async_check(&a1)) ...                      a1.status, a1.result, a1.aux, as wf_head/wf_read answer
    if (wf_async_check(&a2)) ...                      then start the next one
}
```

- On the card (or a Cradle), `wf_async_start` sends the call with
  `SendIO()` on a request of its own that replies to the service's port;
  the card holds it until it has something (`wf_start`, above) or its wait
  runs out, so nothing on the Amiga waits. `wf_wait` waits on that port's
  signal and the program's own; `wf_async_check` is `CheckIO()`, and
  `wf_async_abort` is `AbortIO()`.
- On the native transport, `wf_async_start` asks once with a wait of 0,
  `wf_wait` moves every fetch on and waits on all their sockets with
  `WaitSelect()`, its timeout and the program's signals, and
  `wf_async_check` asks again with a wait of 0.
- Plain calls with a wait of 0 never block either (the names are the one
  exception: `gethostbyname()` blocks while it resolves, on the native
  transport). OPEN, SEND, CANCEL and CLOSE answer at once.
- The only synchronous parts left are the plain calls, which are
  `DoIO()` on the card and wait at most their wait.

## Left for later

- **The WebSocket ops and the `:proxy` line on the card's host library**,
  and HTTP/2 there: the native transport has them, the card answers -2.
- **TLS session resumption** for new connections to a host already seen
  (OpenTLS has `OT_GetSession`); HTTPS proxies and SOCKS; IPv6 in the
  Amiga side (bsdsocket's `gethostbyname` is IPv4); the name resolved
  without blocking.
- **SEND answered later** too, when the upload queue is full, as HEAD and
  READ are now.
- **The LAN transport for real Amigas.** The device already carries the
  same entries to a paired Cradle. Two things are needed first: the
  Cradle's host (`cradle/nursery_host.py`) offering this library, and the
  device keeping every call on a fetch's handle to the one Cradle that
  opened it (today a service open on several Cradles spreads calls to the
  least busy one, which suits stateless services only). A long wait also
  holds the Cradle's connection, so the LAN path wants short waits.
- **Client certificates.**
- **The PiStorm back end** (below).

### The PiStorm back end (design only)

The appliance's PiStorm plan (amigachrome `appliance/pistorm/README.md`)
runs Linux on the Pi with the AmigaChrome runtime on its ARM64 cores, and
the A1200's own bus for its keyboard, mouse and floppy. There the services
card is the same card in the same runtime, so this service needs no change:

- `libopenwebfetch.so` built for ARM64 from the same source; it needs only
  the C library, OpenSSL 3, zlib and libbrotlidec, which Pi OS has.
- The Pi's own Ethernet or Wi-Fi carries the traffic. The Amiga needs no
  network card and no TCP/IP stack for it.
- The trust store is Pi OS's `ca-certificates`. The Pi has no clock that
  keeps time with the power off, so certificates fail as "not yet valid"
  until the clock is set: the appliance should wait for NTP before it
  offers the service, or HEAD will report TLS errors that are not the
  server's fault.
- Memory: 64 fetches with 1 MB windows is 64 MB at most; a Pi with 1 or
  2 GB may want a smaller window or fewer fetches, as a build setting.
- With Emu68 instead of the appliance, there is no Linux on the Pi, so
  no sockets or TLS beside the 68k. Such an Amiga reaches the service on a
  Cradle over the LAN, which waits on the LAN transport above.

## Tests

`host/test_openweb_fetch.py` runs the library against servers it starts
on 127.0.0.1: plain HTTP (headers, `Set-Cookie`, a redirect not followed,
chunked, close-delimited, gzip, raw deflate and Brotli with and without `WF_DECODE`, several MB
checked by MD5, uploads with a length and chunked, HEAD), errors (DNS,
connection refused, timeout, cancel, closed early), HTTPS with a test
certificate the test makes with `openssl` in a temporary directory (the
TLS error and its summary; the same server trusted through
`SSL_CERT_FILE` in a child process, and a name mismatch), eight fetches at
once, the limit of 64, and https://example.com when the network allows.
Answering later, through `wf_start` as the card calls it: a HEAD answered
when its head comes, a READ whose wait runs out ("not yet"), `wf_cancel`
of a held READ (-3, once), the fetch's CANCEL and CLOSE answering held
calls, and 40 HEADs held at once, each answered once. Kept connections: a
second fetch and 30 more on the same connection, a request with a body on
a new one, `Connection: close`, the server's `Keep-Alive: timeout=2` (the
host closes it after 1 s idle), a fetch closed before its body ended, 10
at once to one host (6 kept, 4 closed), a server that closed an idle
connection, one that closes it as the next request comes (sent again on a
new one), one that closes it in the middle of an answer (error 7, not sent
again), and a page of 50 small fetches over HTTP and HTTPS with and
without `OPENWEB_FETCH_KEEP=0`. The test passes with the library built
with AddressSanitizer and UBSan, and with ThreadSanitizer.

`host/test_wfnet.py` runs the native transport's own code, built for
Linux as `host/libwfnet.so` (POSIX sockets, and OpenSSL and zlib in place
of OpenTLS and the datatypes), against servers it starts on 127.0.0.1:
first the pieces against their published examples (RFC 7541 Appendix C
C.2 to C.6, decoded and encoded byte for byte, with and without Huffman
and with eviction; Huffman round trips; MD5, SHA-1 and SHA-256 against
hashlib and RFC 1321 and FIPS 180; base64 from RFC 4648; the accept key
from RFC 6455 1.3; Digest from RFC 7616 3.9.1, MD5 and SHA-256); then
HTTP/1.1 (kept connections, chunked, close-delimited, 3 MB, 10 at once,
uploads with a length and chunked, DNS, refused, timeout, cancel,
Content-Encoding kept and through the hook, one task's 8 fetches with
waits of 0 and `wfn_pump`); HTTPS; HTTP/2 against a server written in the
test by hand (frames, SETTINGS, flow control both ways, HPACK both ways,
12 streams at once on one connection, 3 MB through a 64 KB window, uploads,
RST_STREAM, cancel, GOAWAY with the untaken streams sent again, HTTP/1.1
by flag) and, when Go is there, against Go's net/http HTTP/2 server;
WebSocket echo (ping and pong both ways, parts, large messages, close from
either side, a wrong accept key); a proxy (absolute form, CONNECT for
HTTPS, HTTP/2 and WebSocket, the environment and `no_proxy`, a 407 with
Basic); Basic and Digest auth (MD5, SHA-256, a POST sent again);
compressed bodies through `src/wfnet_dec.c` over opendatatypes' portable
stream core (`ODT`, the checkout, default beside this one; skipped
without it): gzip, x-gzip, gzip members, zlib and raw deflate, Brotli
(with RFC 7932's dictionary, `BROTLI_RFC`), two codings in a row, fed and
taken a byte at a time, damaged and cut short, HEAD and empty bodies,
`Accept-Encoding` with all, some and none of the datatypes, and 3 MB
through a 16 KB window over HTTP/1.1 and HTTP/2, every object disposed;
and the portable files built as C99 with `-Wall -Wextra -Werror -Os`,
with no writable data. It passes with AddressSanitizer and UBSan too,
with no leaks of its own.

Measured on 12 October 2026 with `OWFetch` on a scratch copy of an OS 3.2.3
instance (AC090, the card's runtime built with this service): 20 fetches of
an 8 MB body from a server on the same machine, written to `NIL:`, in
about 1.5 s, so about 100 MB/s into the Amiga's memory; the bodies written
to a file had the server's MD5.

Measured later the same day on the same lab, with the card that holds
calls (AmigaChrome's `services/async-complete-20261012`), servers on the
same machine, the time from the first request to the last as the server
saw them, for a page of 50 small fetches (2 KB each) one after another:

| | v1 (a connection each) | kept |
| --- | --- | --- |
| HTTP | 222 ms, 50 connections | 138 ms, 1 connection |
| HTTPS | 429 ms, 50 connections | 141 ms, 1 connection |
| HTTP, 30 ms more per new connection | 1670 ms | 123 ms |
| HTTPS, 30 ms more per new connection | 1723 ms | 175 ms |

With 12 READs waiting on a stalled server, one small fetch took 4.0 to
6.0 s before (its calls queued behind the waits on the card's four
workers) and 1.0 to 5.2 ms after; with 24 waiting, 1500 small fetches
took 2.2 s, all on one connection, while the waiting READs came back as
their 2 s waits ran out; `AbortIO` on 40 waiting READs had them all back
in 0.8 ms.
