(setq ouinet-build-dir (getenv "OUINET_BUILD_DIR"))
(cd ouinet-build-dir)
(setq injector-i2p-public-id "iWq6NotH~nGBTymTOC4G07IV1qO~I1VoOL5NwS2-cRzkojnfBlrfkrdNuRV9rnKxcRtoJjnwd02QFtdt~d~FL9MhNC4kYqYwc1zqfgqDMAAUVVfZjK1Sw9-Z0tixks5m81xZumhExGjl14kLkkC1~CZH3DsXVhHuXtiZlik9n8oxW5Z1soxgLdZm1Y2PB4kEybcQkcJtuG0JsmeAxaHI6B9pr\0Ta3Voa1f-oq2R~8wi74aPAdBpMKKvQ4S22T1h5Hn0mNC8ommzeZXyAV0oC~NV6IoWjhY399YcGeqekrpGQXupDN0GMUWxRRdx-SKsiOAWcJr8v0VsExyX36IaOfZ5wYJczUABU9UP3gG6tzKAMXQ5ClVT9nGM3eUyAG3FOHUkBC~4dhtMsaQt1wjr4PsnON9-RTfaq-0nApQ2FIA0pHt25VwdI1nXvy6daVnhCDyQBK-znaxPx5bHbVRe54vq2XE12WPJFcVwSCrIM-O\bqwIW73h4Pd8H-pj4OEuo9BQAEAAEAAA==")

;;i2p client with known injecter
(gdb (concat "gdb -i=mi -x client --args " ouinet-build-dir "/client --repo repos/client --disable-cache --listen-on-tcp 127.0.0.1:3888 --injector-ep i2p:" injector-i2p-public-id " --log-level DEBUG"))

;;i2p client with i2psnark test cache
(gdb (concat "gdb -i=mi -x client  --args " ouinet-build-dir "/client --repo repos/client --cache-type bep5-http-over-i2p --listen-on-tcp 127.0.0.1:3888 --front-end-ep 127.0.0.1:3889 --log-level DEBUG"))

