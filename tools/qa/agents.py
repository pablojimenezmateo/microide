#!/usr/bin/env python3
"""Simulated coding agents writing a git tree, for measuring a microide mirror.

  agents.py burst ROOT N TOKEN        edit N tracked source files at once (one line each)
  agents.py one ROOT PATH TOKEN       append TOKEN to one file
  agents.py churn ROOT AGENTS SECONDS each agent loops: edit a function, add/remove its own
                                       files, commit now and then; prints totals
  agents.py reset ROOT                 drop everything the agents did
"""
import os, random, subprocess, sys, time

SRC_EXT = (".cpp", ".h", ".md", ".py", ".txt", ".cmake", ".json")


def tracked(root):
    out = subprocess.run(["git", "-C", root, "ls-files", "-z"], capture_output=True, check=True).stdout
    return [p for p in out.decode().split("\0") if p.endswith(SRC_EXT) and os.path.isfile(os.path.join(root, p))]


def write_atomic(path, data):
    # Agents (editors, LLM tools) mostly write via temp + rename; some truncate in place.
    if random.random() < 0.5:
        tmp = path + ".agent-tmp"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    else:
        with open(path, "wb") as f:
            f.write(data)


def edit_middle(path, line):
    with open(path, "rb") as f:
        lines = f.read().split(b"\n")
    at = random.randrange(len(lines) + 1)
    lines.insert(at, line.encode())
    write_atomic(path, b"\n".join(lines))


def burst(root, n, token):
    files = tracked(root)
    random.shuffle(files)
    for p in files[:n]:
        with open(os.path.join(root, p), "ab") as f:
            f.write(("\n// " + token + "\n").encode())
    print(len(files[:n]))


def churn(root, agents, seconds):
    files = tracked(root)
    pids = []
    for a in range(agents):
        pid = os.fork()
        if pid == 0:
            random.seed(a * 7919 + int(time.time()))
            mine = os.path.join(root, "agent-work", f"agent{a}")
            os.makedirs(mine, exist_ok=True)
            own, edits, creates, deletes = [], 0, 0, 0
            end = time.time() + seconds
            while time.time() < end:
                r = random.random()
                if r < 0.70:
                    p = os.path.join(root, random.choice(files))
                    edit_middle(p, f"    // agent{a} edit {edits}")
                    edits += 1
                elif r < 0.88:
                    name = os.path.join(mine, f"gen{creates}.cpp")
                    body = "".join(f"int agent{a}_f{creates}_{i}() {{ return {i}; }}\n" for i in range(random.randint(5, 400)))
                    write_atomic(name, body.encode())
                    own.append(name)
                    creates += 1
                elif own:
                    os.remove(own.pop(random.randrange(len(own))))
                    deletes += 1
                # A model "thinking" between tool calls, with occasional bursts of rapid writes.
                time.sleep(random.choice([0.02, 0.05, 0.1, 0.3, 0.8]))
            print(f"agent{a} edits={edits} creates={creates} deletes={deletes}", flush=True)
            os._exit(0)
        pids.append(pid)
    # One "lead agent" commits every ~10 s, as agents that commit their work do.
    end = time.time() + seconds
    commits = 0
    while time.time() < end:
        time.sleep(10)
        subprocess.run(["git", "-C", root, "add", "-A"], capture_output=True)
        if subprocess.run(["git", "-C", root, "commit", "-qm", f"agents {commits}"], capture_output=True).returncode == 0:
            commits += 1
    for pid in pids:
        os.waitpid(pid, 0)
    print(f"commits={commits}")


def reset(root):
    base = os.environ.get("AGENTS_BASE", "")
    subprocess.run(["git", "-C", root, "reset", "-q", "--hard"] + ([base] if base else []), check=True)
    subprocess.run(["git", "-C", root, "clean", "-qfdx", "-e", "build"], check=True)


if __name__ == "__main__":
    cmd, root = sys.argv[1], sys.argv[2]
    if cmd == "burst":
        burst(root, int(sys.argv[3]), sys.argv[4])
    elif cmd == "one":
        with open(os.path.join(root, sys.argv[3]), "ab") as f:
            f.write(("\n// " + sys.argv[4] + "\n").encode())
    elif cmd == "churn":
        churn(root, int(sys.argv[3]), float(sys.argv[4]))
    elif cmd == "reset":
        reset(root)
