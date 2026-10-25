"""Tests of the dashboard HTTP API running against a real forwarder."""
import json
import threading
import time
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

import pytest

import server as dash
from ctl_client import CtlClient


@pytest.fixture()
def api(fwd_ctl):
    state = dash.State(CtlClient(*fwd_ctl.ctl_addr, timeout=0.5), fwd_ctl.data_addr)
    dash.Handler.state = state
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), dash.Handler)
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    state.sample(refresh_routes=True)
    base = f"http://127.0.0.1:{httpd.server_address[1]}"

    def call(method, path, body=None, raw=None):
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        req = urllib.request.Request(base + path, data=data, method=method)
        try:
            with urllib.request.urlopen(req, timeout=3) as r:
                return r.status, r.read(), r.headers
        except urllib.error.HTTPError as e:
            return e.code, e.read(), e.headers

    def jcall(*a, **k):
        status, body, _ = call(*a, **k)
        return status, json.loads(body)

    yield call, jcall, state, fwd_ctl
    state.stop()
    httpd.shutdown()
    httpd.server_close()


def test_index_and_assets_are_served(api):
    call, *_ = api
    status, body, headers = call("GET", "/")
    assert status == 200 and b"GTP-U Control Room" in body and "text/html" in headers["Content-Type"]
    assert call("GET", "/app.js")[0] == 200
    assert call("GET", "/img/logo.svg")[2]["Content-Type"].startswith("image/svg+xml")


def test_path_traversal_is_rejected(api):
    call, *_ = api
    for p in ["/../server.py", "/..%2fserver.py", "/%2e%2e/server.py", "/img/../../server.py", "/nope.txt"]:
        assert call("GET", p)[0] == 404, p


def test_stats_and_rates(api):
    call, jcall, state, f = api
    status, st = jcall("GET", "/api/stats")
    assert status == 200 and st["online"] and st["routes"] == 2
    status, resp = jcall("POST", "/api/send", {"hex": "30ff0002000000640102", "count": 20})
    assert status == 200 and resp["sent"] == 20
    time.sleep(0.2)
    state.sample()
    time.sleep(0.6)
    state.sample()
    _, st = jcall("GET", "/api/stats")
    assert st["rx"] == 20 and st["forwarded"] == 20 and st["dropped"] == 0
    _, hist = jcall("GET", "/api/history")
    assert len(hist["samples"]) >= 2


def test_route_crud_and_errors(api):
    call, jcall, state, f = api
    port = f.sink.getsockname()[1]
    status, r = jcall("POST", "/api/routes", {"in_teid": 7, "out_teid": 8, "next_hop": f"127.0.0.1:{port}"})
    assert status == 200 and r["ok"]
    assert any(x["in_teid"] == 7 for x in jcall("GET", "/api/routes")[1]["routes"])
    status, r = jcall("POST", "/api/routes", {"in_teid": 7, "out_teid": 9, "next_hop": f"127.0.0.1:{port}"})
    assert status == 409 and not r["ok"]
    assert jcall("POST", "/api/routes", {"in_teid": 1})[0] == 400                       # missing fields
    assert jcall("POST", "/api/routes", {"in_teid": 1, "out_teid": 2, "next_hop": "bad"})[0] == 409
    assert jcall("POST", "/api/routes", {"in_teid": "x", "out_teid": 2, "next_hop": "1.1.1.1:1"})[0] == 400
    assert jcall("DELETE", "/api/routes/7")[0] == 200
    assert jcall("DELETE", "/api/routes/7")[0] == 404
    assert jcall("DELETE", "/api/routes/abc")[0] == 400


def test_command_injection_in_next_hop_is_rejected(api):
    _, jcall, _, _ = api
    status, r = jcall("POST", "/api/routes", {"in_teid": 3, "out_teid": 4, "next_hop": "127.0.0.1:9 del 100"})
    assert status == 400 and not r["ok"]
    assert any(x["in_teid"] == 100 for x in jcall("GET", "/api/routes")[1]["routes"])


def test_inspect_and_send_echo_reply(api):
    _, jcall, _, _ = api
    _, r = jcall("POST", "/api/inspect", {"hex": "30ff0002000000640102"})
    assert r["action"] == "forward"
    _, r = jcall("POST", "/api/send", {"hex": "3201000400000000beef0000"})
    assert r["reply_hex"] and r["reply_hex"][2:4] == "02"          # Echo Response comes back
    assert jcall("POST", "/api/send", {"hex": "zz"})[0] == 400
    assert jcall("POST", "/api/send", {"hex": ""})[0] == 400


def test_bad_requests(api):
    call, jcall, *_ = api
    assert call("POST", "/api/inspect", raw=b"not json")[0] == 400
    assert call("POST", "/api/inspect", raw=b"[1,2]")[0] == 400
    assert call("POST", "/api/inspect", raw=b"x" * 70000)[0] == 400
    assert jcall("POST", "/api/nope", {})[0] == 404


def test_traffic_generator_moves_counters(api):
    _, jcall, state, f = api
    status, t = jcall("POST", "/api/traffic", {"pps": 400, "mix": {"valid": 1, "unknown": 1, "malformed": 1, "echo": 1}})
    assert status == 200 and t["running"]
    time.sleep(1.0)
    status, t = jcall("DELETE", "/api/traffic")
    assert not t["running"]
    state.sample()
    st = state.snapshot()
    assert st["rx"] > 100 and st["forwarded"] > 0 and st["dropped"] > 0
    assert jcall("POST", "/api/traffic", {"pps": 10, "mix": {"valid": 0, "unknown": 0, "malformed": 0, "echo": 0}})[0] == 400


def test_offline_forwarder_reports_cleanly(api):
    call, jcall, state, f = api
    f.proc.terminate()
    f.proc.wait(timeout=5)
    state.sample()
    _, st = jcall("GET", "/api/stats")
    assert st["online"] is False and "unreachable" in st["error"]
    assert call("GET", "/api/routes")[0] == 502
    assert jcall("GET", "/api/health")[1]["forwarder_online"] is False
