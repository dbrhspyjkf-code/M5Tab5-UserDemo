# Out-of-tree patches for the xiaozhi voice integration

These changes live in `managed_components/` (git-ignored) and in the
`xiaozhi-esp32` submodule, so they are NOT carried by a normal clone. Re-apply
them after `idf.py reconfigure` re-downloads components or after a fresh
submodule checkout, otherwise the xiaozhi voice assistant will not connect.

## 1. esp-ml307: pin DigiCert Global Root G2 for the tenclass cloud

`api.tenclass.net` serves a DigiCert Global Root G2 chain that the ESP
`esp_crt_bundle` fails to verify ("signature verification failed"). We pin the
root explicitly in the TLS + MQTT clients.

Files (drop-in replacements):
- `esp-ml307_esp_ssl.cc.patched`  -> `managed_components/78__esp-ml307/src/esp/esp_ssl.cc`
- `esp-ml307_esp_mqtt.cc.patched` -> `managed_components/78__esp-ml307/src/esp/esp_mqtt.cc`

```sh
cp patches/esp-ml307_esp_ssl.cc.patched  managed_components/78__esp-ml307/src/esp/esp_ssl.cc
cp patches/esp-ml307_esp_mqtt.cc.patched managed_components/78__esp-ml307/src/esp/esp_mqtt.cc
```

## 2. xiaozhi-esp32 submodule: tolerant version parsing

`Ota::ParseVersion` calls `std::stoi` per dot-segment; a git-describe firmware
version like `V0.4-4-g...` throws and aborts the device. The patch makes it
tolerant. (Also contains the `CONFIG_TAB5_XIAOZHI_OFFLINE_ONLY` guard, now
disabled.)

```sh
cd components/xiaozhi_core/xiaozhi-esp32
git apply ../../../patches/xiaozhi-esp32-local.patch
```

> Note: we also set `CONFIG_APP_PROJECT_VER="99.0.0"` (sdkconfig) so the OTA
> server never offers a "newer" official firmware that would overwrite this
> custom build — that part needs no patch.
