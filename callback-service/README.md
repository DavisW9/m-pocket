# Optional QR callback service

The device beta uses the hosted callback URL in `src/service.py`. Feed requests and posting go directly to the Mastodon server.

This directory contains the Cloudflare Worker source. It stores short-lived pairing state and authorization codes; passwords, access tokens, client secrets, and PKCE verifiers remain between the client and Mastodon. Sessions expire after ten minutes. Manual access-token login works without this service.

To run its protocol tests (no Cloudflare account needed):

```sh
node --test worker.test.mjs
```

To host your own copy, install the locked dependencies with `npm ci`, sign into your own Cloudflare account with `npx wrangler login`, then run `npx wrangler deploy`. The published configuration deliberately contains no account ID or credentials. Change `RELAY` in `src/service.py` to your deployment origin and rebuild the client. Registration, redirect URI, and PKCE must use the same origin. Do not include tokens or local `.wrangler` data in commits.

The hosted service is intended for beta use; availability is not guaranteed. A public installation should review its Cloudflare limits and abuse controls before wider distribution.
