"""Run build commands and transfer files using the server's pinned SSH key."""

import argparse
from pathlib import Path
import shlex
import socket
import sys
import warnings

warnings.filterwarnings("ignore", message="TripleDES has been moved")
import paramiko


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="ArchLinux-E5-Server")
    parser.add_argument("--address", help="Connect to this address while verifying the key for --host")
    parser.add_argument("--user", default="hedc")
    sub = parser.add_subparsers(dest="action", required=True)
    run = sub.add_parser("run")
    run.add_argument("command")
    script = sub.add_parser("script")
    script.add_argument("path", type=Path)
    script.add_argument("args", nargs="*")
    for action in ("put", "get"):
        transfer = sub.add_parser(action)
        transfer.add_argument("source")
        transfer.add_argument("destination")
    put_inputs = sub.add_parser("put-inputs")
    put_inputs.add_argument("destination")
    put_inputs.add_argument("sources", nargs="+", type=Path)
    args = parser.parse_args()
    password = sys.stdin.readline().rstrip("\r\n")
    if not password:
        parser.error("supply the SSH password on stdin")

    with paramiko.SSHClient() as client:
        client.load_system_host_keys()
        client.connect(
            args.host.lower(),
            sock=socket.create_connection((args.address, 22), 10) if args.address else None,
            username=args.user,
            password=password,
            allow_agent=False,
            look_for_keys=False,
            timeout=10,
        )
        if args.action in ("put", "get"):
            with client.open_sftp() as sftp:
                getattr(sftp, args.action)(args.source, args.destination)
            return 0
        if args.action == "put-inputs":
            with client.open_sftp() as sftp:
                for source in args.sources:
                    sftp.put(str(source), f"{args.destination.rstrip('/')}/{source.name}")
            return 0
        command = args.command if args.action == "run" else "bash -s -- " + shlex.join(args.args)
        stdin, stdout, _ = client.exec_command(command)
        stdout.channel.set_combine_stderr(True)
        if args.action == "script":
            stdin.write(args.path.read_text(encoding="utf-8"))
        stdin.channel.shutdown_write()
        for line in stdout:
            print(line, end="", flush=True)
        return stdout.channel.recv_exit_status()


if __name__ == "__main__":
    sys.exit(main())
