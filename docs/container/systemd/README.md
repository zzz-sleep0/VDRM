# VDRM container systemd units

Split responsibilities:

- `dev-input.mount` / `dev-snd.mount` — the two private tmpfs mounts;
- `vdrm-input.service` — **input only** (input nodes, udev data/tags, symlinks);
- `vdrm-devices.service` — tty/DRM/audio nodes and DRM seat udev data.

Install:

```sh
install -m 755 tools/vdrm-container-setup.sh /usr/local/sbin/vdrm-container-setup
install -m 644 docs/container/systemd/dev-input.mount /etc/systemd/system/
install -m 644 docs/container/systemd/dev-snd.mount /etc/systemd/system/
install -m 644 docs/container/systemd/vdrm-input.service /etc/systemd/system/
install -m 644 docs/container/systemd/vdrm-devices.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable dev-input.mount dev-snd.mount vdrm-input.service vdrm-devices.service
```

Do not run `mount -t tmpfs` from the old service anymore. The units own the
mounts; the script only verifies they are tmpfs before creating nodes.
