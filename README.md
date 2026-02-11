# SDDC_FX3 Firmware

Cypress FX3 USB controller firmware for the RX888mk2 and related
direct-sampling SDR hardware.

This repository is a fork of the firmware portion of
[ExtIO_sddc](https://github.com/ik1xpv/ExtIO_sddc), stripped down to
support the RX888mk2 (rx888r2) hardware only.

## Building

### Requirements

- Debian or Ubuntu-based Linux distribution
- `arm-none-eabi-gcc` toolchain (tested with 13.2.1)
- `gcc` (host compiler, for the `elf2img` utility)
- `make`

Install the cross-compiler on Debian/Ubuntu:

```
sudo apt install gcc-arm-none-eabi
```

The Cypress FX3 SDK (v1.3.4) is included in the `SDK/` directory.

### Build

```
cd SDDC_FX3
make clean && make all
```

This produces `SDDC_FX3.img`, which can be flashed to the device.

## Testing

Hardware tests require an RX888mk2 connected via USB and `libusb-1.0-0-dev`:

```
sudo apt install libusb-1.0-0-dev
git submodule update --init
cd tests && make
./fw_test.sh --firmware ../SDDC_FX3/SDDC_FX3.img
```

This builds `fx3_cmd` (vendor command exerciser) and `rx888_stream`
(from the [rx888_tools](https://github.com/ringof/rx888_tools) submodule),
uploads the firmware, and runs an automated test suite.

## Repository Layout

```
SDDC_FX3/           Firmware source code
  driver/           IC drivers (Si5351 clock gen, R82xx tuner)
  radio/            Radio hardware support (rx888r2)
SDK/                Cypress FX3 SDK 1.3.4 (headers, libraries, build system)
tests/              Hardware test tools
  rx888_tools/      Submodule: USB streamer and firmware uploader
docs/               Project documentation and analysis
```

## License

MIT License — see [LICENSE.txt](LICENSE.txt).

The Cypress FX3 SDK in `SDK/` is covered by the Cypress Software License
Agreement (included in `SDK/license/license.txt`).

## Acknowledgments

This project is derived from
[ExtIO_sddc](https://github.com/ik1xpv/ExtIO_sddc) by Oscar Steila
(IK1XPV). Thanks to all who contributed to the original project:

- Oscar Steila (IK1XPV) — original author
- Howard Su
- Franco Venturi
- Hayati Ayguen
- Ruslan Migirov
- Vladisslav2011
- Phil Ashby (phlash)
- Alberto di Bene (I2PHD)
- Mario Taeubel
