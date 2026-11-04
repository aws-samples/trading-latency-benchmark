"""User-data wiring for the native sender/receiver build."""
import base64
import os
import pathlib
import re
import shutil
import subprocess

import aws_cdk as cdk
import pytest
from aws_cdk.assertions import Template

from stacks.main import NATIVE_SRC_PATH, BenchmarkStack
from tests.test_benchmark_stack import _get_user_data_script, _synth

TGW_DIR = pathlib.Path(__file__).resolve().parents[2]


def _ud(**kw) -> str:
    return _get_user_data_script(_synth(**kw))


def _build_script(ud: str) -> str:
    m = re.search(r"<<'NATIVE_BUILD_EOF'\n(.*?)\nNATIVE_BUILD_EOF\n", ud, re.S)
    assert m, "build script heredoc not found"
    return m.group(1)


def test_defaults_clone_and_build():
    ud = _ud()
    assert "https://github.com/aws-samples/trading-latency-benchmark.git" in ud
    assert "git fetch -q --depth 1 --filter=blob:none origin 'main'" in ud
    assert "/opt/benchmark-native-ready" in ud and "/opt/benchmark-native-failed" in ud
    assert "@" not in re.sub(r"\$@|\$\{@\}|ec2-user@", "", _build_script(ud)), "unsubstituted placeholder"


def test_build_failure_does_not_block_env_ready():
    ud = _ud()
    assert ud.index("benchmark-native-failed") < ud.index("touch /opt/benchmark-env-ready")
    assert "exit 1" not in ud[ud.index("Phase 1b"):ud.index("Phase 2:")]


def test_native_build_happens_before_ena_reload():
    ud = _ud()
    assert ud.index("Phase 1b") < ud.index("rmmod ena")


def test_context_values_are_used():
    ud = _ud(native_repo="https://github.com/me/fork.git", native_ref="feat/x")
    assert "origin 'feat/x'" in ud and "https://github.com/me/fork.git" in ud


def test_chrony_wait_gates_on_100us_and_steps():
    ud = _ud()
    assert "chronyc waitsync 30 0.0001 0 1" in ud
    assert "chronyc waitsync 30 || true" not in ud
    assert ud.index("waitsync") < ud.index("chronyc makestep")


@pytest.mark.parametrize("repo", [
    "http://github.com/a/b.git", "https://x/a'; rm -rf /; '", "https://x/a b", "git@github.com:a/b.git", "https://x/$(id)",
])
def test_bad_repo_rejected(repo):
    with pytest.raises(ValueError, match="native_repo"):
        BenchmarkStack(cdk.App(), "S", native_repo=repo)


@pytest.mark.parametrize("ref", ["main; id", "-x", "a b", "$(id)", "a..b", "a'b", ""])
def test_bad_ref_rejected(ref):
    with pytest.raises(ValueError, match="native_ref"):
        BenchmarkStack(cdk.App(), "S", native_ref=ref)


@pytest.mark.parametrize("ref", ["main", "feat/af_", "v1.2.3", "a" * 40])
def test_good_refs_accepted(ref):
    BenchmarkStack(cdk.App(), "S", native_ref=ref)


def test_user_data_fits_ec2_limit():
    ud = _ud()
    assert len(ud.encode()) < 16 * 1024


def test_user_data_parses_as_bash():
    p = subprocess.run(["bash", "-n"], input=_ud(), text=True, capture_output=True)
    assert p.returncode == 0, p.stderr
    p = subprocess.run(["bash", "-n"], input=_build_script(_ud()), text=True, capture_output=True)
    assert p.returncode == 0, p.stderr


@pytest.mark.skipif(not shutil.which("g++") and not shutil.which("c++"), reason="no C++ compiler")
def test_generated_build_script_clones_and_builds_a_local_repo(tmp_path):
    """Run the real generated script against a local repo laid out like this one."""
    if shutil.which("g++") is None:
        pytest.skip("script uses make with default CXX=g++")
    src = tmp_path / "repo"
    dst = src / NATIVE_SRC_PATH
    shutil.copytree(TGW_DIR / "benchmark" / "host" / "cpp", dst, ignore=shutil.ignore_patterns("build", "tests", "__pycache__"))
    other = src / "networking_benchmarks" / "other" / "x.txt"
    other.parent.mkdir(parents=True)
    other.write_text("not fetched\n")
    git = lambda *a: subprocess.run(["git", "-C", str(src), *a], check=True, capture_output=True)  # noqa: E731
    git("init", "-q", "-b", "main")
    git("-c", "user.name=t", "-c", "user.email=t@t", "add", "-A")
    git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "x")
    sha = subprocess.run(["git", "-C", str(src), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()

    # The mcast_recv needs Linux headers; on other hosts only the send path can be built.
    script = _build_script(_ud())
    root = tmp_path / "opt"
    script = (script.replace("/opt/benchmark-native", str(root))
              .replace("https://github.com/aws-samples/trading-latency-benchmark.git", str(src))
              .replace("'main'", f"'{sha}'")
              .replace("git fetch -q", "git -c protocol.file.allow=always fetch -q"))
    p = subprocess.run(["bash", "-c", script], capture_output=True, text=True, env=dict(os.environ))
    if os.uname().sysname != "Linux":
        assert p.returncode != 0  # mcast_recv is Linux-only; the clone and sparse steps ran first
        assert (root / "src" / NATIVE_SRC_PATH / "Makefile").exists()
        assert not (root / "src" / "networking_benchmarks" / "other").exists()
        return
    assert p.returncode == 0, p.stderr + p.stdout
    assert (root / "bin" / "mcast_send").exists() and (root / "bin" / "mcast_recv").exists()
    info = (root / "BUILD_INFO").read_text()
    assert f"commit={sha}" in info
    assert not (root / "src" / "networking_benchmarks" / "other").exists(), "sparse checkout fetched too much"


def test_refclock_line():
    ud = _ud()
    assert "refclock PHC /dev/ptp_ena poll 0 dpoll -2 delay 0.000010 prefer" in ud
    # one append, guarded so a rerun cannot add a second refclock (preflight.sh gates on exactly one)
    assert ud.count("echo 'refclock PHC") == 1 and 'grep -q "refclock PHC /dev/ptp_ena" /etc/chrony.conf' in ud
