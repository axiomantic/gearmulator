# Task TOOL-18. The GDB-with-traffic harness's client side.
#
# A minimal RSP client for the stub gdbStub.cpp serves. It exists because the
# stub's packet set is deliberately small -- g/G/m/M/Z0-z3/s/c/D plus the one
# qSupported capability -- and lldb's gdb-remote client requires a negotiation
# that stub does not answer. Measured 2026-08-28, recorded in plan section
# 24.6 row W3-454: a hand-rolled client over the loopback socket is what a
# session on this stub uses.
#
# THE PACKET LAYER IS THE PROTOCOL AND NOTHING HERE INVENTS A DIALECT: one
# request is `$<payload>#<checksum>`, the checksum is the payload's bytes
# summed modulo 256, and each side answers `+` for a packet read and `-` for
# one whose checksum failed. Detach (`D`) ends the session; the stub answers
# OK and the console process exits, which is why run_script's teardown sends
# it rather than closing the socket.

import argparse
import re
import socket
import sys
import time

# The stub answers every packet, so a reply that never arrives means the peer
# went away. A timeout turns that into an error instead of a hang.
REPLY_TIMEOUT = 60.0

STOP_RE = re.compile(
    r"^T(?P<signal>[0-9a-fA-F]{2})"
    r"(?:(?P<kind>watch|rwatch|awatch):(?P<addr>[0-9a-fA-F]+);)?"
)


def _checksum(payload):
    return sum(payload.encode()) & 0xFF


class ScriptError(Exception):
    pass


class Client:
    def __init__(self, sock):
        self._sock = sock
        self._rxbuf = b""

    # ------------------------------------------------------------- framing

    def _read_more(self):
        chunk = self._sock.recv(4096)
        if not chunk:
            raise ScriptError("the stub closed the connection")
        self._rxbuf += chunk

    def _read_exact(self, count):
        while len(self._rxbuf) < count:
            self._read_more()
        out = self._rxbuf[:count]
        self._rxbuf = self._rxbuf[count:]
        return out

    def _read_until(self, marker):
        while marker not in self._rxbuf:
            self._read_more()
        head, _, tail = self._rxbuf.partition(marker)
        self._rxbuf = tail
        return head

    def exchange(self, payload):
        framed = "$" + payload + "#%02x" % _checksum(payload)
        self._sock.sendall(framed.encode())

        ack = self._read_exact(1)
        if ack != b"+":
            raise ScriptError("the stub answered %r instead of acknowledging" % ack)

        body = self._read_until(b"#")
        given = self._read_exact(2)
        if int(given, 16) != _checksum(body.decode()):
            self._sock.sendall(b"-")
            raise ScriptError("the stub's reply checksum did not match")

        self._sock.sendall(b"+")
        return body.decode()

    # ------------------------------------------------------------ commands

    def connect(self, payload):
        # qSupported first: it is the one capability packet the stub answers,
        # and a client that skipped it could not tell a dead socket from an
        # unsupported negotiation.
        reply = self.exchange(payload)
        if not reply.startswith("PacketSize="):
            raise ScriptError("qSupported answered %r" % reply)
        return reply

    def read_registers(self):
        reply = self.exchange("g")
        if len(reply) != 18 * 8:
            raise ScriptError("g answered %d hex digits" % len(reply))
        values = [int(reply[i * 8:(i + 1) * 8], 16) for i in range(18)]
        return values

    def read_pc(self):
        return self.read_registers()[17]

    def read_mem(self, address, length):
        reply = self.exchange("m%x,%x" % (address, length))
        if reply == "E01":
            raise ScriptError("m at 0x%x was refused" % address)
        return bytes.fromhex(reply)

    def write_mem(self, address, data):
        reply = self.exchange("M%x,%x:%s" % (address, len(data), data.hex()))
        if reply != "OK":
            raise ScriptError("M at 0x%x answered %r" % (address, reply))

    def _point(self, insert, kind, address, length):
        letter = "Z" if insert else "z"
        reply = self.exchange("%s%s,%x,%x" % (letter, kind, address, length))
        if reply != "OK":
            raise ScriptError("%s%s at 0x%x answered %r" % (letter, kind, address, reply))

    def zbreak(self, address, on):
        self._point(on, "0", address, 2)

    def zwatch(self, address, on):
        self._point(on, "2", address, 4)

    # The stop reply, parsed. `T05` alone is a plain stop; a watch stop NAMES
    # the address, and that name is what "who wrote this" is answered from.
    def parse_stop(self, reply):
        match = STOP_RE.match(reply)
        if match is None:
            raise ScriptError("the stop reply %r is not a stop" % reply)
        kind = match.group("kind") or "bp"
        addr = match.group("addr")
        return {"kind": kind, "address": int(addr, 16) if addr else None}

    def continue_until_stop(self, timeout=None):
        saved = self._sock.gettimeout()
        if timeout is not None:
            self._sock.settimeout(timeout)
        try:
            reply = self.exchange("c")
        except socket.timeout:
            raise ScriptError("the continue produced no stop within %s s" % timeout)
        finally:
            self._sock.settimeout(saved)
        return self.parse_stop(reply)

    def detach(self):
        # The stub exits the console on this packet; the reply may arrive
        # before the process is gone, so a refusal here is an error and a
        # broken pipe after it is the session ending.
        reply = self.exchange("D")
        if reply != "OK":
            raise ScriptError("D answered %r" % reply)

    # The batch mode, on the client for a caller that wants one call: arm and
    # continue per step, one stop line per step. ScriptRunner is the fuller
    # form; this one prints to stdout with the same shape.
    def run_script(self, steps, timeout=None):
        ScriptRunner(self).run(steps, timeout=timeout)


