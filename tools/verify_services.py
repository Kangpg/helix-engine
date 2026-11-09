#!/usr/bin/env python3
"""Run native gRPC/Kafka integration checks against a real Kafka broker.

  python tools/verify_services.py
  python tools/verify_services.py --bootstrap 127.0.0.1:9092

Without --bootstrap, verifies and extracts build/kafka_2.13-4.4.0.tgz,
then starts an isolated single-node KRaft JVM on two unused loopback ports.
Requires Java 17+ and a Release build with HELIX_BUILD_SERVICES=ON.
Download the archive from https://downloads.apache.org/kafka/4.4.0/ .
Only this helper's child broker is terminated. This cleanup is not a graceful
shutdown test. Broker logs and storage are retained under build/service-runs/.
"""
import argparse
import base64
import hashlib
import os
import shutil
import socket
import subprocess
import time
import uuid
from pathlib import Path
from bootstrap_services import unpack

ROOT = Path(__file__).resolve().parents[1]
KAFKA_SHA512 = ("f5ceba8a265ab6bb2d68362b1f6859a95e15d60ef92ba83b43fdf065b557dc18"
                "e98d6383c15b9977320e25b3a2c8b6f11decb17b0fa6c0f163423793a0eb95cb")
HIDDEN = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def check(*args, **kwargs):
    print("+", *map(str, args), flush=True)
    # Explicit pipes preserve child output with CREATE_NO_WINDOW on Windows.
    result = subprocess.run(list(map(str, args)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace", creationflags=HIDDEN, **kwargs)
    print(result.stdout, end="", flush=True)
    result.check_returncode()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bootstrap", help="Use an existing broker, without starting/stopping it")
    parser.add_argument("--kafka-home", type=Path)
    parser.add_argument("--kafka-archive", type=Path, default=ROOT / "build/kafka_2.13-4.4.0.tgz")
    parser.add_argument("--executable", type=Path, default=ROOT / "build/services-x64/Release/helix_services_integration_test.exe")
    parser.add_argument("--java", default=shutil.which("java"))
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f"Build the integration test first: {executable}")
    if args.bootstrap:
        check(executable, args.bootstrap, timeout=180)
        return
    if not args.java:
        parser.error("Java is required; use --java with the java executable path")
    home = args.kafka_home.resolve() if args.kafka_home else ROOT / "build/kafka_2.13-4.4.0"
    if not args.kafka_home:
        checksum = hashlib.sha512(args.kafka_archive.read_bytes()).hexdigest()
        if checksum != KAFKA_SHA512:
            raise ValueError("Kafka archive SHA512 does not match the official Apache release")
        marker = home / ".helix-sha512"
        if not marker.is_file() or marker.read_text() != checksum:
            unpack(args.kafka_archive, home, "kafka_2.13-4.4.0")
            marker.write_text(checksum)
    if not (home / "libs").is_dir():
        parser.error(f"Kafka libraries not found: {home}")
    run = ROOT / "build/service-runs" / uuid.uuid4().hex
    run.mkdir(parents=True)
    broker_port, controller_port = port(), port()
    while broker_port == controller_port:
        controller_port = port()
    config = run / "server.properties"
    config.write_text(f"""process.roles=broker,controller
node.id=1
controller.quorum.voters=1@127.0.0.1:{controller_port}
listeners=PLAINTEXT://127.0.0.1:{broker_port},CONTROLLER://127.0.0.1:{controller_port}
advertised.listeners=PLAINTEXT://127.0.0.1:{broker_port}
listener.security.protocol.map=PLAINTEXT:PLAINTEXT,CONTROLLER:PLAINTEXT
controller.listener.names=CONTROLLER
inter.broker.listener.name=PLAINTEXT
log.dirs={(run / 'data').as_posix()}
num.partitions=1
offsets.topic.replication.factor=1
transaction.state.log.replication.factor=1
transaction.state.log.min.isr=1
group.initial.rebalance.delay.ms=0
auto.create.topics.enable=true
""", encoding="utf-8")
    java = [args.java, "-Xms256m", "-Xmx512m", "-cp", str(home / "libs/*")]
    check(*java, "kafka.tools.StorageTool", "format", "--cluster-id=" + base64.urlsafe_b64encode(uuid.uuid4().bytes).decode().rstrip("="),
          "--config", config, timeout=60)
    with (run / "broker.log").open("w", encoding="utf-8") as log:
        broker = subprocess.Popen([*java, "kafka.Kafka", str(config)], stdout=log,
                                  stderr=subprocess.STDOUT, creationflags=HIDDEN)
        try:
            deadline = time.monotonic() + 60
            while True:
                if broker.poll() is not None:
                    raise RuntimeError(f"Broker exited {broker.returncode}; see {run / 'broker.log'}")
                try:
                    with socket.create_connection(("127.0.0.1", broker_port), timeout=0.2):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f"Broker startup timed out; see {run / 'broker.log'}")
                    time.sleep(0.2)
            check(executable, f"127.0.0.1:{broker_port}", timeout=180)
        finally:
            if broker.poll() is None:
                broker.terminate()
                try:
                    broker.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    broker.kill()
                    broker.wait(timeout=10)
            print("Broker evidence:", run, flush=True)


if __name__ == "__main__":
    main()
