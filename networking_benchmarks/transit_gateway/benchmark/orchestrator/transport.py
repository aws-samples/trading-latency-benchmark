"""How shell scripts reach the instances: SSM Run Command or SSH (dev only).

Both run a script as root and return its stdout. Trailing newlines are stripped from the
output, matching what callers have always seen.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import threading
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence

from .aws import AwsError, aws
from .parallel import Outcome, par_run
from .util import Abort, Console, sleep

SSM_POLL_INTERVAL = 10
SSM_POLL_MAX_ATTEMPTS = 60
SSM_WAIT_INTERVAL = 1


@dataclass
class Result:
    ok: bool
    out: str = ""
    status: str = ""


class Transport:
    """Base class: counts remote calls and defines the two stage hooks."""

    name = "base"

    def __init__(self, console: Console):
        self.console = console
        self.calls = 0
        self._lock = threading.Lock()

    def run(self, inst: str, timeout: int, command: str) -> Result:
        """Run a script on one instance. ok is False on any failure; out is kept when there is some."""
        with self._lock:
            self.calls += 1
        r = self._run(inst, timeout, command)
        r.out = r.out.rstrip("\n")
        return r

    def _run(self, inst: str, timeout: int, command: str) -> Result:
        raise NotImplementedError

    def open(self, ids: Sequence[str]) -> None:
        """Cheap per-process setup, done on every run."""

    def wait_online(self, ids: Sequence[str]) -> None:
        """Slow reachability wait, done by prepare only."""

    def close(self) -> None:
        pass


class SsmTransport(Transport):
    name = "ssm"

    def _run(self, inst: str, timeout: int, command: str) -> Result:
        params = json.dumps({"commands": [command], "executionTimeout": [str(timeout)]})
        try:
            cid = aws("ssm", "send-command", "--instance-ids", inst, "--document-name", "AWS-RunShellScript",
                      "--parameters", params, "--query", "Command.CommandId", "--output", "text").strip()
        except AwsError as e:
            self.console.err(f"ERROR: could not send a command to {inst}: {e}")
            return Result(False, "", "SendFailed")
        out, status, elapsed = "", "Pending", 0
        while elapsed < timeout + 30:
            try:
                d = json.loads(aws("ssm", "get-command-invocation", "--command-id", cid,
                                   "--instance-id", inst, "--output", "json"))
                status, out = d.get("Status", "Pending"), d.get("StandardOutputContent", "")
            except (AwsError, ValueError):
                status = "Pending"
            if status == "Success":
                return Result(True, out, status)
            if status in ("Failed", "TimedOut", "Cancelled"):
                self.console.err(f"ERROR: SSM command {cid} on {inst} finished with status: {status}")
                return Result(False, out, status)
            sleep(SSM_WAIT_INTERVAL)
            elapsed += SSM_WAIT_INTERVAL
        self.console.err(f"ERROR: Timed out waiting for SSM command {cid} on {inst}")
        return Result(False, out, "WaitTimeout")

    def wait_online(self, ids: Sequence[str]) -> None:
        self.console.out(">>> Waiting for all instances to be SSM-ready...")
        for attempt in range(SSM_POLL_MAX_ATTEMPTS):
            try:
                online = aws("ssm", "describe-instance-information", "--filters",
                             "Key=InstanceIds,Values=" + ",".join(ids), "--query",
                             "InstanceInformationList[?PingStatus=='Online'].InstanceId",
                             "--output", "text").split()
            except AwsError:
                online = []
            missing = [i for i in ids if i not in online]
            if not missing:
                for i in ids:
                    self.console.out(f"  {i} - SSM online")
                return
            if attempt + 1 < SSM_POLL_MAX_ATTEMPTS:
                sleep(SSM_POLL_INTERVAL)
        self.console.err(f"ERROR: {' '.join(missing)} did not become SSM-ready after "
                         f"{SSM_POLL_MAX_ATTEMPTS * SSM_POLL_INTERVAL}s")
        raise Abort(1)


class SshTransport(Transport):
    """EC2 Instance Connect pushes a throwaway public key (valid 60 s, enough to open the
    connection) and one SSH connection per instance is reused for every command. The script goes
    in on stdin to `sudo bash -s`, so quoting is not an issue and the remote process list never
    contains the script text (pkill -f stays safe)."""

    name = "ssh"

    def __init__(self, console: Console, user: str, key: Optional[str]):
        super().__init__(console)
        self.user = user
        self.key = key
        self.public_ips: Dict[str, Optional[str]] = {}
        self.ctl_dir: Optional[str] = None
        self.base: List[str] = []

    def setup(self) -> None:
        for tool in ("ssh", "ssh-keygen"):
            if not shutil.which(tool):
                self.console.err(f"ERROR: --transport ssh needs '{tool}' on this machine")
                raise Abort(1)
        # short path: unix socket paths are limited to about 104 bytes
        self.ctl_dir = tempfile.mkdtemp(prefix="tgwcm.", dir="/tmp")
        if not self.key:
            self.key = os.path.join(self.ctl_dir, "key")
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", self.key],
                           check=True, capture_output=True)
        elif not (os.path.isfile(self.key) and os.path.isfile(self.key + ".pub")):
            self.console.err(f"ERROR: --ssh-key needs both {self.key} and {self.key}.pub")
            self.close()
            raise Abort(1)
        self.base = ["-i", self.key, "-o", "IdentitiesOnly=yes", "-o", "StrictHostKeyChecking=no",
                     "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", "-o", "BatchMode=yes",
                     "-o", "ConnectTimeout=15", "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4",
                     "-o", "ControlMaster=auto", "-o", "ControlPersist=300",
                     "-o", f"ControlPath={self.ctl_dir}/%C"]

    def push_key(self, inst: str) -> None:
        aws("ec2-instance-connect", "send-ssh-public-key", "--instance-id", inst,
            "--instance-os-user", self.user, "--ssh-public-key", f"file://{self.key}.pub",
            "--query", "Success", "--output", "text")

    def open(self, ids: Sequence[str]) -> None:
        def worker(i: str) -> Outcome:
            try:
                self.push_key(i)
            except AwsError:
                return Outcome(i, False, err=[f"ERROR: could not push an SSH key to {i} "
                                              "(needs ec2-instance-connect:SendSSHPublicKey)"])
            return Outcome(i, True, out=[f"  {i} - SSH key pushed"])

        self.console.out(">>> Opening SSH access to all instances...")
        outcomes = par_run(self.console, worker, list(ids))
        if not all(o.ok for o in outcomes):
            raise Abort(1)

    def _run(self, inst: str, timeout: int, command: str) -> Result:
        ip = self.public_ips.get(inst)
        if not ip:
            self.console.err(f"ERROR: {inst} has no public IP; use --transport ssm")
            return Result(False, "", "Failed")
        retried = False
        while True:
            with tempfile.TemporaryFile() as errf:
                try:
                    p = subprocess.run(["ssh", *self.base, f"{self.user}@{ip}", "sudo", "bash", "-s"],
                                       input=command, stdout=subprocess.PIPE, stderr=errf, text=True,
                                       timeout=timeout + 30)
                    rc, out = p.returncode, p.stdout
                except subprocess.TimeoutExpired as e:
                    rc = 142
                    out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
                errf.seek(0)
                errtxt = errf.read().decode(errors="replace")
            # 255 is a connection or authentication failure. The pushed key only authorizes a new
            # connection for 60 s, so push again and retry once.
            if rc == 255 and not retried:
                retried = True
                try:
                    self.push_key(inst)
                except AwsError:
                    pass
                continue
            break
        if rc == 0:
            return Result(True, out, "Success")
        if rc == 142:
            self.console.err(f"ERROR: command on {inst} timed out")
            return Result(False, out, "TimedOut")
        self.console.err(f"ERROR: command on {inst} failed (exit {rc})")
        for line in errtxt.splitlines():
            self.console.err(f"    ssh: {line}")
        return Result(False, out, "Failed")

    def close(self) -> None:
        if self.ctl_dir and os.path.isdir(self.ctl_dir):
            for name in os.listdir(self.ctl_dir):
                sock = os.path.join(self.ctl_dir, name)
                if os.path.exists(sock) and not os.path.isfile(sock):
                    subprocess.run(["ssh", "-o", f"ControlPath={sock}", "-O", "exit", "placeholder"],
                                   capture_output=True)
            shutil.rmtree(self.ctl_dir, ignore_errors=True)
        self.ctl_dir = None


def make_transport(kind: str, console: Console, ssh_user: str, ssh_key: Optional[str]) -> Transport:
    if kind == "ssh":
        t = SshTransport(console, ssh_user, ssh_key)
        t.setup()
        return t
    return SsmTransport(console)
