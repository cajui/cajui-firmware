# ADR 0003: The receiver searches its network for the broker

- Status: Accepted
- Date: 2026-11-13

## Context

A receiver stores the broker's host and connects to it. Whoever sets it up had to know
that host: the setup page listed only brokers announced through mDNS (`_mqtt._tcp`), and
a broker running in a container is not announced unless something on its computer does
it. A monitoring application in a container cannot tell the address of its own computer
on the home network either, so the person had to find it and type it. Afterwards, when
the router handed the broker's computer another address, forwarding stopped until
someone opened the setup page or connected a USB cable again.

## Decision

- The receiver can look for the broker itself: it tries a TCP connection to the broker
  port on every other address of its station's IPv4 network and lists those that accept.
  A network larger than 254 hosts is narrowed to the /24 block around the receiver, so
  trying the port takes about 35 seconds whatever the network's size. Wi-Fi power saving
  is off while the port is tried: on one home network it delayed answers to the receiver
  past the waiting window, and a broker was missed.
- **Setup page.** The broker section lists what the search found, after any announced
  broker, with the same "Use" link. The search sends no credentials; the person still
  enters the username and password.
- **In operation.** With the setup page closed, Wi-Fi connected to the stored network and
  the broker unreachable for two minutes, the receiver searches. Each address that
  answered, the stored host first if it is among them, gets an MQTT 3.1.1 CONNECT with a
  clean session, no will and its own client ID, first with the stored username and a
  random control password. A broker that accepts it checks no password and is skipped.
  Only a broker that refuses it is asked with the stored password. The first one that
  accepts replaces the stored host, is saved and takes over forwarding without a
  restart; when it is the stored host itself, the connection is restarted. While the
  broker stays unreachable the search repeats every five minutes.
- The range, the CONNECT packet, the reading of the answer, the control password, the
  decision on each candidate and the schedule are host code in `lib/CajuiUplink` under
  the coverage gate. The sockets live in
  `src/board/broker_scan.cpp` and run on hardware only.

## Consequences

- A broker restart or a short outage changes nothing: the stored host is retried every
  five seconds as before, and the first search starts only after two minutes.
- A stored host name is replaced by the address found. The next address change is
  handled by another search.
- A broker without authentication on the same network, such as Mosquitto with anonymous
  access, never takes over forwarding and never receives the stored password: it accepts
  the control password and is skipped.
- In operation the receiver still sends the stored password to any computer on the stored
  Wi-Fi network that answers on the broker port and refuses the control password, not
  only to the host the password was entered for. Something that imitates a broker on
  purpose gets the password and the forwarding, including the management channel. The
  uplink is already plain MQTT on a network that must be trusted, and someone on that
  network can read the password from the forwarding connection; the search makes it
  easier, since answering on a port is simpler than intercepting traffic. The receiver
  searches only while its station is on the stored network, and the setup page still
  never sends a saved password to a host the person typed. Broker TLS with a pinned
  identity would close this and is not implemented.
- Only the receiver's own network is searched. A broker behind another router or on
  another subnet is not found and still needs its address typed, as does a broker on
  another port when no broker is stored yet (the page searches the staged port, or 1883).
- At most six brokers are listed or asked per search.
- Trying the port opens up to six connections at a time for about 35 seconds;
  each address of the network sees one connection attempt per search, and each that
  answers sees up to two more for the CONNECT probes. On the setup page the mDNS query
  adds up to about eight seconds. In operation each probe waits up to four and a half
  seconds, so a search with six silent candidates can take about a minute and a half; a
  search that cannot start is tried again after ten seconds.

## Alternatives

- Announce the broker from the monitoring application's installation: keeps the address
  out of the receiver's hands, but a container cannot send the announcement on common
  desktop installations, so it needs a helper on the computer for each operating system.
- Store a `.local` host name: needs the broker's computer to answer mDNS and the router
  to pass multicast between Wi-Fi and cable, and still asks the person for the name.
- Have the installer write the address into the application's settings: correct on the
  day of installation only.
- Search without credentials in operation and move only when exactly one host answers:
  sends no password, but moves forwarding to whatever answers on the port and breaks
  when two brokers share the network.
- Reserve the address on the router: works and remains useful, but it is a step outside
  the system that many people will not take.
