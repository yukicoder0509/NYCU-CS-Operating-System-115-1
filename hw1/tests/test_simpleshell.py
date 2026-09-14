#!/usr/bin/env python3
"""
Black-box test bench for "A Simple Shell" (HW1, Intro to OS, CS@NYCU).

This harness does NOT read or depend on simpleshell.c in any way. It only
knows about the compiled executable and drives it exactly the way a human
would at a real terminal (through a pty), then checks behavior against the
assignment spec (hw1_simpleshell_spec.pdf):

  - prompt "> ", read a line, parse into <program> <arg1> <arg2> ...
  - fork + exec + wait (foreground blocks until child exits)
  - trailing "&" -> run in background, prompt returns immediately
  - no zombie children left behind (transient zombies are fine, persistent
    ones are not), whether via SIGCHLD reaping, double-fork, or otherwise
  - bonus: single "<" or ">" redirection; single "|" pipe

It deliberately avoids any assumption about internal implementation choices
beyond what the spec dictates.

Usage:
    python3 tests/test_simpleshell.py [--binary PATH | --source PATH]

Default: compiles ../simpleshell.c fresh if present, else falls back to
../a.out.
"""

import argparse
import os
import pty
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(HERE)


# --------------------------------------------------------------------------
# Low-level pty-driven shell session
# --------------------------------------------------------------------------

class PromptTimeout(Exception):
    pass


class ShellSession:
    """Spawns the shell attached to a pty, like a real interactive terminal."""

    def __init__(self, binary, cwd):
        self.binary = os.path.abspath(binary)
        self.cwd = cwd
        self.reaped = False
        pid, fd = pty.fork()
        if pid == 0:
            # child
            try:
                os.chdir(self.cwd)
                os.execv(self.binary, [self.binary])
            except Exception as e:
                os.write(2, str(e).encode())
            os._exit(127)
        self.pid = pid
        self.fd = fd

    def _read_available(self, timeout):
        try:
            r, _, _ = select.select([self.fd], [], [], timeout)
        except (OSError, ValueError):
            return None
        if self.fd in r:
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                return None
            if not data:
                return None
            return data.decode(errors="replace")
        return ""

    def wait_for_prompt(self, overall_timeout=6.0):
        """Read until the trailing '>' prompt marker shows up. Returns
        (raw_text_since_call, elapsed_seconds)."""
        start = time.time()
        local = ""
        while True:
            chunk = self._read_available(0.05)
            if chunk is None:
                raise EOFError(
                    "shell process closed its terminal / exited unexpectedly "
                    f"(buffer so far: {local!r})"
                )
            local += chunk
            normalized = local.replace("\r\n", "\n").replace("\r", "\n")
            if normalized.rstrip(" ").endswith(">"):
                return normalized, time.time() - start
            if time.time() - start > overall_timeout:
                raise PromptTimeout(
                    f"timed out waiting for '>' prompt after {overall_timeout}s; "
                    f"received so far: {local!r}"
                )

    def send_line(self, line):
        os.write(self.fd, (line + "\n").encode())

    def send_eof(self):
        os.write(self.fd, b"\x04")

    def is_alive(self):
        return (not self.reaped) and os.path.exists(f"/proc/{self.pid}")

    def wait_exit(self, timeout=3.0):
        start = time.time()
        while time.time() - start < timeout:
            try:
                pid, _status = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                self.reaped = True
                return True
            if pid == self.pid:
                self.reaped = True
                return True
            time.sleep(0.05)
        return False

    def close(self):
        if not self.reaped:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(self.pid, 0)
            except ChildProcessError:
                pass
            self.reaped = True
        try:
            os.close(self.fd)
        except OSError:
            pass


def extract_output(raw):
    """Strip the pty's local echo of the typed command and the trailing
    '>' prompt marker, leaving just what the program under test produced."""
    idx = raw.find("\n")
    body = raw[idx + 1:] if idx != -1 else ""
    body = body.rstrip(" ")
    if body.endswith(">"):
        body = body[:-1]
    return body.rstrip("\n")


