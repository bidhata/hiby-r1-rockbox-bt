# HiBy R1 Baseline Provenance

This baseline corresponds to the binary currently running on-device when this repo was created.

- Device path: `/usr/data/mnt/sd_0/.rockbox/rockbox.r1`
- Binary SHA-256: `ada98d8ac6f6ddef7845619e1e5189e829ba92fb1003031ffa0c609d1567ca17`
- Embedded build signature: `dd21a1d1d9M-260307`

Source correlation:

- Upstream base commit: `dd21a1d1d9ae4f314eb16d4813414ee72e2aa0de`
- Baseline patch commit (reconstructed dirty tree): `6af68b9dc7373a02d1c55d169780f35f01d4c011`
- Reviewable baseline series branch head: `2c1171f0b16e55c1ebb63b69b02d5ae5ba6520bb`
- Equivalent resulting tree hash (for both commits above): `e4a12980402ee633f59b6c272821b567d06a4f65`

Patch files in this repo (apply in lexicographic order):

1. `patches/0001-hibyr1-add-bluetooth-UI-menu-integration.patch`
2. `patches/0002-hibyr1-add-bluealsa-pcm-backend-and-routing-hooks.patch`
3. `patches/0003-hibyr1-keep-usb-adb-active-and-add-hosted-runtime-gl.patch`
4. `patches/0004-hosted-add-devinput-trace-hooks-for-on-device-debugg.patch`
5. `patches/0005-hibyr1-preserve-EC2-baseline-auxiliary-source-snapsh.patch`

This series is the initial baseline for reproducible versioning in CI.
