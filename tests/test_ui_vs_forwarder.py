"""Every packet-lab scenario must get the verdict the UI promises from the real C parser."""
import json
import shutil
import subprocess

import pytest

from conftest import ROOT
from ctl_client import CtlClient

NODE = shutil.which("node")
SCRIPT = (
    "const P=require('./dashboard/static/packet.js');"
    "console.log(JSON.stringify(P.PRESETS.map(p=>({id:p.id,expect:p.expect,hex:P.toHex(P.build(p.opts).bytes)}))));"
)
ROUTES = "1000 2000 127.0.0.1:SINK\n1001 2001 127.0.0.1:SINK\n1002 2002 127.0.0.1:SINK\n"

pytestmark = pytest.mark.skipif(not NODE, reason="node not installed")


def presets():
    out = subprocess.run([NODE, "-e", SCRIPT], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return {p["id"]: p for p in json.loads(out)}


def test_default_mode_matches_every_promise_except_strict_only_scenario(launch):
    ps = presets()
    assert len(ps) >= 14
    c = CtlClient(*launch(routes=ROUTES).ctl_addr)
    for pid, p in ps.items():
        r = c.inspect(p["hex"])
        assert r["ok"], pid
        # Without -i the corrupted inner checksum is forwarded untouched; the UI says so in its blurb.
        expected = "forward" if pid == "badsum" else p["expect"]
        assert r["action"] == expected, f"{pid}: UI promises {expected}, forwarder says {r}"


def test_strict_mode_judges_the_inner_packet(launch):
    ps = presets()
    c = CtlClient(*launch(["-i"], routes=ROUTES).ctl_addr)
    assert c.inspect(ps["ipv4"]["hex"])["action"] == "forward"
    assert c.inspect(ps["ipv6"]["hex"])["action"] == "forward"
    assert c.inspect(ps["badsum"]["hex"])["reason"] == "bad_inner_ip"
    assert c.inspect(ps["valid"]["hex"])["reason"] == "bad_inner_ip"      # opaque bytes are not IP
    assert c.inspect(ps["echo"]["hex"])["action"] == "echo_response"      # control messages are exempt
