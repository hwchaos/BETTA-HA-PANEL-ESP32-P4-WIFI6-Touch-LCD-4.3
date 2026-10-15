# Wake word models

`okay_nabu.tflite` / `okay_nabu.json` are the microWakeWord v2 "Okay Nabu"
model by Kevin Ahrendt from
[esphome/micro-wake-word-models](https://github.com/esphome/micro-wake-word-models)
(`models/v2`), licensed under the Apache License 2.0. The model is embedded
in the firmware as the built-in wake word (see `assist/panel_wake.cc`);
further models are installed at runtime from the URL of their manifest.
