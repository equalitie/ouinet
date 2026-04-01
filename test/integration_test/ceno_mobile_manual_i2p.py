import asyncio
from try_i2p_at_browser import (
    monitor_ctrl_c,
    all_dirs,
    default_i2p_injector,
    exit_on_demand,
)
from test_http import wait_for_benchmark
from test_fixtures import TestFixtures


async def main():
    """
    A simple script for mobile Ceno to have a test injector
    """
    all_dirs()

    i2pinjector = default_i2p_injector()
    await wait_for_benchmark(i2pinjector, TestFixtures.I2P_TUNNEL_READY_REGEX)
    injector_i2p_public_id = i2pinjector.get_I2P_public_ID()
    assert injector_i2p_public_id

    print("Injector endpoint for Ceno browser:")
    print("--injector-ep", "i2p:" + injector_i2p_public_id)
    await exit_on_demand()


if __name__ == "__main__":
    monitor_ctrl_c()
    asyncio.run(main())
