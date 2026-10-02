# NL CommandCenter V4 License

## Client behavior

- Product id: `nl_command_center_v4`
- License server: `http://47.101.131.245`
- Verify API: `POST /v1/verify`
- Offline grace period: 90 days after a successful online verification
- Local cache: `%LOCALAPPDATA%\NL_CommandCenter_v4_qt\license_cache.json`
- Server response verification: built-in Ed25519 public key check over `product`, `device_id`, and `key_hash`
- Local cache protection: HMAC-signed cache payload

The app shows the license dialog only when no usable cached license exists, the cached license has exceeded the offline grace period and cannot be refreshed online, or the server explicitly rejects the license. After successful activation, normal startup enters the GUI directly.

## Create a customer license

Run from PowerShell. Keep `X-Admin-Token` private; only send the returned `key` to the customer. Store the admin token in a local environment variable or a private password manager, not in this repository.

```powershell
$env:LICENSE_ADMIN_TOKEN = "<your-admin-token>"
curl.exe -X POST http://47.101.131.245/admin/licenses `
  -H "Content-Type: application/json" `
  -H "X-Admin-Token: $env:LICENSE_ADMIN_TOKEN" `
  -d "{\"product\":\"nl_command_center_v4\",\"max_devices\":1,\"days\":365,\"note\":\"客户A\"}"
```

## Notes

- First activation must be online.
- HTTP 403/404 and other explicit server rejections do not use offline grace.
- Network timeout/server outage can use cached offline grace when the same license key was verified successfully before.
- The Qt client pins the server Ed25519 public key and rejects unsigned, tampered, replayed-for-another-device, or replayed-for-another-key verify responses.
- The offline cache is HMAC-signed. Older unsigned cache files can only prefill the license key; they cannot authorize offline use.
