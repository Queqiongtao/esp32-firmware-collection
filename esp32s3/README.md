# ESP32-S3 Projects

This directory contains independent ESP-IDF projects for ESP32-S3.

Run ESP-IDF commands inside a concrete project directory, for example:

```sh
cd blink
idf.py set-target esp32s3
idf.py build
```

Available projects:

- `blink`: minimal starter project.
- `clock`: migrated ESP32-S3 clock firmware project.
- `amoled_subscreen`: Waveshare ESP32-S3-Touch-AMOLED-1.8 USB 副屏工程。
- `amoled_fluid`: Waveshare ESP32-S3-Touch-AMOLED-1.8 AMOLED 流体效果实验工程，硬件复用 `waveshare_amoled_1_8`。
- `waveshare_amoled_1_8`: Waveshare ESP32-S3-Touch-AMOLED-1.8 板级 bring-up 工程。
