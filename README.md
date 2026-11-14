# ESP32 MQTT Accelerometer + ML

The project reads an ADXL345 accelerometer with an ESP32, using a self-made Edge-Impulse ML model the accelerometer data its classified and then published through MQTT. It uses PlatformIO with different setups configured in `platformio.ini` so the program can be used for normal/moked usage and data gathering/inference.

## Requirements

- PlatformIO Core or the PlatformIO IDE extension
- An ESP32 board
- An ADXL345 connected over I²C (SDA: GPIO 21, SCL: GPIO 22)

## How to gather data for Edge Impulse:
Install the cli to manage Edge Impulse through the terminal
```bash
npm install -g edge-impulse-cli
```
then run
```bash
edge-impulse-data-forwarder
```
Configure the Edge Impulse project so it properly recieves the information and label each received data to create the model.

## Wokwi environment

The `wokwi` environment uses the Wokwi guest Wi-Fi and the MQTT host configured in `src/main.cpp`. Build it with:

```bash
pio run -e wokwi
```

To upload to a connected board, use `pio run -e wokwi -t upload`. The Wokwi simulator can use the same project configuration through `wokwi.toml`.

## Real hardware environment

Create a local secrets file from the committed example:

```bash
cp include/secrets.example.h include/secrets.h
```

Edit `include/secrets.h` and set `WIFI_SSID` and `WIFI_PASSWORD`. Set `MQTT_USER` and `MQTT_PASSWORD` if the broker requires authentication; otherwise, leave them empty. This file is ignored by Git and must not be committed.

The broker address and port for real hardware are currently configured in `src/main.cpp`. Update them there if your broker uses different values. Build the real environment with:

```bash
pio run -e real
```

To upload it to the board, use `pio run -e real -t upload`. The `real` environment defines `ENV_REAL`, which makes `main.cpp` include the local `secrets.h` file.

## Serial monitor

The configured baud rate is 115200. For example:

```bash
pio device monitor -b 115200
```

## Credentials and firmware

Ignoring `include/secrets.h` prevents accidental commits of Wi-Fi credentials, but credentials included in a compiled firmware are not encrypted and may be recoverable from the device or firmware binary. Use credentials dedicated to this device where possible.
