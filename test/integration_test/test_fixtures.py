# Copyright 2018 eQualit.ie
# See LICENSE for other credits and copying information

# Constants used in the test

class TestFixtures:
    FATAL_ERROR_INDICATOR = "[ABORT]"
    TIMEOUT_LEN = 30 # seconds
    INJECTOR_IPNS_PERSISTANT_IDENTITY = { "Identity": {"PeerID": "QmQdMc9wqxhmcwr5iYn4t48EDVUCD96FjyVn7ZdgowkxL2",
                                            "PrivKey": "CAASpwkwggSjAgEAAoIBAQDCW6vOI/6msrbswRy/wKFp/4gbhkYQ9RYFb4Ty4kKVFagK8irkyqINDr8lqBRwqUM52h0zqa0e6Yj3cJ5QEpdmanrVOZ1mVnc3gFibcQc1vIMnJz/mI14CH17NOFaxLRONGAV/98u2JsnGIHsJ+JGzpxGSyHDv5k2geaRNOYa4NUn/ry3x6gZL2G6XBMWHzgUeMvmNyZ7BKKWaVONn+QbAdJgrI32rD5q9cKptbLBiLj70a0Jxgt6zNCfnhLvRKhSW2P9bmqqTPnxYBjTsfpqV8D646n2r5P7mvD8KT5D75aiqP4EkjwyMzI3OppsxDt3EArlcWKN85k6yEFOuM75NAgMBAAECggEABXNouBlOVQKCGtW3prESVdSyzoLPiD43ZeOgyOcLkv7OfbAY/92m+dLGDZpPKHG2zvKNCxvhHRLToozoA7rhwB+QXlaFUY9vPIE++u0KlLk6vGhfZGbthgW3NO41kDaBa92WmeYrMmqYEhRrHvZ3r6Ap4AH7GN9OogeHUhsg6h2X7e+FyWLCd3x9UIhGeY0D4jLrapj7x6CTx9Yw1/KdjEcCWy5zGSr7W+qVi3DsVv0H6bnIEVcfbve8V0C8sA6Lb4JQxr38hzb8EDfsMMTGzGHhDi0He6ftR56p/XBu2wZN/+yr3rAfvHKYuNXEBx80Lv4TAoxrGfqQt/L4RIRZSQKBgQDNgqQnThCUMHAwE+kC22fhiuvl3Nmyx8QPxbTBJu1gGergKCu0m9uNX5hQdN+9/ieCzTakth0oETx3Vz+DzI0FjVC9yr30kiNeaFMToBCEenYcGGCC3U2Vz6ppquiZAvVm025MvLHiAH3n/PS3yXEzoYSHUDNhi/+ddNoP4I+ElwKBgQDyG59Crup1amLXBc/Z0YTcGApnU9dbCTinmdpCRQVQczRj29tVOu05QSk1wQ0d2+V12Ydg2VD97rAx9T0mFFHq0QS7/I9fNCajerR3R0f7wsyrHZovFN8mTEh8fxl8pO3i0ScH7L1Z0wQbuxAzgehqmEoaj7ULkNfU0tgUhNm8uwKBgQCOhyhpyg5deCqWbXiQ7rHhDoQEa2LgRwOHHMr7mo/Osqrew31sSRu/tKjiQ+xYzEeCw+g927/k5e9VpUD7m4XCb/urZUzQrfmxpBDZ740FFBmN6qokmG8Sk2/Q0SN320FvCvvYZJXJ9CVeG2VtgVvtPvu3DLxVzs582WnS0R84CQKBgAnllSoNqmnoUmgFxcxaozq4BNzacYg4JUe8o05oMeJrAy4904Z1ZTMc9clLvfSFg6jAnqcX2xa2Rh+Urc47sGmP58ijd1zl7dpq7qudj1S8Ts+D40SfbsvK/H+SVoFg4JSQBi9tvwPH+3gCupPQcKbC2OyjCTySzC/X+ptEHv53AoGAJy5KhFBEQNTD3M942QJfTVdZL6i5c54hsaszblhcA+4yG2l3QEwad9HjofzfILhCsh8ZZg2o7yODBkgIwGYROX/GxbAhvAsRrHgCEUshV5hM6Ei3dFRyZt+d7AYqjWDs1d38XX0pFbMrSI0P/LlMpiVz64aIiDg6QsnIQL6FTVs="}}
    INJECTOR_PORT = 7070
    INJECTOR_CONF_FILE_CONTENT = "open-file-limit = 32768\n"
    TEST_PAGE_BODY="<html><body>TESTPAGE</body></html>\n"
    TEST_HTTP_SERVER_PORT = 7080
    RESPONSE_LENGTH = 20

    REPO_FOLDER_NAME = "repos"
    FIRST_CLIENT = { "name": "client1",
                         "port": 8081}
    FIRST_CLIENT_CONF_FILE_CONTENT = "open-file-limit = 4096\n"

    
