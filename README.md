# ESP32 Firmware Collection

This collection groups independent ESP-IDF firmware projects by chip model.

## Directory Layout

```text
esp32-firmware-collection/
├── esp32s3/
│   ├── README.md
│   └── blink/
│       ├── CMakeLists.txt
│       ├── sdkconfig.defaults
│       ├── main/
│       └── components/
└── esp32p4/
    ├── README.md
    └── blink/
        ├── CMakeLists.txt
        ├── sdkconfig.defaults
        ├── main/
        └── components/
```

The chip directories are only categories. Run ESP-IDF commands from inside a
concrete project directory such as `esp32s3/blink`.

## Build

For ESP32-S3 blink:

```sh
cd esp32s3/blink
idf.py set-target esp32s3
idf.py build
```

For ESP32-P4 blink:

```sh
cd esp32p4/blink
idf.py set-target esp32p4
idf.py build
```

Add new projects as sibling directories under the matching chip directory.