def run_cmd(session, cmd, timeout=6.0):
    """Send a command line, wait for the next prompt, return (output, elapsed)."""
    session.send_line(cmd)
    raw, elapsed = session.wait_for_prompt(overall_timeout=timeout)
    return extract_output(raw), elapsed


# --------------------------------------------------------------------------
# /proc based zombie / descendant inspection (no dependency on `ps`)
# --------------------------------------------------------------------------

def snapshot_processes():
    """dict pid -> (ppid, state) for every process currently visible."""
    out = {}
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open(f"/proc/{name}/stat", "rb") as f:
                content = f.read().decode(errors="replace")
        except (FileNotFoundError, ProcessLookupError):
            continue
        rpar = content.rfind(")")
        if rpar == -1:
            continue
        fields = content[rpar + 2:].split()
        if len(fields) < 2:
            continue
        out[int(name)] = (int(fields[1]), fields[0])  # (ppid, state)
    return out


def descendants_of(root_pid, snapshot):
    children = {}
    for pid, (ppid, _state) in snapshot.items():
        children.setdefault(ppid, []).append(pid)
    result = []
    stack = [root_pid]
    while stack:
        cur = stack.pop()
        for c in children.get(cur, []):
            result.append(c)
            stack.append(c)
    return result


# --------------------------------------------------------------------------
# Test infrastructure
# --------------------------------------------------------------------------

class TestResult:
    def __init__(self, name, category, passed, message=""):
        self.name = name
        self.category = category
        self.passed = passed
        self.message = message


RESULTS = []
ALL_TESTS = []


def make_session(binary, files):
    tmpdir = tempfile.mkdtemp(prefix="simpleshell_test_")
    for relpath, content in files.items():
        full = os.path.join(tmpdir, relpath)
        os.makedirs(os.path.dirname(full) or tmpdir, exist_ok=True)
        with open(full, "w") as f:
            f.write(content)
    session = ShellSession(binary, tmpdir)
    return session, tmpdir


def register(name, category, files, body):
    """Registers a test: spawns a fresh shell + temp dir, consumes the
    startup prompt, runs `body(session, tmpdir)`, tears everything down."""

    def run(binary):
        session, tmpdir = make_session(binary, files)
        try:
            session.wait_for_prompt(overall_timeout=4.0)  # consume startup prompt
            body(session, tmpdir)
            RESULTS.append(TestResult(name, category, True))
        except AssertionError as e:
            RESULTS.append(TestResult(name, category, False, str(e)))
        except (PromptTimeout, EOFError) as e:
            RESULTS.append(TestResult(name, category, False, f"{type(e).__name__}: {e}"))
        except Exception as e:
            RESULTS.append(TestResult(name, category, False, f"unexpected error: {e!r}"))
        finally:
            session.close()
            shutil.rmtree(tmpdir, ignore_errors=True)

    ALL_TESTS.append(run)


def _basic_files():
    return {
        "alpha.txt": "Alpha file\n",
        "beta.txt": "Beta file\n",
        "sub/.keep": "",
    }


def expected_ls_entries(tmpdir):
    return sorted(e for e in os.listdir(tmpdir) if not e.startswith("."))


# --------------------------------------------------------------------------
# CORE tests
# --------------------------------------------------------------------------

def _initial_prompt(binary):
    """The very first prompt is checked separately since register() normally
    consumes it as setup."""
    tmpdir = tempfile.mkdtemp(prefix="simpleshell_test_")
    session = ShellSession(binary, tmpdir)
    try:
        raw, _elapsed = session.wait_for_prompt(overall_timeout=4.0)
        assert raw.rstrip(" ").endswith(">"), f"expected '>' prompt on startup, got: {raw!r}"
        RESULTS.append(TestResult("initial prompt is displayed", "CORE", True))
    except AssertionError as e:
        RESULTS.append(TestResult("initial prompt is displayed", "CORE", False, str(e)))
    except (PromptTimeout, EOFError) as e:
        RESULTS.append(TestResult("initial prompt is displayed", "CORE", False, f"{type(e).__name__}: {e}"))
    finally:
        session.close()
        shutil.rmtree(tmpdir, ignore_errors=True)


