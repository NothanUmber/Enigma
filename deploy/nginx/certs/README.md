Place the TLS certificate and key files used by `docker-compose.nginx.yml` in
this directory.

By default nginx expects:

- `fullchain.pem`
- `privkey.pem`

You can override those filenames with:

- `NGINX_ENIGMA_TLS_CERT`
- `NGINX_ENIGMA_TLS_KEY`

If you use Let's Encrypt on the VM, one simple approach is to copy or symlink
the live certificate files into this directory before starting the compose
stack.
