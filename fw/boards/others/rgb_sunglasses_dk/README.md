# Board files

This directory contains all board files for the RGB Sunglasses Devkit, based around the NRF5340 SoC.

## SoC Information

The NRF5340 SoC contains two cores:

- The "app" core (cpuapp)
- The "net" core (cpunet)

The "board" contains configuration information for both the app and net cores.

Additionally, the "app" core contains an ARM TrustZone (TZ) processor. This processor has its own firmware image, typically based around the Trusted Firmware-M (TF-M) project.

## Variants

The board has two "variants":

- The "default" variant (no suffix): includes support for TF-M
- The "non-secure" variant ("ns" suffix): does not include support for TF-M

Due to flash limitations, the devkit is typically built in the non-secure variant.

## Trusted Firmware-M

https://docs.zephyrproject.org/latest/services/tfm/overview.html

TF-M complicates the boot process. We won't be using it for now.