ALL_TESTS.append(_initial_prompt)


def _exec_ls_basic(session, tmpdir):
    out, _elapsed = run_cmd(session, "ls")
    got = set(out.split())
    expected = set(expected_ls_entries(tmpdir))
    assert expected.issubset(got), \
        f"expected 'ls' output to contain {sorted(expected)}, got tokens {sorted(got)} (raw: {out!r})"


register(
    "exec 'ls' finds files via PATH lookup",
    "CORE", _basic_files(), _exec_ls_basic,
)


def _exec_absolute_path(session, tmpdir):
    out, _elapsed = run_cmd(session, "/bin/ls")
    got = set(out.split())
    expected = set(expected_ls_entries(tmpdir))
    assert expected.issubset(got), \
        f"expected '/bin/ls' output to contain {sorted(expected)}, got {sorted(got)} (raw: {out!r})"


register(
    "exec by absolute path (/bin/ls)",
    "CORE", _basic_files(), _exec_absolute_path,
)


def _exec_echo_args(session, tmpdir):
    out, _elapsed = run_cmd(session, "echo hello world foo")
    assert out.strip() == "hello world foo", f"expected 'hello world foo', got {out!r}"


register("exec program with multiple args (echo)", "CORE", {}, _exec_echo_args)


def _exec_cp(session, tmpdir):
    run_cmd(session, "cp alpha.txt alpha_copy.txt")
    dst = os.path.join(tmpdir, "alpha_copy.txt")
    assert os.path.exists(dst), "'cp alpha.txt alpha_copy.txt' did not create alpha_copy.txt"
    with open(dst) as f:
        assert f.read() == "Alpha file\n", "copied file content does not match source"


register("exec 'cp a b' copies a file correctly", "CORE", _basic_files(), _exec_cp)


def _exec_which(session, tmpdir):
    out, _elapsed = run_cmd(session, "which ls")
    ref = subprocess.run(["which", "ls"], capture_output=True, text=True).stdout.strip()
    assert out.strip() == ref, f"expected 'which ls' -> {ref!r}, got {out!r}"


register("exec 'which ls' (spec example)", "CORE", {}, _exec_which)


def _nonexistent_program(session, tmpdir):
    session.send_line("this_program_does_not_exist_xyz123")
    raw, _elapsed = session.wait_for_prompt(overall_timeout=4.0)
    assert raw.rstrip(" ").endswith(">"), "shell did not reprompt after an invalid command"
    assert session.is_alive(), "shell process died after an invalid command"
    out, _ = run_cmd(session, "echo still_alive")
    assert out.strip() == "still_alive", "shell not responsive after an invalid command"


register("nonexistent program does not crash the shell", "CORE", {}, _nonexistent_program)


def _foreground_blocks(session, tmpdir):
    _out, elapsed = run_cmd(session, "sleep 1", timeout=6.0)
    assert elapsed >= 0.8, \
        f"'sleep 1' returned the prompt after only {elapsed:.2f}s; " \
        "shell should wait() for the foreground child"
    assert elapsed <= 4.0, f"'sleep 1' took {elapsed:.2f}s to return the prompt; unexpectedly slow"


register("foreground command blocks until child exits (wait)", "CORE", {}, _foreground_blocks)


def _foreground_no_zombie(session, tmpdir):
    run_cmd(session, "echo hi")
    snap = snapshot_processes()
    zombies = [pid for pid in descendants_of(session.pid, snap) if snap[pid][1] == "Z"]
    assert not zombies, f"found zombie descendant(s) right after a foreground command: {zombies}"


register("no zombie remains after a foreground command", "CORE", {}, _foreground_no_zombie)


