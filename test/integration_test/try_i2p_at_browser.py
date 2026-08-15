# This file is not to be used with pytest, for manual tests only

import asyncio
import signal

from test_fixtures import TestFixtures
from test_http import (
    run_i2p_client,
    run_i2p_injector_with_cache_pub_key,
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
    i2pinjector = run_i2p_injector_with_cache_pub_key(
        [
            "--listen-on-i2p",
            "true",
            "--log-level",
            "DEBUG",
            "--i2p-hops-per-tunnel",
            str(TestFixtures.I2P_FAST_TUNNEL_HOP_COUNT),
        ]
    )
    return i2pinjector


def cacheless_i2p_client(injector_i2p_public_id):
    """
    Baseline I2P client — talks straight to the injector, no BEP3 cache.
    Serves as a control: if this stops working the setup itself is broken,
    not the BEP3 cache path.
    """
    return run_i2p_client(
        TestFixtures.I2P_CLIENT["name"],
        None,
        [
            "--disable-origin-access",
            "--disable-cache",
            "--listen-on-tcp",
            "127.0.0.1:" + str(TestFixtures.I2P_CLIENT["port"]),
            "--front-end-ep",
            "127.0.0.1:" + str(TestFixtures.I2P_CLIENT["fe_port"]),
            "--injector-ep",
            "i2p:" + injector_i2p_public_id,
            "--i2p-hops-per-tunnel",
            str(TestFixtures.I2P_FAST_TUNNEL_HOP_COUNT),
        ],
    )


def bep3_cache_contributor(cfg, index_key, injector_i2p_public_id):
    """
    Cache-contributing I2P client: knows the injector, fetches via it,
    caches via BEP3-over-I2P, and announces to the BEP3 tracker.
    """
    return run_i2p_client(
        name=cfg["name"],
        idx_key=None,
        args=[
            "--disable-origin-access",
            "--cache-type",
            "bep3-http-over-i2p",
            "--cache-http-public-key",
            index_key,
            "--i2p-bep3-tracker",
            TestFixtures.BEP3_TRACKER_ID,
            "--listen-on-tcp",
            "127.0.0.1:" + str(cfg["port"]),
            "--front-end-ep",
            "127.0.0.1:" + str(cfg["fe_port"]),
            "--injector-ep",
            "i2p:" + injector_i2p_public_id,
            "--i2p-hops-per-tunnel",
            str(TestFixtures.I2P_FAST_TUNNEL_HOP_COUNT),
        ],
    )


def bep3_puller_client(cfg, index_key):
    """
    Puller I2P client: does NOT know the injector; the only way for it to
    obtain fresh content is by looking peers up via the BEP3 tracker and
    fetching from a cache contributor.
    """
    return run_i2p_client(
        name=cfg["name"],
        idx_key=None,
        args=[
            "--disable-origin-access",
            "--disable-proxy-access",
            "--cache-type",
            "bep3-http-over-i2p",
            "--cache-http-public-key",
            index_key,
            "--i2p-bep3-tracker",
            TestFixtures.BEP3_TRACKER_ID,
            "--listen-on-tcp",
            "127.0.0.1:" + str(cfg["port"]),
            "--front-end-ep",
            "127.0.0.1:" + str(cfg["fe_port"]),
            "--i2p-hops-per-tunnel",
            str(TestFixtures.I2P_FAST_TUNNEL_HOP_COUNT),
        ],
    )


async def exit_on_demand():
    await ctrl_c.wait()
    await cleanup()


async def main():
    """
    Set up an I2P injector plus three I2P clients so you can manually verify
    BEP3-cache-over-I2P behaviour in a browser:

      * client1 — baseline. Uses the injector directly, NO cache. If this
                  works but client2/client3 don't, the fault is in the
                  BEP3 cache path, not the surrounding setup.
      * client2 — BEP3 cache contributor. Knows the injector, caches and
                  announces to the BEP3 tracker.
      * client3 — pure puller. Does NOT know the injector; can only fetch
                  content that some contributor has already announced.
    """
    all_dirs()

    i2pinjector = default_i2p_injector()

    # The pubkey line is emitted before the tunnel is fully advertised, so
    # grab the key first, then wait on the tunnel being ready.
    await wait_for_benchmark(i2pinjector, TestFixtures.BEP5_PUBK_ANNOUNCE_REGEX)
    index_key = i2pinjector.get_index_key()
    assert index_key
    print("Index key: " + index_key)

    await wait_for_benchmark(i2pinjector, TestFixtures.I2P_TUNNEL_READY_REGEX)
    injector_i2p_public_id = i2pinjector.get_I2P_public_ID()
    assert injector_i2p_public_id
    print("Injector I2P id: " + injector_i2p_public_id)

    # Baseline (no BEP3 cache).
    # client1 = cacheless_i2p_client(injector_i2p_public_id)
    # await wait_for_benchmark(client1, TestFixtures.I2P_TUNNEL_READY_REGEX)

    # BEP3 cache contributor.
    client2 = bep3_cache_contributor(
        TestFixtures.CACHE_CLIENT[0], index_key, injector_i2p_public_id
    )
    await wait_for_benchmark(client2, TestFixtures.I2P_TUNNEL_READY_REGEX)

    # # BEP3 cache puller — no --injector-ep.
    client3 = bep3_puller_client(TestFixtures.CACHE_CLIENT[1], index_key)
    await wait_for_benchmark(client3, TestFixtures.I2P_TUNNEL_READY_REGEX)

    print()
    print(f"client1 (baseline, no cache)  proxy: localhost:{TestFixtures.I2P_CLIENT['port']}")
    print(f"client2 (BEP3 cache contributor) proxy: localhost:{TestFixtures.CACHE_CLIENT[0]['port']}")
    print(f"client3 (BEP3 cache puller)      proxy: localhost:{CACHE_CLIENT_3['port']}")
    print()
    print(
        "Test flow: browse a URL through client1 to confirm the injector "
        "path works. Then browse the same URL through client2 to seed the "
        "BEP3 cache. Finally hit that URL through client3 — it must "
        "succeed via BEP3 cache lookup only."
    )
    print("press ctrl+c when done")

    await exit_on_demand()


if __name__ == "__main__":
    monitor_ctrl_c()
    asyncio.run(main())
