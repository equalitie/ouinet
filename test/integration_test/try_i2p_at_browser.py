# This file is not to be used with pytest, for manual tests only

import asyncio
import signal

from test_fixtures import TestFixtures
from test_http import (
    run_i2p_client,
    run_i2p_injector,
    wait_for_benchmark,
    cleanup,
    all_dirs,
)

ctrl_c = asyncio.Event()


def shutdown(_x, _y):
    ctrl_c.set()


def monitor_ctrl_c():
    signal.signal(signal.SIGINT, shutdown)


def default_i2p_injector():
    i2pinjector = run_i2p_injector(
        [
            "--listen-on-i2p",
            "true",
            "--log-level",
            "DEBUG",
        ]
    )
    return i2pinjector


async def exit_on_demand():
    await ctrl_c.wait()
    await cleanup()


async def main():
    """
    provide a proxy to manually see if browsing works
    """
    all_dirs()

    i2pinjector = default_i2p_injector()

    # wait for the injector tunnel to be advertised
    await wait_for_benchmark(i2pinjector, TestFixtures.I2P_TUNNEL_READY_REGEX)
    # Gets generated only when injector is ready
    injector_i2p_public_id = i2pinjector.get_I2P_public_ID()
    assert injector_i2p_public_id

    i2p_client = run_i2p_client(
        TestFixtures.I2P_CLIENT["name"],
        None,
        [
            "--disable-origin-access",
            "--disable-cache",
            "--listen-on-tcp",
            "127.0.0.1:" + str(TestFixtures.I2P_CLIENT["port"]),
            "--injector-ep",
            "i2p:" + injector_i2p_public_id,
        ],
    )
    await wait_for_benchmark(i2p_client, TestFixtures.I2P_TUNNEL_READY_REGEX)

    print(
        f"Done. In your browser, use localhost:{TestFixtures.I2P_CLIENT["port"]} as a proxy"
    )
    print("press ctrl+c when done")

    await exit_on_demand()


if __name__ == "__main__":
    monitor_ctrl_c()
    asyncio.run(main())