def _background_immediate_and_reaped(session, tmpdir):
    before_descendants = set(descendants_of(session.pid, snapshot_processes()))

    send_time = time.time()
    session.send_line("sleep 2 &")
    raw, elapsed = session.wait_for_prompt(overall_timeout=4.0)
    assert elapsed <= 1.0, f"'sleep 2 &' should return the prompt almost immediately, took {elapsed:.2f}s"

    # Identify newly-spawned descendants tracked directly under the shell.
    # A compliant double-fork implementation may show none here -- that's fine,
    # what matters is that no zombie persists.
    new_pids = [
        p for p in descendants_of(session.pid, snapshot_processes())
        if p not in before_descendants
    ]

    zombie_streak = {}
    persistent_zombie = None
    deadline = send_time + 4.0
    while time.time() < deadline:
        snap = snapshot_processes()
        for pid in descendants_of(session.pid, snap):
            if snap[pid][1] == "Z":
                zombie_streak[pid] = zombie_streak.get(pid, 0) + 1
            else:
                zombie_streak[pid] = 0
            if zombie_streak[pid] >= 4:  # zombie for ~0.8s straight
                persistent_zombie = pid
        if persistent_zombie:
            break
        time.sleep(0.2)

    assert persistent_zombie is None, \
        f"pid {persistent_zombie} stayed a zombie for a sustained period after " \
        "backgrounding 'sleep 2 &' -- child was not reaped"

    assert session.is_alive(), "shell died while/after handling a background job"
    out, _ = run_cmd(session, "echo still_here", timeout=4.0)
    assert out.strip() == "still_here", "shell not responsive after a background job completed"

    final_snap = snapshot_processes()
    lingering = [p for p in new_pids if p in final_snap and final_snap[p][1] == "Z"]
    assert not lingering, f"background child(ren) still zombie after completion: {lingering}"


register(
    "background '&' returns immediately, no zombie left behind",
    "CORE", {}, _background_immediate_and_reaped,
)


# --------------------------------------------------------------------------
# BONUS: I/O redirection
# --------------------------------------------------------------------------

def _redirect_output_echo(session, tmpdir):
    out, _elapsed = run_cmd(session, "echo redirected_text > out.txt")
    assert out.strip() == "", f"stdout should be redirected to the file, not printed; got {out!r}"
    outfile = os.path.join(tmpdir, "out.txt")
    assert os.path.exists(outfile), "'echo ... > out.txt' did not create out.txt"
    with open(outfile) as f:
        content = f.read()
    assert content.strip() == "redirected_text", f"expected 'redirected_text' in out.txt, got {content!r}"


register("[bonus] output redirection '>' (echo)", "BONUS", {}, _redirect_output_echo)


def _redirect_output_ls(session, tmpdir):
    run_cmd(session, "ls -l > out2.txt")
    outfile = os.path.join(tmpdir, "out2.txt")
    assert os.path.exists(outfile), "'ls -l > out2.txt' did not create out2.txt"
    with open(outfile) as f:
        content = f.read()
    for name in expected_ls_entries(tmpdir):
        if name == "out2.txt":
            continue
        assert name in content, f"expected {name!r} to appear in redirected 'ls -l' output"


register("[bonus] output redirection '>' (ls -l)", "BONUS", _basic_files(), _redirect_output_ls)


def _redirect_input_cat(session, tmpdir):
    out, _elapsed = run_cmd(session, "cat < in.txt")
    assert out.strip() == "hello redir", f"expected 'hello redir', got {out!r}"


register("[bonus] input redirection '<' (cat)", "BONUS", {"in.txt": "hello redir\n"}, _redirect_input_cat)


# --------------------------------------------------------------------------
# BONUS: pipe
# --------------------------------------------------------------------------

def _pipe_ls_wc(session, tmpdir):
    out, _elapsed = run_cmd(session, "ls | wc -l")
    expected_count = len(expected_ls_entries(tmpdir))
    got = out.strip().split()
    assert got and got[0].isdigit(), f"expected a number from 'ls | wc -l', got {out!r}"
    assert int(got[0]) == expected_count, \
        f"expected 'ls | wc -l' == {expected_count}, got {got[0]} (raw: {out!r})"