def connect(host, port):
    sock = socket.create_connection((host, port), timeout=REPLY_TIMEOUT)
    return Client(sock)


class ScriptRunner:
    def __init__(self, client, out=sys.stdout):
        self._client = client
        self._out = out
        self._counts = {}

    # One line per stop, named so a later reader can tell a hit from a miss
    # without re-deriving which breakpoint was where. A miss is REPORTED as a
    # miss: a stop that names no armed point prints hit=none.
    def report(self, stop):
        self._counts[stop["kind"]] = self._counts.get(stop["kind"], 0) + 1
        count = self._counts[stop["kind"]]
        addr = stop["address"]
        print("stop pc=0x%08x hit=%s@%s count=%d" % (
            stop["pc"], stop["kind"],
            "0x%08x" % addr if addr is not None else "none", count),
            flush=True)

    def run(self, steps, timeout=None):
        for kind, address, actions in steps:
            if kind == "break":
                self._client.zbreak(address, True)
            elif kind == "watch":
                self._client.zwatch(address, True)
            else:
                raise ScriptError("unknown step kind %r" % kind)
            stop = self._client.continue_until_stop(timeout)
            stop["pc"] = self._client.read_pc()
            self.report(stop)
            for action in actions:
                action(self._client)


def parse_script(path):
    """Read one step per line: `break 0x...` or `watch 0x...`.

    A step may carry a trailing `mem 0xADDR=0xVAL` action, which writes one
    byte through M before the next continue.
    """
    steps = []
    with open(path) as handle:
        for number, raw in enumerate(handle, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            actions = []
            for field in fields[2:]:
                action = re.match(r"^mem(0x[0-9a-fA-F]+)=(0x[0-9a-fA-F]+)$", field)
                if action is None:
                    raise ScriptError("line %d: bad action %r" % (number, field))
                address = int(action.group(1), 16)
                value = int(action.group(2), 16)
                actions.append(lambda c, a=address, v=value: c.write_mem(a, bytes([v])))
            if fields[0] not in ("break", "watch"):
                raise ScriptError("line %d: kind must be break or watch" % number)
            steps.append((fields[0], int(fields[1], 16), actions))
    return steps


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Run a scripted session against the g2 GDB stub.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--script", required=True)
    parser.add_argument("--timeout", type=float, default=None)
    args = parser.parse_args(argv)

    client = connect(args.host, args.port)
    client.connect("qSupported:multiprocess+")
    for step in parse_script(args.script):
        kind, address, _ = step
        print("arm %s 0x%08x" % (kind, address), flush=True)

    ScriptRunner(client).run(parse_script(args.script), timeout=args.timeout)
    client.detach()
    return 0


if __name__ == "__main__":
    sys.exit(main())
