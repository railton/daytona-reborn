"""Discover and handshake installed account clients without an inference turn.

Output contains local paths, not tokens. Keep the output directory untracked.
No installation, login, subscription change or API-key fallback is performed.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import time

from team_rpc import JsonLines, RpcError, acp_initialize


def discover():
    clients = {}
    for name, protocol, args in [("grok", "acp", ["agent", "stdio"]),
                                  ("codex", "codex", ["app-server", "--listen", "stdio://"])]:
        executable = shutil.which(name)
        if executable:
            clients[name] = {"protocol": protocol, "argv": [executable, *args],
                             "billing": "existing-account"}
    if os.name == "nt":
        home = Path(os.environ["LOCALAPPDATA"]) / "Programs/DeepSeek Harness"
        if (home / "DeepSeek Harness.exe").is_file():
            clients["deepseek"] = {"protocol": "acp", "argv": [str(home / "DeepSeek Harness.exe"),
                "--expose-internals", str(home / "resources/app.asar/dsh/node_modules/@deepseek-ai/dsh-desktop-host/lib/cli.js"), "acp"],
                "env": {"ELECTRON_RUN_AS_NODE": "1"}, "billing": "existing-account",
                "account_model": '["deepseek-account","deepseek-flash"]'}
    elif executable := shutil.which("dsh"):
        clients["deepseek"] = {"protocol": "acp", "argv": [executable, "acp"],
            "billing": "existing-account", "account_model": '["deepseek-account","deepseek-flash"]'}
    return clients


def probe(name, cfg, cwd):
    env = os.environ.copy(); env.update(cfg.get("env", {}))
    with JsonLines(cfg["argv"], cwd, env) as rpc:
        deadline = time.monotonic() + 45
        if cfg["protocol"] == "acp":
            hello, session = acp_initialize(rpc, cwd, deadline)
            result = {"client": name, "protocol": "acp", "version": hello.get("protocolVersion"),
                      "session": session["sessionId"], "inference_tested": False}
            if "account_model" in cfg:
                selected = rpc.request("session/set_config_option", {"sessionId": session["sessionId"],
                    "configId": "model", "value": cfg["account_model"]}, deadline)
                if not any(o.get("id") == "model" and o.get("currentValue") == cfg["account_model"]
                           for o in selected.get("configOptions", [])):
                    raise RpcError("existing account provider unavailable")
                result["account_model"] = cfg["account_model"]
            return result
        rpc.request("initialize", {"clientInfo": {"name": "daytona-team", "title": "Daytona team",
            "version": "0.1"}, "capabilities": {}}, deadline)
        rpc.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        account = rpc.request("account/read", {}, deadline).get("account") or {}
        if account.get("type") != "chatgpt":
            raise RpcError("existing ChatGPT account required")
        return {"client": name, "protocol": "codex", "account_type": account["type"],
                "inference_tested": False}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=Path("build-team"))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    clients = discover(); checked = {}; results = []
    for name, cfg in clients.items():
        try:
            results.append(probe(name, cfg, str(Path.cwd())))
            checked[name] = cfg
        except Exception as exc:
            results.append({"client": name, "error": str(exc)[:1000]})
    (args.out / "backends.local.json").write_text(json.dumps(checked, indent=2), encoding="utf-8")
    (args.out / "probe.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(json.dumps(results, indent=2))
    return 0 if set(checked) == {"codex", "grok", "deepseek"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