register("[bonus] pipe 'ls | wc -l'", "BONUS", _basic_files(), _pipe_ls_wc)


def _pipe_cat_wc(session, tmpdir):
    out, _elapsed = run_cmd(session, "cat lines.txt | wc -l")
    got = out.strip().split()
    assert got and got[0].isdigit(), f"expected a number, got {out!r}"
    assert int(got[0]) == 3, f"expected 3 lines, got {got[0]} (raw: {out!r})"


register("[bonus] pipe 'cat file | wc -l'", "BONUS", {"lines.txt": "one\ntwo\nthree\n"}, _pipe_cat_wc)


# --------------------------------------------------------------------------
# INFO: not explicitly mandated by the spec, but a reasonable expectation
# --------------------------------------------------------------------------

def _eof_terminates(session, tmpdir):
    session.send_eof()
    exited = session.wait_exit(timeout=3.0)
    assert exited, "shell did not terminate within 3s after EOF (Ctrl-D) on stdin"


register("[info] shell exits gracefully on EOF (Ctrl-D)", "INFO", {}, _eof_terminates)


# --------------------------------------------------------------------------
# Runner
# --------------------------------------------------------------------------

def resolve_binary(args):
    if args.binary:
        if not os.path.exists(args.binary):
            print(f"error: binary not found: {args.binary}")
            sys.exit(1)
        return args.binary

    src = args.source
    if not src:
        candidate = os.path.join(PROJECT_DIR, "simpleshell.c")
        if os.path.exists(candidate):
            src = candidate

    if src:
        build_dir = tempfile.mkdtemp(prefix="simpleshell_build_")
        tmp_bin = os.path.join(build_dir, "simpleshell_under_test")
        r = subprocess.run(["gcc", "-Wall", "-o", tmp_bin, src], capture_output=True, text=True)
        if r.returncode != 0:
            print("Compilation failed:\n" + r.stderr)
            sys.exit(1)
        print(f"[built {src} -> {tmp_bin}]")
        return tmp_bin

    fallback = os.path.join(PROJECT_DIR, "a.out")
    if os.path.exists(fallback):
        print(f"[no source found, using existing binary {fallback}]")
        return fallback

    print("error: no --binary, no --source, no simpleshell.c, and no a.out found")
    sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", help="path to an already-built shell executable")
    parser.add_argument("--source", help="path to a .c file to compile fresh with gcc")
    args = parser.parse_args()

    binary = resolve_binary(args)
    print(f"Running black-box tests against: {binary}\n")

    for fn in ALL_TESTS:
        fn(binary)

    overall_core_pass = True
    for cat in ("CORE", "BONUS", "INFO"):
        cat_results = [r for r in RESULTS if r.category == cat]
        if not cat_results:
            continue
        print(f"== {cat} " + "=" * (60 - len(cat)))
        for r in cat_results:
            status = "PASS" if r.passed else "FAIL"
            print(f"[{status}] {r.name}")
            if not r.passed:
                print(f"       -> {r.message}")
                if cat == "CORE":
                    overall_core_pass = False
        print()

    total = len(RESULTS)
    passed = sum(1 for r in RESULTS if r.passed)
    core_results = [r for r in RESULTS if r.category == "CORE"]
    core_passed = sum(1 for r in core_results if r.passed)
    bonus_results = [r for r in RESULTS if r.category == "BONUS"]
    bonus_passed = sum(1 for r in bonus_results if r.passed)

    print("=" * 64)
    print(f"CORE:  {core_passed}/{len(core_results)} passed")
    print(f"BONUS: {bonus_passed}/{len(bonus_results)} passed")
    print(f"TOTAL: {passed}/{total} passed")

    sys.exit(0 if overall_core_pass else 1)


if __name__ == "__main__":
    main()
