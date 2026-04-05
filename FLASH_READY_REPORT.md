## Define Constants
The file tank_triage.ino contains multiple explicitly defined constants. PIN_POWER is set to 26 and controls the BC547 power switch. PIN_TDS is set to 32 and controls the TDS sensor analog pin. PIN_TURB is set to 33 and controls the Turbidity sensor analog pin. INTERVAL_MS is set to 3000 and controls the wait time between sensor emission cycles in the loop. WARMUP_MS is set to 200 and controls the delay after enabling the power switch before analog reads occur. N_SAMPLES is set to 10 and controls how many readings are taken and averaged per sensor during a read. BASELINE_N is set to 30 and controls the number of iterations in the startup baseline computation. THETA_TDS is set to 80.0f and controls the acceptable baseline deviation threshold for TDS. THETA_TURB is set to 0.4f and controls the acceptable baseline deviation threshold for turbidity. ADC_COUNTS is set to 4095.0f and controls the division factor for 12-bit ADC voltage conversions. VREF_V is set to 3.3f and controls the reference voltage multiplier used in the ADC conversions.

## GPIO Pin Assignments
The firmware explicitly assigns GPIO 26 to the PIN_POWER switch, GPIO 32 to the PIN_TDS sensor, and GPIO 33 to the PIN_TURB sensor. The CONTEXT.md file explains that GPIO 26 connects to a BC547 NPN power switch, GPIO 32 connects directly to the TDS sensor, and GPIO 33 connects to the Turbidity RKI-5163 sensor via a 10k resistor voltage divider.

## ADC Attenuation Setting
The exact ADC attenuation setting used in the setup function is ADC_11db. None of the provided files contain an explicit statement explaining why this attenuation setting matters for a 3.3V ADC.

## Library Dependencies
The tank_triage.ino file includes Arduino.h, WiFi.h, WiFiMulti.h, ESPmDNS.h, ESPAsyncWebServer.h, and LittleFS.h. The GEMINI.md rules file explicitly states that ESPAsyncWebServer and AsyncTCP are manually installed. The provided documents do not explicitly state whether the remaining included libraries are bundled with the ESP32 Arduino core or if they require manual installation, but they are excluded from the manually installed list.

## Partition Scheme
None of the provided code or documentation files contain an explicit statement specifying the exact partition scheme required for LittleFS to mount correctly.

## Baseline Computation
The baseline computation explicitly takes 30 samples. The loop executes a delay of 500 milliseconds between reading cycles, alongside internal readSensors block delays of 200 milliseconds for warmup and an additional 100 milliseconds for iterating over the 10 inner samples. This sum amounts to 800 milliseconds per baseline iteration, resulting in a total explicit blocking time of 24 seconds. If either the computed TDS or Turbidity baseline value reads below 0.01f, the code explicitly reassigns that specific baseline value to exactly 0.01f.

## Flash Order Constraint
The prompt requests the reason why firmware must be flashed before the LittleFS filesystem. However, CONTEXT.md explicitly states that data/index.html is uploaded to LittleFS "separately before firmware." None of the provided files state a reason for any specific flash order constraint or why the firmware should be flashed before the filesystem.

## WiFi Credential Placeholders
The exact variable placeholders found in the wifiMulti configurations are "YOUR_SSID_1", "YOUR_PASSWORD_1", "YOUR_SSID_2", "YOUR_PASSWORD_2", "YOUR_SSID_3", and "YOUR_PASSWORD_3". The GEMINI.md file explicitly demands that a teammate must never hardcode real values in place of these placeholders.

## API JSON Fields and Units
In the index.html file, the exact JSON field names mapped to the display are tdsppm, turbv, deltatds, deltaturb, anomaly, label, baselinetds, and baselineturb. The tdsppm and baselinetds fields receive the unit "ppm" in the HTML layout. The turbv and baselineturb fields receive the unit "V". The deltatds and deltaturb fields do not have adjacent explicit units in the HTML format, but display derived deviation numbers with a plus or minus sign. The anomaly field uses a boolean to toggle the visibility of the red banner. The label field displays string badges.

## Halting Conditions
There are two conditions in the setup execution that cause a permanent while (true) halt. The first condition triggers when the WiFiMulti connection fails to reach a WL_CONNECTED state within 30000 milliseconds. The second condition triggers when the LittleFS filesystem fails to mount, explicitly evaluated as !LittleFS.begin(true).

## Inconsistencies
There is an inconsistency in numerical precision. The firmware outputs tdsppm formatted to one decimal place inside its JSON payload, but the index.html javascript truncates tdsppm to zero decimal places. Additionally, the firmware outputs turbv to three decimal places, but index.html formats it to two decimal places. There is also an inconsistency in label badge styling. The firmware assigns the label HIGH-TDS, and CONTEXT.md explicitly rules that HIGH-TDS should map to the color blue, but the explicit inline CSS inside index.html maps the lbl-HIGH-TDS class to yellow.
